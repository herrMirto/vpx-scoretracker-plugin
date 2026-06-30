// license:GPLv3+

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ScoreTracker.h"
#include "ScoresFileWriter.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>

#include "plugins/LoggingPlugin.h"
#include "mongoose/mongoose.h"

namespace ScoreTrackerPlugin
{

// Game-over confirmation debounce. A "no ball in play" flag (e.g. Stern SAM
// 0x02110904 bit 0x40) is ALSO set between balls during the end-of-ball bonus /
// ball search, which can exceed a couple of seconds (Metallica). A pending
// game-over is cancelled the instant a ball relaunches, so this only needs to
// exceed the longest between-ball pause to avoid finalizing the game mid-play.
static constexpr int kGameOverConfirmSeconds = 25;
// Ignore "games" shorter than this. Resuming a table reloads the previous
// game's score from NVRAM, which can briefly look like a live game; a real game
// always lasts longer.
static constexpr int kMinGameDurationSeconds = 30;

LPI_USE_CPP();

extern const MsgPluginAPI* msgApi;

void WebEventHandler(struct mg_connection* c, int ev, void* ev_data)
{
   if (c->mgr == nullptr || c->mgr->userdata == nullptr)
      return;
   ScoreTracker* tracker = static_cast<ScoreTracker*>(c->mgr->userdata);
   
   if (ev == MG_EV_HTTP_MSG)
   {
      struct mg_http_message* hm = static_cast<struct mg_http_message*>(ev_data);
      if (hm->uri.len == 3 && strncmp(hm->uri.buf, "/ws", 3) == 0)
      {
         if (!tracker->m_enableWebSocket)
         {
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
               "%s", "{\"error\":\"WebSocket output is disabled\"}");
            return;
         }
         mg_ws_upgrade(c, hm, nullptr);
      }
      else
      {
         mg_http_reply(c, 200, "Content-Type: text/plain\r\n",
            "ScoreTracker server is running. WebSocket: /ws.");
      }
   }
   else if (ev == MG_EV_WS_OPEN)
   {
      std::lock_guard<std::mutex> lock(tracker->m_clientsMutex);
      tracker->m_clients.push_back(c);
      
      // Send initial state immediately to new client
      std::lock_guard<std::mutex> stateLock(tracker->m_stateMutex);
      if (!tracker->m_lastJsonState.empty())
      {
         mg_ws_send(c, tracker->m_lastJsonState.c_str(), tracker->m_lastJsonState.length(), WEBSOCKET_OP_TEXT);
      }
   }
   else if (ev == MG_EV_CLOSE)
   {
      std::lock_guard<std::mutex> lock(tracker->m_clientsMutex);
      auto it = std::find(tracker->m_clients.begin(), tracker->m_clients.end(), c);
      if (it != tracker->m_clients.end())
      {
         tracker->m_clients.erase(it);
      }
   }
}

ScoreTracker::ScoreTracker(const MsgPluginAPI* api, unsigned int endpointId)
   : m_msgApi(api)
   , m_endpointId(endpointId)
{
}

ScoreTracker::~ScoreTracker()
{
   Stop();
}

ScoreTracker::MapStatus ScoreTracker::ProbeMap(const std::string& gameId, const std::string& mapsPath, std::string& detail)
{
   detail.clear();
   if (gameId.empty())
      return MapStatus::NotFound;

   const std::filesystem::path mapsRoot(mapsPath);
   const std::filesystem::path indexPath = mapsRoot / "index.json";
   std::ifstream indexFile(indexPath);
   if (!indexFile.is_open())
   {
      detail = "could not open " + indexPath.string();
      return MapStatus::Error;
   }

   try
   {
      nlohmann::json indexJson;
      indexFile >> indexJson;
      if (!indexJson.contains(gameId))
      {
         detail = "ROM is not listed in index.json";
         return MapStatus::NotFound;
      }
      if (!indexJson[gameId].is_string())
      {
         detail = "index.json entry is not a path string";
         return MapStatus::Error;
      }
      const std::filesystem::path mapPath = mapsRoot / indexJson[gameId].get<std::string>();
      if (!std::filesystem::is_regular_file(mapPath))
      {
         detail = "declared map file is missing: " + mapPath.string();
         return MapStatus::Error;
      }
      detail = mapPath.string();
      return MapStatus::Available;
   }
   catch (const std::exception& e)
   {
      detail = "failed to parse index.json: " + std::string(e.what());
      return MapStatus::Error;
   }
}

int ScoreTracker::ParseIntVal(const nlohmann::json& val, int defaultVal)
{
   if (val.is_null())
      return defaultVal;
   if (val.is_number())
      return val.get<int>();
   if (val.is_string())
   {
      std::string s = val.get<std::string>();
      if (s.empty())
         return defaultVal;
      if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
      {
         try
         {
            return std::stoi(s, nullptr, 16);
         }
         catch (...)
         {
            return defaultVal;
         }
      }
      try
      {
         return std::stoi(s);
      }
      catch (...)
      {
         return defaultVal;
      }
   }
   if (val.is_boolean())
      return val.get<bool>() ? 1 : 0;
   return defaultVal;
}

std::vector<int> ScoreTracker::ResolveAddresses(const nlohmann::json& desc)
{
   std::vector<int> out;
   if (desc.contains("offsets") && desc["offsets"].is_array())
   {
      for (const auto& item : desc["offsets"])
      {
         out.push_back(ParseIntVal(item));
      }
      return out;
   }
   
   if (desc.contains("start"))
   {
      int start = ParseIntVal(desc["start"]);
      if (desc.contains("length"))
      {
         int length = ParseIntVal(desc["length"]);
         if (length > 0)
         {
            for (int i = 0; i < length; ++i)
            {
               out.push_back(start + i);
            }
         }
      }
      else if (desc.contains("end"))
      {
         int end = ParseIntVal(desc["end"]);
         if (end >= start)
         {
            for (int i = start; i <= end; ++i)
            {
               out.push_back(i);
            }
         }
      }
      else
      {
         out.push_back(start);
      }
   }
   return out;
}

Descriptor ScoreTracker::ParseDescriptor(const nlohmann::json& desc)
{
   Descriptor d;
   bool isLittle = false;
   if (m_platformData.contains("endian") && m_platformData["endian"].is_string())
   {
      std::string endStr = m_platformData["endian"].get<std::string>();
      isLittle = (endStr == "little");
   }
   d.is_little_endian = isLittle;

   if (desc.contains("label"))
      d.label = desc["label"].get<std::string>();

   std::string encStr = "int";
   if (desc.contains("encoding"))
      encStr = desc["encoding"].get<std::string>();
   std::transform(encStr.begin(), encStr.end(), encStr.begin(), ::tolower);
   if (encStr == "bcd")
      d.encoding = EncodingType::Bcd;
   else if (encStr == "bool")
      d.encoding = EncodingType::Bool;
   else
      d.encoding = EncodingType::Int;

   if (desc.contains("nibble"))
   {
      d.nibble = desc["nibble"].get<std::string>();
      std::transform(d.nibble.begin(), d.nibble.end(), d.nibble.begin(), ::tolower);
   }

   if (desc.contains("mask"))
   {
      d.mask = ParseIntVal(desc["mask"]);
      d.has_mask = true;
   }

   if (desc.contains("endian"))
   {
      std::string endStr = desc["endian"].get<std::string>();
      d.is_little_endian = (endStr == "little");
   }

   if (desc.contains("invert"))
      d.invert = desc["invert"].get<bool>();
   if (desc.contains("scale"))
      d.scale = desc["scale"].get<double>();
   if (desc.contains("offset"))
      d.offset = desc["offset"].get<double>();

   d.addresses = ResolveAddresses(desc);

   return d;
}

void ScoreTracker::ResolveDescriptor(Descriptor& d)
{
   d.resolvedAddresses.clear();
   for (int addr : d.addresses)
   {
      ResolvedAddress ra;
      ra.address = addr;
      ra.isRam = false;
      ra.fileOffset = -1;
      ra.nibble = d.nibble.empty() ? "both" : d.nibble;

      std::string regionType = GetAddrRegionType(addr);
      if (regionType == "ram")
      {
         ra.isRam = true;
      }
      else
      {
         if (!m_segments.empty())
         {
            NVRAMSegment* seg = GetSegmentForAddr(addr);
            if (seg == nullptr)
            {
               if (m_lowRamMirrorLimit > 0 && addr >= 0 && addr < m_lowRamMirrorLimit)
               {
                  ra.fileOffset = addr;
               }
               else
               {
                  ra.fileOffset = -1;
               }
            }
            else
            {
               ra.fileOffset = seg->file_base + (addr - seg->address);
               if (d.nibble.empty())
               {
                  ra.nibble = seg->nibble;
                  std::transform(ra.nibble.begin(), ra.nibble.end(), ra.nibble.begin(), ::tolower);
               }
            }
         }
         else
         {
            ra.fileOffset = addr;
         }
      }
      d.resolvedAddresses.push_back(ra);
   }

   if (!d.resolvedAddresses.empty())
   {
      d.resolvedNibbleMode = d.resolvedAddresses.back().nibble;
   }
   else
   {
      d.resolvedNibbleMode = "both";
   }
}

bool ScoreTracker::LoadMap(const std::string& gameId)
{
   std::filesystem::path mapsRoot(m_mapsPath);
   std::filesystem::path indexPath = mapsRoot / "index.json";
   
   std::ifstream indexFile(indexPath);
   if (!indexFile.is_open())
   {
      std::cerr << "[ScoreTracker] Could not open index.json at " << indexPath.string() << std::endl;
      return false;
   }
   
   nlohmann::json indexJson;
   try
   {
      indexFile >> indexJson;
   }
   catch (const std::exception& e)
   {
      std::cerr << "[ScoreTracker] JSON parse error in index.json: " << e.what() << std::endl;
      return false;
   }
   
   if (!indexJson.contains(gameId))
   {
      std::cerr << "[ScoreTracker] ROM " << gameId << " not found in index.json" << std::endl;
      return false;
   }
   
   std::string mapRelPath = indexJson[gameId].get<std::string>();
   std::filesystem::path mapPath = mapsRoot / mapRelPath;
   
   std::ifstream mapFile(mapPath);
   if (!mapFile.is_open())
   {
      std::cerr << "[ScoreTracker] Could not open map file at " << mapPath.string() << std::endl;
      return false;
   }
   
   try
   {
      mapFile >> m_mapData;
   }
   catch (const std::exception& e)
   {
      std::cerr << "[ScoreTracker] JSON parse error in map file " << mapPath.string() << ": " << e.what() << std::endl;
      return false;
   }
   
   // Load platform data if referenced
   if (m_mapData.contains("_metadata") && m_mapData["_metadata"].contains("platform"))
   {
      std::string platform = m_mapData["_metadata"]["platform"].get<std::string>();
      std::filesystem::path platformPath = mapsRoot / "platforms" / (platform + ".json");
      std::ifstream platformFile(platformPath);
      if (platformFile.is_open())
      {
         try
         {
            platformFile >> m_platformData;
         }
         catch (const std::exception& e)
         {
            std::cerr << "[ScoreTracker] JSON parse error in platform file " << platformPath.string() << ": " << e.what() << std::endl;
         }
      }
   }
   
   // Parse layout and build segments
   m_layout.clear();
   m_segments.clear();
   m_lowRamMirrorLimit = 0;
   
   if (m_platformData.contains("memory_layout") && m_platformData["memory_layout"].is_array())
   {
      int nvramFileBase = 0;
      int ramZeroSize = 0;
      int firstNvramAddr = -1;
      
      for (const auto& entryJson : m_platformData["memory_layout"])
      {
         MemoryLayoutEntry entry;
         if (entryJson.contains("address"))
            entry.address = ParseIntVal(entryJson["address"]);
         if (entryJson.contains("size"))
            entry.size = ParseIntVal(entryJson["size"]);
         if (entryJson.contains("type"))
            entry.type = entryJson["type"].get<std::string>();
         if (entryJson.contains("nibble"))
            entry.nibble = entryJson["nibble"].get<std::string>();
         m_layout.push_back(entry);
         
         std::string typeLower = entry.type;
         std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);
         
         if (typeLower == "nvram")
         {
            NVRAMSegment seg;
            seg.address = entry.address;
            seg.size = entry.size;
            seg.nibble = entry.nibble.empty() ? "both" : entry.nibble;
            std::transform(seg.nibble.begin(), seg.nibble.end(), seg.nibble.begin(), ::tolower);
            seg.file_base = nvramFileBase;
            m_segments.push_back(seg);
            nvramFileBase += entry.size;
            
            if (firstNvramAddr == -1 || entry.address < firstNvramAddr)
            {
               firstNvramAddr = entry.address;
            }
         }
         else if (typeLower == "ram" && entry.address == 0)
         {
            ramZeroSize = (std::max)(ramZeroSize, entry.size);
         }
      }
      
      std::sort(m_segments.begin(), m_segments.end(), [](const NVRAMSegment& a, const NVRAMSegment& b) {
         return a.address < b.address;
      });
      
      if (firstNvramAddr > 0 && ramZeroSize >= firstNvramAddr)
      {
         m_lowRamMirrorLimit = firstNvramAddr;
      }
   }
   
   // Parse game_state descriptors
   m_scoresDesc.clear();
   m_gameStateDescriptors.clear();
   m_hasRamFields = false;
   
   if (m_mapData.contains("game_state"))
   {
      const auto& gs = m_mapData["game_state"];
      for (auto it = gs.begin(); it != gs.end(); ++it)
      {
         std::string key = it.key();
         if (key == "scores" && it.value().is_array())
         {
            for (const auto& scoreJson : it.value())
            {
               Descriptor d = ParseDescriptor(scoreJson);
               ResolveDescriptor(d);
               for (const auto& ra : d.resolvedAddresses)
               {
                  if (ra.isRam)
                     m_hasRamFields = true;
               }
               m_scoresDesc.push_back(d);
            }
         }
         else if (it.value().is_object())
         {
            Descriptor d = ParseDescriptor(it.value());
            ResolveDescriptor(d);
            for (const auto& ra : d.resolvedAddresses)
            {
               if (ra.isRam)
                  m_hasRamFields = true;
            }
            m_gameStateDescriptors.push_back({key, d});
         }
      }
   }
   
   std::cout << "[ScoreTracker] ROM map successfully loaded: " << gameId << std::endl;
   return true;
}

std::string ScoreTracker::GetAddrRegionType(int addr)
{
   for (const auto& e : m_layout)
   {
      if (e.address <= addr && addr < (e.address + e.size))
      {
         std::string t = e.type;
         std::transform(t.begin(), t.end(), t.begin(), ::tolower);
         return t;
      }
   }
   return "";
}

NVRAMSegment* ScoreTracker::GetSegmentForAddr(int addr)
{
   for (auto& seg : m_segments)
   {
      if (seg.address <= addr && addr < (seg.address + seg.size))
      {
         return &seg;
      }
   }
   return nullptr;
}

bool ScoreTracker::ReadUnits(const std::vector<uint8_t>& nvram, const Descriptor& desc, std::vector<uint8_t>& units)
{
   units.clear();
   for (const auto& ra : desc.resolvedAddresses)
   {
      if (ra.isRam)
      {
         uint8_t val = 0;
         if (PinmameReadMainCPUByte(static_cast<uint32_t>(ra.address), &val) == 0)
         {
            return false;
         }
         units.push_back(desc.has_mask ? (val & desc.mask) : val);
      }
      else
      {
         if (ra.fileOffset < 0 || ra.fileOffset >= static_cast<int>(nvram.size()))
         {
            return false;
         }
         uint8_t raw_b = nvram[ra.fileOffset];
         uint8_t unit = 0;
         if (ra.nibble == "low")
         {
            unit = raw_b & 0x0F;
         }
         else if (ra.nibble == "high")
         {
            unit = (raw_b >> 4) & 0x0F;
         }
         else
         {
            unit = raw_b;
         }
         units.push_back(desc.has_mask ? (unit & desc.mask) : unit);
      }
   }
   return true;
}

int64_t ScoreTracker::DecodeInt(const std::vector<uint8_t>& units, bool is_little_endian)
{
   int64_t value = 0;
   if (is_little_endian)
   {
      for (auto it = units.rbegin(); it != units.rend(); ++it)
      {
         value = (value << 8) | (*it & 0xFF);
      }
   }
   else
   {
      for (uint8_t b : units)
      {
         value = (value << 8) | (b & 0xFF);
      }
   }
   return value;
}

int64_t ScoreTracker::DecodeBCD(const std::vector<uint8_t>& units, const std::string& nibbleMode, bool is_little_endian)
{
   int64_t value = 0;
   if (is_little_endian)
   {
      if (nibbleMode == "low" || nibbleMode == "high")
      {
         for (auto it = units.rbegin(); it != units.rend(); ++it)
         {
            uint8_t n = *it;
            value = (value * 10) + ((n <= 9) ? n : 0);
         }
      }
      else
      {
         for (auto it = units.rbegin(); it != units.rend(); ++it)
         {
            uint8_t b = *it;
            int hi = (b >> 4) & 0x0F;
            int lo = b & 0x0F;
            value = (value * 10) + ((hi <= 9) ? hi : 0);
            value = (value * 10) + ((lo <= 9) ? lo : 0);
         }
      }
   }
   else
   {
      if (nibbleMode == "low" || nibbleMode == "high")
      {
         for (uint8_t n : units)
         {
            value = (value * 10) + ((n <= 9) ? n : 0);
         }
      }
      else
      {
         for (uint8_t b : units)
         {
            int hi = (b >> 4) & 0x0F;
            int lo = b & 0x0F;
            value = (value * 10) + ((hi <= 9) ? hi : 0);
            value = (value * 10) + ((lo <= 9) ? lo : 0);
         }
      }
   }
   return value;
}

int64_t ScoreTracker::Decode(const std::vector<uint8_t>& nvram, const Descriptor& desc)
{
   if (!ReadUnits(nvram, desc, m_decodeUnits))
   {
      return 0;
   }
   
   int64_t value = 0;
   if (desc.encoding == EncodingType::Int)
   {
      value = DecodeInt(m_decodeUnits, desc.is_little_endian);
   }
   else if (desc.encoding == EncodingType::Bcd)
   {
      value = DecodeBCD(m_decodeUnits, desc.resolvedNibbleMode, desc.is_little_endian);
   }
   else if (desc.encoding == EncodingType::Bool)
   {
      value = DecodeInt(m_decodeUnits, desc.is_little_endian) != 0;
      if (desc.invert)
      {
         value = !value;
      }
   }
   
   return static_cast<int64_t>(value * desc.scale + desc.offset);
}

bool ScoreTracker::Start(const std::string& gameId, const std::string& mapsPath, int port, int pollIntervalMs,
   bool enableWebSocket, const std::string& tablePath)
{
   m_gameId = gameId;
   m_mapsPath = mapsPath;
   m_tablePath = tablePath;
   m_enableWebSocket = enableWebSocket;
   m_pollIntervalMs = std::clamp(pollIntervalMs, 50, 5000);
   {
      std::lock_guard<std::mutex> lock(m_nvramMutex);
      m_lastNvram.clear();
   }
   
   m_sessionStartRealTime = std::chrono::steady_clock::now();
   m_gameOverLast = false;
   m_gameOverPending = false;
   m_summarySent = false;
   m_hasBeenInPlay = false;
   m_highestScores.clear();
   m_maxGameStateValues.clear();
   m_monitoringStarted = false;
   
   if (!LoadMap(gameId))
   {
      std::cerr << "[ScoreTracker] Failed to load JSON map. Disabling monitoring." << std::endl;
      return false;
   }

   m_monitoringStarted = true;

   if (m_enableWebSocket)
   {
      StartWebServer(port);
   }
   else
   {
      std::cout << "[ScoreTracker] WebSocket output disabled; no local web server started" << std::endl;
      LPI_LOGI_CPP("[INFO] - WebSocket output disabled; no local web server started");
   }

   m_running = true;
   m_thread = std::thread(&ScoreTracker::Loop, this);
   return true;
}

void ScoreTracker::Stop()
{
   m_running = false;
   if (m_thread.joinable())
   {
      m_thread.join();
   }
   
   // If VPX exits before a mapped game-over is confirmed, persist the played
   // session as a fallback instead of only broadcasting an ephemeral summary.
   if (m_monitoringStarted && !m_summarySent && !m_gameId.empty())
   {
      std::vector<int64_t> finalScores;
      try
      {
         if (!m_lastJsonState.empty())
         {
            auto j = nlohmann::json::parse(m_lastJsonState);
            if (j.contains("scores") && j["scores"].is_array())
            {
               for (const auto& s : j["scores"])
               {
                  finalScores.push_back(s.get<int64_t>());
               }
            }
         }
      }
      catch (...) {}
      
      const bool hasScore = std::any_of(finalScores.begin(), finalScores.end(),
         [](int64_t value) { return value > 0; });
      const bool writeExitFallback = m_hasBeenInPlay && hasScore;
      if (writeExitFallback)
      {
         LPI_LOGI_CPP("[INFO] - No confirmed game-over before VPX exit; writing the active NVRAM session as an exit fallback");
      }
      SendSummaryPayload(finalScores, writeExitFallback);
      m_summarySent = true;
   }
   
   StopWebServer();
   m_monitoringStarted = false;
}

void ScoreTracker::Loop()
{
   bool hasLastState = false;
   
   m_loopNvram.clear();
   m_loopNvramBuffer.clear();

   int lastCurrentPlayer = -1;
   int lastCurrentBall = -1;
   bool lastIsGameOver = false;
   std::vector<int64_t> lastPlayerScores;
   std::unordered_map<std::string, int64_t> lastDecodedValues;

   while (m_running)
   {
      if (!PinmameIsRunning())
      {
         std::this_thread::sleep_for(std::chrono::milliseconds(m_pollIntervalMs));
         continue;
      }
      
      int nvramSize = PinmameGetMaxNVRAM();
      bool nvramChanged = false;
      
      if (nvramSize > 0)
      {
         if (m_loopNvramBuffer.size() != static_cast<size_t>(nvramSize))
         {
            m_loopNvramBuffer.resize(nvramSize);
         }
         
         int count = PinmameGetNVRAM(m_loopNvramBuffer.data());
         if (count < 0)
            count = 0;
         
         bool sizeChanged = m_loopNvram.size() != static_cast<size_t>(count);
         bool contentChanged = false;
         if (!sizeChanged)
         {
            for (int i = 0; i < count; ++i)
            {
               if (m_loopNvram[i] != m_loopNvramBuffer[i].currStat)
               {
                  contentChanged = true;
                  break;
               }
            }
         }
         
         if (sizeChanged || contentChanged)
         {
            m_loopNvram.resize(count);
            for (int i = 0; i < count; ++i)
            {
               m_loopNvram[i] = m_loopNvramBuffer[i].currStat;
            }
            {
               std::lock_guard<std::mutex> lock(m_nvramMutex);
               m_lastNvram = m_loopNvram;
            }
            nvramChanged = true;
         }
      }
      
      // If NVRAM has not changed and there are no RAM-mapped fields,
      // we can skip decoding and serialization completely.
      if (!nvramChanged && !m_hasRamFields && hasLastState)
      {
         if (m_gameOverPending && !m_gameOverLast)
         {
            const bool gameOverConfirmed = std::chrono::steady_clock::now() - m_gameOverSince >= std::chrono::seconds(kGameOverConfirmSeconds);
            if (gameOverConfirmed)
            {
               std::string tableName = m_tablePath.empty() ? "unknown" : std::filesystem::path(m_tablePath).filename().string();
               LPI_LOGI_CPP(std::string("[INFO] - Game play for table ") + tableName + " with rom " + m_gameId + " is over");
               std::cout << "[INFO] - Game play for table " << tableName << " with rom " << m_gameId << " is over" << std::endl;
               
               if (!m_summarySent)
               {
                  std::vector<int64_t> playerScores;
                  for (const auto& scoreDesc : m_scoresDesc)
                  {
                     playerScores.push_back(Decode(m_loopNvram, scoreDesc));
                  }
                  SendSummaryPayload(playerScores, true);
                  m_summarySent = true;
               }
               m_gameOverLast = true;
               m_hasBeenInPlay = false;
            }
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(m_pollIntervalMs));
         continue;
      }

      std::unordered_map<std::string, int64_t> decodedValues;
      bool valuesChanged = false;
      
      // 1. Decode dynamic descriptors
      for (const auto& [key, desc] : m_gameStateDescriptors)
      {
         int64_t val = Decode(m_loopNvram, desc);
         decodedValues[key] = val;
         if (lastDecodedValues.count(key) == 0 || lastDecodedValues[key] != val)
         {
            valuesChanged = true;
         }
      }
      
      // 2. Resolve default fields for standard API keys
      int currentPlayer = 1;
      if (decodedValues.count("current_player"))
      {
         currentPlayer = static_cast<int>(decodedValues["current_player"]);
      }
      
      int currentBall = 1;
      if (decodedValues.count("current_ball"))
      {
         currentBall = static_cast<int>(decodedValues["current_ball"]);
      }
      
      bool isGameOver = false;
      if (decodedValues.count("game_over"))
      {
         isGameOver = (decodedValues["game_over"] != 0);
      }
      
      // 3. Decode scores
      std::vector<int64_t> playerScores;
      playerScores.reserve(m_scoresDesc.size());
      for (const auto& scoreDesc : m_scoresDesc)
      {
         playerScores.push_back(Decode(m_loopNvram, scoreDesc));
      }
      
      if (playerScores != lastPlayerScores)
      {
         valuesChanged = true;
      }
      if (currentPlayer != lastCurrentPlayer || currentBall != lastCurrentBall || isGameOver != lastIsGameOver)
      {
         valuesChanged = true;
      }
      
      int64_t score = 0;
      if (!playerScores.empty())
      {
         if (currentPlayer >= 1 && currentPlayer <= static_cast<int>(playerScores.size()))
         {
            score = playerScores[currentPlayer - 1];
         }
         else
         {
            score = playerScores[0];
         }
      }
      
      // Raw lifecycle flags can briefly toggle during display and ball-state
      // transitions. Only confirm game-over after it remains asserted for two
      // seconds; a transient must never finalize or reset a session.
      if (!isGameOver)
      {
         m_gameOverPending = false;
         if (!m_hasBeenInPlay)
         {
            m_summarySent = false;
            m_highestScores.clear();
            m_maxGameStateValues.clear();
            m_sessionStartRealTime = std::chrono::steady_clock::now();
         }
         m_gameOverLast = false;
         m_hasBeenInPlay = true;
      }
      else if (!m_gameOverPending)
      {
         m_gameOverPending = true;
         m_gameOverSince = std::chrono::steady_clock::now();
      }

      // 4. Update statistics only after a game has actually entered play.
      // Scores retained in NVRAM during attract mode belong to the previous
      // game and must not become the next session's highest score.
      if (m_hasBeenInPlay)
      {
         if (m_highestScores.size() < playerScores.size())
         {
            m_highestScores.resize(playerScores.size(), 0);
         }
         for (size_t i = 0; i < playerScores.size(); ++i)
         {
            if (playerScores[i] > m_highestScores[i])
            {
               m_highestScores[i] = playerScores[i];
            }
         }
         for (const auto& [key, val] : decodedValues)
         {
            if (!m_maxGameStateValues.count(key) || val > m_maxGameStateValues[key])
            {
               m_maxGameStateValues[key] = val;
            }
         }
      }

      // 5. Handle confirmed game over logging and summary
      const bool gameOverConfirmed = isGameOver && m_gameOverPending
         && std::chrono::steady_clock::now() - m_gameOverSince >= std::chrono::seconds(kGameOverConfirmSeconds);
      if (gameOverConfirmed && m_hasBeenInPlay && !m_gameOverLast)
      {
         std::string tableName = m_tablePath.empty() ? "unknown" : std::filesystem::path(m_tablePath).filename().string();
         LPI_LOGI_CPP(std::string("[INFO] - Game play for table ") + tableName + " with rom " + m_gameId + " is over");
         std::cout << "[INFO] - Game play for table " << tableName << " with rom " << m_gameId << " is over" << std::endl;
         
         if (!m_summarySent)
         {
            SendSummaryPayload(playerScores, true);
            m_summarySent = true;
         }
         m_gameOverLast = true;
         m_hasBeenInPlay = false;
      }
      
      // 6. Populate top level standard payload fields and serialize only if changed
      if (valuesChanged || !hasLastState)
      {
         nlohmann::json state;
         for (const auto& [key, val] : decodedValues)
         {
            bool isBool = false;
            for (const auto& [gsKey, desc] : m_gameStateDescriptors)
            {
               if (gsKey == key && desc.encoding == EncodingType::Bool)
               {
                  isBool = true;
                  break;
               }
            }
            if (isBool)
            {
               state[key] = (val != 0);
            }
            else
            {
               state[key] = val;
            }
         }
         
         state["rom"] = m_gameId;
         state["player"] = "player" + std::to_string(currentPlayer);
         state["score"] = score;
         state["ball"] = currentBall;
         state["is_game_over"] = isGameOver;
         state["scores"] = playerScores;
         
         std::string jsonStr = state.dump();
         
         hasLastState = true;
         lastCurrentPlayer = currentPlayer;
         lastCurrentBall = currentBall;
         lastIsGameOver = isGameOver;
         lastPlayerScores = playerScores;
         lastDecodedValues = decodedValues;
         
         {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_lastJsonState = jsonStr;
         }
         
         // Broadcast to all connected clients
         std::lock_guard<std::mutex> lock(m_clientsMutex);
         for (struct mg_connection* conn : m_clients)
         {
            mg_ws_send(conn, jsonStr.c_str(), jsonStr.length(), WEBSOCKET_OP_TEXT);
         }
      }
      
      std::this_thread::sleep_for(std::chrono::milliseconds(m_pollIntervalMs));
   }
}

void ScoreTracker::StartWebServer(int port)
{
   m_mgr = new mg_mgr();
   mg_mgr_init(m_mgr);
   m_mgr->userdata = this;
   
   std::string listenUrl = "http://0.0.0.0:" + std::to_string(port);
   struct mg_connection* c = mg_http_listen(m_mgr, listenUrl.c_str(), WebEventHandler, nullptr);
   if (c == nullptr)
   {
      std::cerr << "[ScoreTracker] Web server failed to listen on port " << port << std::endl;
      delete m_mgr;
      m_mgr = nullptr;
      return;
   }
   
   std::cout << "[ScoreTracker] WebServer listening on ws://0.0.0.0:" << port << "/ws" << std::endl;
   m_webRunning = true;
   m_webThread = std::thread([this]() {
      while (m_webRunning)
      {
         mg_mgr_poll(m_mgr, 50);
      }
      mg_mgr_free(m_mgr);
      delete m_mgr;
      m_mgr = nullptr;
   });
}

void ScoreTracker::StopWebServer()
{
   m_webRunning = false;
   if (m_webThread.joinable())
   {
      m_webThread.join();
   }
   {
      std::lock_guard<std::mutex> lock(m_clientsMutex);
      m_clients.clear();
   }
}

nlohmann::json ScoreTracker::BuildCompletedGameState() const
{
   nlohmann::json gameState = nlohmann::json::object();
   for (const auto& [key, val] : m_maxGameStateValues)
   {
      if (key == "game_over" || key == "current_player" || key == "current_ball")
         continue;
      if (val == 0)
         continue;
      gameState[key] = val;
   }
   return gameState;
}

void ScoreTracker::SendSummaryPayload(const std::vector<int64_t>& finalScores, bool writeScoresFile)
{
   nlohmann::json summary;
   summary["event"] = "game_summary";
   summary["rom"] = m_gameId;
   summary["table"] = m_tablePath.empty() ? "unknown" : std::filesystem::path(m_tablePath).filename().string();
   
   auto now = std::chrono::steady_clock::now();
   // Real play time: session start -> when game-over was first detected, excluding
   // the game-over confirmation debounce.
   auto endTime = m_gameOverPending ? m_gameOverSince : now;
   double duration = std::chrono::duration_cast<std::chrono::seconds>(endTime - m_sessionStartRealTime).count();
   if (duration < 0) duration = 0;
   summary["duration_seconds"] = duration;

   // Report the peak score per player. The instantaneous score at game-over can
   // lag the final bonus; the peak seen during the session is the robust value.
   std::vector<int64_t> bestScores = finalScores;
   for (size_t i = 0; i < bestScores.size(); ++i)
      if (i < m_highestScores.size() && m_highestScores[i] > bestScores[i])
         bestScores[i] = m_highestScores[i];

   summary["final_scores"] = bestScores;
   summary["highest_scores"] = m_highestScores;

   nlohmann::json maxState;
   for (const auto& [key, val] : m_maxGameStateValues)
   {
      maxState[key] = val;
   }
   summary["max_game_state"] = maxState;

   if (writeScoresFile)
   {
      if (duration < kMinGameDurationSeconds)
      {
         LPI_LOGI_CPP(std::string("[INFO] - ScoreTracker: ignoring game shorter than ")
            + std::to_string(kMinGameDurationSeconds) + "s (was " + std::to_string((int)duration)
            + "s) - not writing scores.json");
      }
      else
      {
         CompletedGameRecord record;
         record.tablePath = m_tablePath;
         record.rom = m_gameId;
         record.scores = bestScores;
         record.gameDuration = static_cast<int64_t>(duration);
         record.gameState = BuildCompletedGameState();
         ScoresFileWriter::AppendCompletedGame(record);
      }
   }

   std::string jsonStr = summary.dump();
   std::cout << "[INFO] - Game summary payload: " << jsonStr << std::endl;
   LPI_LOGI_CPP(std::string("[INFO] - Game summary payload: ") + jsonStr);

   std::lock_guard<std::mutex> lock(m_clientsMutex);
   for (struct mg_connection* conn : m_clients)
   {
      mg_ws_send(conn, jsonStr.c_str(), jsonStr.length(), WEBSOCKET_OP_TEXT);
   }
}

} // namespace ScoreTrackerPlugin
