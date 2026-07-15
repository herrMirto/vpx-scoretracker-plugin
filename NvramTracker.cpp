// license:GPLv3+

#include "NvramTracker.h"
#include "ScoresFileWriter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace ScoreTracker
{

// Game-over confirmation debounce. A "no ball in play" flag (e.g. Stern SAM 0x02110904 bit 0x40)
// is ALSO set between balls during the end-of-ball bonus / ball search, which can exceed a couple
// of seconds (Metallica). A pending game-over is cancelled the instant a ball relaunches, so this
// only needs to exceed the longest between-ball pause to avoid finalizing the game mid-play.
static constexpr int kGameOverConfirmSeconds = 25;
// Ignore "games" shorter than this. Resuming a table reloads the previous game's score from
// NVRAM, which can briefly look like a live game; a real game always lasts longer.
static constexpr int kMinGameDurationSeconds = 30;
// Number of score increases observed while game_over claims to be asserted (before the session
// ever entered play) after which the field is considered wrong and ignored for the session.
static constexpr int kGameOverAnomalyLimit = 3;

NvramTracker::~NvramTracker() { Stop(); }

NvramTracker::MapStatus NvramTracker::ProbeMap(const string& gameId, const string& mapsPath, string& detail)
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
      const std::filesystem::path mapPath = mapsRoot / indexJson[gameId].get<string>();
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
      detail = "failed to parse index.json: " + string(e.what());
      return MapStatus::Error;
   }
}

int NvramTracker::ParseIntVal(const nlohmann::json& val, int defaultVal)
{
   if (val.is_null())
      return defaultVal;
   if (val.is_number())
      return val.get<int>();
   if (val.is_string())
   {
      const string s = val.get<string>();
      if (s.empty())
         return defaultVal;
      try
      {
         if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
            return std::stoi(s, nullptr, 16);
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

vector<int> NvramTracker::ResolveAddresses(const nlohmann::json& desc)
{
   vector<int> out;
   if (desc.contains("offsets") && desc["offsets"].is_array())
   {
      for (const auto& item : desc["offsets"])
         out.push_back(ParseIntVal(item));
      return out;
   }

   if (desc.contains("start"))
   {
      const int start = ParseIntVal(desc["start"]);
      if (desc.contains("length"))
      {
         const int length = ParseIntVal(desc["length"]);
         for (int i = 0; i < length; ++i)
            out.push_back(start + i);
      }
      else if (desc.contains("end"))
      {
         const int end = ParseIntVal(desc["end"]);
         for (int i = start; i <= end; ++i)
            out.push_back(i);
      }
      else
      {
         out.push_back(start);
      }
   }
   return out;
}

Descriptor NvramTracker::ParseDescriptor(const nlohmann::json& desc)
{
   Descriptor d;
   if (m_platformData.contains("endian") && m_platformData["endian"].is_string())
      d.isLittleEndian = m_platformData["endian"].get<string>() == "little";

   if (desc.contains("label"))
      d.label = desc["label"].get<string>();

   string encStr = desc.contains("encoding") ? desc["encoding"].get<string>() : "int"s;
   std::transform(encStr.begin(), encStr.end(), encStr.begin(), ::tolower);
   if (encStr == "bcd")
      d.encoding = EncodingType::Bcd;
   else if (encStr == "bool")
      d.encoding = EncodingType::Bool;
   else
      d.encoding = EncodingType::Int;

   if (desc.contains("nibble"))
   {
      d.nibble = desc["nibble"].get<string>();
      std::transform(d.nibble.begin(), d.nibble.end(), d.nibble.begin(), ::tolower);
   }

   if (desc.contains("mask"))
   {
      d.mask = ParseIntVal(desc["mask"]);
      d.hasMask = true;
   }

   if (desc.contains("endian"))
      d.isLittleEndian = desc["endian"].get<string>() == "little";

   if (desc.contains("invert"))
      d.invert = desc["invert"].get<bool>();
   if (desc.contains("scale"))
      d.scale = desc["scale"].get<double>();
   if (desc.contains("offset"))
      d.offset = desc["offset"].get<double>();

   d.addresses = ResolveAddresses(desc);

   return d;
}

void NvramTracker::ResolveDescriptor(Descriptor& d)
{
   d.resolvedAddresses.clear();
   for (int addr : d.addresses)
   {
      ResolvedAddress ra;
      ra.address = addr;
      ra.isRam = false;
      ra.fileOffset = -1;
      ra.nibble = d.nibble.empty() ? "both" : d.nibble;

      if (GetAddrRegionType(addr) == "ram")
      {
         ra.isRam = true;
      }
      else if (!m_segments.empty())
      {
         const NvramSegment* seg = GetSegmentForAddr(addr);
         if (seg == nullptr)
         {
            // Some platforms mirror the start of the NVRAM file over low RAM addresses
            if (m_lowRamMirrorLimit > 0 && addr >= 0 && addr < m_lowRamMirrorLimit)
               ra.fileOffset = addr;
         }
         else
         {
            ra.fileOffset = seg->fileBase + (addr - seg->address);
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
      d.resolvedAddresses.push_back(ra);

      if (ra.isRam)
         m_hasRamFields = true;
   }

   d.resolvedNibbleMode = d.resolvedAddresses.empty() ? "both"s : d.resolvedAddresses.back().nibble;
}

bool NvramTracker::LoadMap(const string& gameId)
{
   const std::filesystem::path mapsRoot(m_mapsPath);
   const std::filesystem::path indexPath = mapsRoot / "index.json";

   std::ifstream indexFile(indexPath);
   if (!indexFile.is_open())
   {
      LOGE("Could not open index.json at %s", indexPath.string().c_str());
      return false;
   }

   nlohmann::json indexJson;
   try
   {
      indexFile >> indexJson;
   }
   catch (const std::exception& e)
   {
      LOGE("JSON parse error in index.json: %s", e.what());
      return false;
   }

   if (!indexJson.contains(gameId))
   {
      LOGE("ROM %s not found in index.json", gameId.c_str());
      return false;
   }

   const std::filesystem::path mapPath = mapsRoot / indexJson[gameId].get<string>();
   std::ifstream mapFile(mapPath);
   if (!mapFile.is_open())
   {
      LOGE("Could not open map file at %s", mapPath.string().c_str());
      return false;
   }

   try
   {
      mapFile >> m_mapData;
   }
   catch (const std::exception& e)
   {
      LOGE("JSON parse error in map file %s: %s", mapPath.string().c_str(), e.what());
      return false;
   }

   // Load platform data if referenced
   if (m_mapData.contains("_metadata") && m_mapData["_metadata"].contains("platform"))
   {
      const string platform = m_mapData["_metadata"]["platform"].get<string>();
      const std::filesystem::path platformPath = mapsRoot / "platforms" / (platform + ".json");
      std::ifstream platformFile(platformPath);
      if (platformFile.is_open())
      {
         try
         {
            platformFile >> m_platformData;
         }
         catch (const std::exception& e)
         {
            LOGE("JSON parse error in platform file %s: %s", platformPath.string().c_str(), e.what());
         }
      }
   }

   // Parse the platform memory layout: it maps the (CPU) addresses used in the map file to
   // NVRAM file offsets, and identifies values living in volatile RAM instead of NVRAM
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
            entry.type = entryJson["type"].get<string>();
         if (entryJson.contains("nibble"))
            entry.nibble = entryJson["nibble"].get<string>();
         m_layout.push_back(entry);

         string typeLower = entry.type;
         std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);

         if (typeLower == "nvram")
         {
            NvramSegment seg;
            seg.address = entry.address;
            seg.size = entry.size;
            seg.nibble = entry.nibble.empty() ? "both" : entry.nibble;
            std::transform(seg.nibble.begin(), seg.nibble.end(), seg.nibble.begin(), ::tolower);
            seg.fileBase = nvramFileBase;
            m_segments.push_back(seg);
            nvramFileBase += entry.size;

            if (firstNvramAddr == -1 || entry.address < firstNvramAddr)
               firstNvramAddr = entry.address;
         }
         else if (typeLower == "ram" && entry.address == 0)
         {
            ramZeroSize = std::max(ramZeroSize, entry.size);
         }
      }

      std::sort(m_segments.begin(), m_segments.end(), [](const NvramSegment& a, const NvramSegment& b) { return a.address < b.address; });

      if (firstNvramAddr > 0 && ramZeroSize >= firstNvramAddr)
         m_lowRamMirrorLimit = firstNvramAddr;
   }

   // Parse game_state descriptors
   m_scoresDesc.clear();
   m_finalScoresDesc.clear();
   m_finalScoresMostRecentFirst = false;
   m_gameStateDescriptors.clear();
   m_hasRamFields = false;

   if (m_mapData.contains("game_state"))
   {
      for (const auto& [key, value] : m_mapData["game_state"].items())
      {
         if (key == "scores" && value.is_array())
         {
            for (const auto& scoreJson : value)
            {
               Descriptor d = ParseDescriptor(scoreJson);
               ResolveDescriptor(d);
               m_scoresDesc.push_back(d);
            }
         }
         else if (key == "final_scores" && value.is_array())
         {
            for (const auto& scoreJson : value)
            {
               Descriptor d = ParseDescriptor(scoreJson);
               ResolveDescriptor(d);
               m_finalScoresDesc.push_back(d);
            }
         }
         else if (key == "final_scores_order" && value.is_string())
         {
            m_finalScoresMostRecentFirst = value.get<string>() == "most_recent_first";
         }
         else if (value.is_object())
         {
            Descriptor d = ParseDescriptor(value);
            ResolveDescriptor(d);
            m_gameStateDescriptors.emplace_back(key, d);
         }
      }
   }

   LOGI("ROM map successfully loaded: %s", gameId.c_str());
   return true;
}

string NvramTracker::GetAddrRegionType(int addr) const
{
   for (const auto& e : m_layout)
   {
      if (e.address <= addr && addr < (e.address + e.size))
      {
         string t = e.type;
         std::transform(t.begin(), t.end(), t.begin(), ::tolower);
         return t;
      }
   }
   return ""s;
}

const NvramSegment* NvramTracker::GetSegmentForAddr(int addr) const
{
   for (const auto& seg : m_segments)
   {
      if (seg.address <= addr && addr < (seg.address + seg.size))
         return &seg;
   }
   return nullptr;
}

bool NvramTracker::ReadUnits(const Descriptor& desc, vector<uint8_t>& units)
{
   units.clear();
   for (const auto& ra : desc.resolvedAddresses)
   {
      if (ra.isRam)
      {
         // Value lives in volatile RAM, not in the NVRAM snapshot: read it through the CPU memory map
         uint8_t val = 0;
         if (PinmameReadMainCPUByte(static_cast<uint32_t>(ra.address), &val) == 0)
            return false;
         units.push_back(desc.hasMask ? (val & desc.mask) : val);
      }
      else
      {
         if (ra.fileOffset < 0 || ra.fileOffset >= static_cast<int>(m_nvram.size()))
            return false;
         const uint8_t raw = m_nvram[ra.fileOffset];
         uint8_t unit;
         if (ra.nibble == "low")
            unit = raw & 0x0F;
         else if (ra.nibble == "high")
            unit = (raw >> 4) & 0x0F;
         else
            unit = raw;
         units.push_back(desc.hasMask ? (unit & desc.mask) : unit);
      }
   }
   return true;
}

int64_t NvramTracker::DecodeInt(const vector<uint8_t>& units, bool isLittleEndian)
{
   int64_t value = 0;
   if (isLittleEndian)
   {
      for (auto it = units.rbegin(); it != units.rend(); ++it)
         value = (value << 8) | *it;
   }
   else
   {
      for (uint8_t b : units)
         value = (value << 8) | b;
   }
   return value;
}

int64_t NvramTracker::DecodeBCD(const vector<uint8_t>& units, const string& nibbleMode, bool isLittleEndian)
{
   const bool singleNibble = nibbleMode == "low" || nibbleMode == "high";
   const auto appendByte = [singleNibble](int64_t value, uint8_t b) -> int64_t
   {
      if (singleNibble)
         return (value * 10) + ((b <= 9) ? b : 0);
      const int hi = (b >> 4) & 0x0F;
      const int lo = b & 0x0F;
      value = (value * 10) + ((hi <= 9) ? hi : 0);
      return (value * 10) + ((lo <= 9) ? lo : 0);
   };

   int64_t value = 0;
   if (isLittleEndian)
   {
      for (auto it = units.rbegin(); it != units.rend(); ++it)
         value = appendByte(value, *it);
   }
   else
   {
      for (uint8_t b : units)
         value = appendByte(value, b);
   }
   return value;
}

int64_t NvramTracker::Decode(const Descriptor& desc)
{
   if (!ReadUnits(desc, m_decodeUnits))
      return 0;

   int64_t value = 0;
   switch (desc.encoding)
   {
   case EncodingType::Int: value = DecodeInt(m_decodeUnits, desc.isLittleEndian); break;
   case EncodingType::Bcd: value = DecodeBCD(m_decodeUnits, desc.resolvedNibbleMode, desc.isLittleEndian); break;
   case EncodingType::Bool:
      value = DecodeInt(m_decodeUnits, desc.isLittleEndian) != 0;
      if (desc.invert)
         value = !value;
      break;
   }

   return static_cast<int64_t>(value * desc.scale + desc.offset);
}

vector<int64_t> NvramTracker::BuildFinalScoresSnapshot(const vector<int64_t>& playerScores, bool gameOverObserved, size_t& authoritativeCount)
{
   vector<int64_t> snapshot = playerScores;
   authoritativeCount = 0;
   // Without an authoritative frozen copy the live snapshot is the best data available
   if (m_finalScoresDesc.empty())
      return snapshot;

   if (m_finalScoresMostRecentFirst)
   {
      // The buffer receives one push per player at game-over; until then it still holds the
      // previous games, so an unfinished session must fall back to the live snapshot.
      if (!gameOverObserved)
         return snapshot;
      const size_t total = m_finalScoresDesc.size();
      vector<int64_t> current(total);
      for (size_t i = 0; i < total; ++i)
         current[i] = Decode(m_finalScoresDesc[i]);
      // Player count sources, most to least trusted: the mapped (glitch-filtered) player_count
      // byte; the number of entries pushed on top of the session-start baseline (the smallest
      // shift k that turns the baseline into the current list -- k == total always matches, so
      // a full-depth game terminates there); legacy single-player attribution. Verified on
      // bbb109 (real 2P and 4P games) and bsv103 (real 4P game, which has no count byte).
      size_t players = 0;
      const auto playerCount = m_maxGameStateValues.find("player_count");
      if (playerCount != m_maxGameStateValues.end() && playerCount->second >= 1)
         players = std::min(static_cast<size_t>(playerCount->second), total);
      else if (m_hasFinalScoresBaseline)
      {
         for (size_t k = 0; k <= total; ++k)
         {
            bool match = true;
            for (size_t i = k; i < total && match; ++i)
               match = current[i] == m_finalScoresBaseline[i - k];
            if (match)
            {
               players = k;
               break;
            }
         }
         // Game-over without a push (the machine records nothing for aborted games):
         // nothing can be attributed, keep the live snapshot.
         if (players == 0)
            return snapshot;
      }
      else
         players = 1;
      snapshot.assign(players, 0);
      for (size_t i = 0; i < players; ++i)
         snapshot[i] = current[players - 1 - i];
      authoritativeCount = players;
      return snapshot;
   }

   if (snapshot.size() < m_finalScoresDesc.size())
      snapshot.resize(m_finalScoresDesc.size(), 0);
   for (size_t i = 0; i < m_finalScoresDesc.size(); ++i)
      snapshot[i] = Decode(m_finalScoresDesc[i]);
   // Player indices beyond what game_state.final_scores covers have no authoritative source:
   // repeated live testing showed the "scores" reading for those slots does not track real play
   // (it moves around but never converges on the displayed score), so reporting it would just
   // be a different wrong number instead of an honest "no data".
   for (size_t i = m_finalScoresDesc.size(); i < snapshot.size(); ++i)
      snapshot[i] = 0;
   authoritativeCount = m_finalScoresDesc.size();
   return snapshot;
}

bool NvramTracker::Start(const string& gameId, const string& mapsPath, const string& tablePath, const string& outputPath)
{
   m_gameId = gameId;
   m_mapsPath = mapsPath;
   m_tablePath = tablePath;
   m_outputPath = outputPath;

   m_sessionStartRealTime = std::chrono::steady_clock::now();
   m_nvram.clear();
   m_playerScores.clear();
   m_decodedValues.clear();
   m_highestScores.clear();
   m_maxGameStateValues.clear();
   m_prevPlayerScores.clear();
   m_lastPlayerCountRead = -1;
   m_finalScoresBaseline.clear();
   m_hasFinalScoresBaseline = false;
   m_hasLastState = false;
   m_gameOverLast = false;
   m_gameOverPending = false;
   m_gameOverAnomalies = 0;
   m_ignoreGameOver = false;
   m_summarySent = false;
   m_hasBeenInPlay = false;
   m_monitoringStarted = false;

   if (!LoadMap(gameId))
   {
      LOGE("Failed to load JSON map for %s. Disabling monitoring.", gameId.c_str());
      return false;
   }

   m_monitoringStarted = true;
   return true;
}

void NvramTracker::Stop()
{
   // If VPX exits before a mapped game-over is confirmed, persist the played session as a
   // fallback. m_playerScores/m_nvram hold the last state read by Poll().
   if (m_monitoringStarted && !m_summarySent && !m_gameId.empty())
   {
      // A pending (not yet debounce-confirmed) game-over still means the machine already
      // archived the finished game, so final_scores may be trusted at exit.
      size_t authoritativeCount = 0;
      const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores, m_gameOverPending, authoritativeCount);
      // The peak scores also count as evidence of a played game: the live reading can already be
      // cleared or unreliable at exit while the recorded peaks hold the real values.
      const bool hasScore = std::any_of(finalScores.begin(), finalScores.end(), [](int64_t value) { return value > 0; })
         || std::any_of(m_highestScores.begin(), m_highestScores.end(), [](int64_t value) { return value > 0; });
      const bool writeExitFallback = m_hasBeenInPlay && hasScore;
      if (writeExitFallback)
         LOGI("No confirmed game-over before VPX exit; writing the active session as an exit fallback");
      FinalizeSession(finalScores, writeExitFallback, authoritativeCount);
      m_summarySent = true;
   }
   m_monitoringStarted = false;
}

void NvramTracker::Poll()
{
   if (!m_monitoringStarted)
      return;

   // Snapshot the machine NVRAM; nothing to do while no machine is running
   if (!PinmameIsRunning())
      return;
   const int maxNvram = PinmameGetMaxNVRAM();
   if (maxNvram <= 0)
      return;
   if (m_nvramBuffer.size() != static_cast<size_t>(maxNvram))
      m_nvramBuffer.resize(maxNvram);
   int count = PinmameGetNVRAM(m_nvramBuffer.data());
   if (count <= 0)
      return;

   bool nvramChanged = m_nvram.size() != static_cast<size_t>(count);
   if (!nvramChanged)
   {
      for (int i = 0; i < count; ++i)
      {
         if (m_nvram[i] != m_nvramBuffer[i].currStat)
         {
            nvramChanged = true;
            break;
         }
      }
   }
   if (nvramChanged)
   {
      m_nvram.resize(count);
      for (int i = 0; i < count; ++i)
         m_nvram[i] = m_nvramBuffer[i].currStat;
   }

   // If NVRAM has not changed and there are no RAM-mapped fields, nothing can have moved:
   // only check whether a pending game-over reached its confirmation delay
   if (!nvramChanged && !m_hasRamFields && m_hasLastState)
   {
      if (m_gameOverPending && !m_gameOverLast && std::chrono::steady_clock::now() - m_gameOverSince >= std::chrono::seconds(kGameOverConfirmSeconds))
      {
         LOGI("Game play for rom %s is over", m_gameId.c_str());
         if (!m_summarySent)
         {
            m_playerScores.resize(m_scoresDesc.size());
            for (size_t i = 0; i < m_scoresDesc.size(); ++i)
               m_playerScores[i] = Decode(m_scoresDesc[i]);
            size_t authoritativeCount = 0;
            const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores, true, authoritativeCount);
            FinalizeSession(finalScores, true, authoritativeCount);
            m_summarySent = true;
         }
         m_gameOverLast = true;
         m_hasBeenInPlay = false;
      }
      return;
   }

   // Decode the game state fields
   m_decodedValues.clear();
   for (const auto& [key, desc] : m_gameStateDescriptors)
      m_decodedValues[key] = Decode(desc);

   const auto gameOverIt = m_decodedValues.find("game_over");
   bool isGameOver = gameOverIt != m_decodedValues.end() && gameOverIt->second != 0;

   // Decode the per-player scores
   m_playerScores.resize(m_scoresDesc.size());
   for (size_t i = 0; i < m_scoresDesc.size(); ++i)
      m_playerScores[i] = Decode(m_scoresDesc[i]);

   // A game_over flag that stays asserted from boot while a score is visibly rising is wrong for
   // this machine (observed on Spider-Man VE, where the SAM flag address holds other data). Stop
   // trusting it for the rest of the session; the exit fallback then persists the played game.
   // Scoped to sessions that never entered play so the between-balls bonus count-up of a valid
   // flag (which also asserts it briefly) can never trip this.
   if (isGameOver && !m_ignoreGameOver && !m_hasBeenInPlay && m_prevPlayerScores.size() == m_playerScores.size())
   {
      bool increased = false;
      for (size_t i = 0; i < m_playerScores.size() && !increased; ++i)
         increased = m_playerScores[i] > m_prevPlayerScores[i];
      if (increased && ++m_gameOverAnomalies >= kGameOverAnomalyLimit)
      {
         LOGW("game_over reads as asserted while scores are rising; ignoring the game_over field for this session");
         m_ignoreGameOver = true;
      }
   }
   if (m_ignoreGameOver)
      isGameOver = false;
   m_prevPlayerScores = m_playerScores;

   // A new game zeroes the score area, and that is the only unambiguous session boundary
   // available here: game_over also deasserts when a ball merely relaunches after the
   // end-of-ball bonus, which is why confirming it needs kGameOverConfirmSeconds. Starting the
   // next game inside that window -- routine between back-to-back games -- deasserts game_over
   // and cancels the pending confirmation below, so the finished game is never confirmed, never
   // written, and never resets the session: its peak scores and player_count then bleed into the
   // new game. Observed on afm_113b, where two games were reported as one record and a 1-player
   // game carried the previous 2-player game's player 2 score with player_count 2.
   // Scoped to !isGameOver because several platforms clear the live "scores" as part of the
   // game-over sequence (the reason final_scores exists); that clear is not a new game.
   bool newGameStarted = false;
   if (!isGameOver && m_hasBeenInPlay && !m_playerScores.empty())
   {
      const bool allZero = std::all_of(m_playerScores.begin(), m_playerScores.end(), [](int64_t value) { return value == 0; });
      const bool hadScore = std::any_of(m_highestScores.begin(), m_highestScores.end(), [](int64_t value) { return value > 0; });
      newGameStarted = allZero && hadScore;
   }

   if (newGameStarted)
   {
      // Persist the finished game before the new one overwrites its state. The ROM has already
      // cleared the live scores, so the peaks in m_highestScores are the only copy left;
      // FinalizeSession merges them in. A span too short to be a real game is still dropped by
      // its minimum-duration guard.
      if (!m_summarySent)
      {
         LOGI("New game started for rom %s before the previous game-over was confirmed; finalizing the previous game", m_gameId.c_str());
         size_t authoritativeCount = 0;
         const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores, m_gameOverPending, authoritativeCount);
         FinalizeSession(finalScores, true, authoritativeCount);
         m_summarySent = true;
      }
      // Re-arm so the reset below starts a clean session for the game that just began.
      m_hasBeenInPlay = false;
   }

   // Raw lifecycle flags can briefly toggle during display and ball-state transitions. Only
   // confirm game-over after it remains asserted for the confirmation delay; a transient must
   // never finalize or reset a session.
   bool sessionReset = false;
   if (!isGameOver)
   {
      m_gameOverPending = false;
      if (!m_hasBeenInPlay)
      {
         sessionReset = true;
         m_summarySent = false;
         m_highestScores.clear();
         m_maxGameStateValues.clear();
         m_sessionStartRealTime = std::chrono::steady_clock::now();
         // Baseline of the most-recent-first buffer: it still holds only previous games until
         // this game's game-over push, so whatever sits on top of it then was pushed by the
         // current game -- one entry per player.
         if (m_finalScoresMostRecentFirst)
         {
            m_finalScoresBaseline.resize(m_finalScoresDesc.size());
            for (size_t i = 0; i < m_finalScoresDesc.size(); ++i)
               m_finalScoresBaseline[i] = Decode(m_finalScoresDesc[i]);
            m_hasFinalScoresBaseline = true;
         }
      }
      m_gameOverLast = false;
      m_hasBeenInPlay = true;
   }
   else if (!m_gameOverPending)
   {
      m_gameOverPending = true;
      m_gameOverSince = std::chrono::steady_clock::now();
   }

   // Update statistics only after a game has actually entered play. Scores retained in NVRAM
   // during attract mode belong to the previous game and must not become the next session's
   // highest score. The poll that starts a session is skipped as well: its values were decoded
   // at the top of this call, before the reset above, so they can still be the previous game's.
   // On WPC the game_over byte clears on the start-button press, a moment before the ROM zeroes
   // the score area and player_count -- folding that read back in would re-seed the accumulators
   // the reset just cleared, and max() can never walk them back down.
   if (m_hasBeenInPlay && !sessionReset)
   {
      if (m_highestScores.size() < m_playerScores.size())
         m_highestScores.resize(m_playerScores.size(), 0);
      for (size_t i = 0; i < m_playerScores.size(); ++i)
         m_highestScores[i] = std::max(m_highestScores[i], m_playerScores[i]);
      for (const auto& [key, val] : m_decodedValues)
      {
         // player_count drives the final_scores attribution, so one corrupted read must not
         // poison the session maximum (a real 4-player game recorded a transient 195): accept
         // only plausible values confirmed by two consecutive polls, and none once game_over
         // is asserted -- the game-over sequence can reuse that work RAM.
         if (key == "player_count")
         {
            const bool confirmed = val == m_lastPlayerCountRead;
            m_lastPlayerCountRead = val;
            if (isGameOver || !confirmed || val < 1 || val > 8)
               continue;
         }
         auto it = m_maxGameStateValues.find(key);
         if (it == m_maxGameStateValues.end())
            m_maxGameStateValues[key] = val;
         else
            it->second = std::max(it->second, val);
      }
   }

   // Handle confirmed game over
   const bool gameOverConfirmed = isGameOver && m_gameOverPending && std::chrono::steady_clock::now() - m_gameOverSince >= std::chrono::seconds(kGameOverConfirmSeconds);
   if (gameOverConfirmed && m_hasBeenInPlay && !m_gameOverLast)
   {
      LOGI("Game play for rom %s is over", m_gameId.c_str());
      if (!m_summarySent)
      {
         size_t authoritativeCount = 0;
         const vector<int64_t> finalScores = BuildFinalScoresSnapshot(m_playerScores, true, authoritativeCount);
         FinalizeSession(finalScores, true, authoritativeCount);
         m_summarySent = true;
      }
      m_gameOverLast = true;
      m_hasBeenInPlay = false;
   }

   m_hasLastState = true;
}

nlohmann::json NvramTracker::BuildCompletedGameState() const
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

void NvramTracker::FinalizeSession(const vector<int64_t>& finalScores, bool writeScoresFile, size_t authoritativeCount)
{
   // Real play time: session start -> when game-over was first detected, excluding the
   // game-over confirmation debounce
   const auto endTime = m_gameOverPending ? m_gameOverSince : std::chrono::steady_clock::now();
   const int64_t duration = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(endTime - m_sessionStartRealTime).count());

   // Report the peak score per player. The instantaneous score at game-over can lag the final
   // bonus; the peak seen during the session is the robust value. Indices below
   // authoritativeCount came from game_state.final_scores -- a frozen value the ROM itself
   // writes at game-over -- so they are trusted as-is and skip the peak merge (the live
   // "scores" source they'd be merged against may be unreliable on platforms where
   // final_scores exists specifically to work around that).
   vector<int64_t> bestScores = finalScores;
   // Only report the players who actually took part when the machine exposes a player count:
   // unused player slots do not read 0 everywhere (SAM holds 0xFF filler there, other platforms
   // keep stale data), so extra slots would record garbage.
   const auto playerCount = m_maxGameStateValues.find("player_count");
   if (playerCount != m_maxGameStateValues.end() && playerCount->second >= 1 && playerCount->second < static_cast<int64_t>(bestScores.size()))
      bestScores.resize(static_cast<size_t>(playerCount->second));
   for (size_t i = authoritativeCount; i < bestScores.size(); ++i)
      if (i < m_highestScores.size() && m_highestScores[i] > bestScores[i])
         bestScores[i] = m_highestScores[i];

   string scoreList;
   for (int64_t s : bestScores)
      scoreList += (scoreList.empty() ? ""s : ", "s) + std::to_string(s);
   LOGI("Game summary: rom=%s, duration=%llds, scores=[%s]", m_gameId.c_str(), static_cast<long long>(duration), scoreList.c_str());

   if (!writeScoresFile)
      return;

   if (duration < kMinGameDurationSeconds)
   {
      LOGI("Ignoring game shorter than %ds (was %llds) - not writing scores.json", kMinGameDurationSeconds, static_cast<long long>(duration));
      return;
   }

   CompletedGameRecord record;
   record.tablePath = m_tablePath;
   record.outputPath = m_outputPath;
   record.rom = m_gameId;
   record.scores = bestScores;
   record.gameDuration = duration;
   record.gameState = BuildCompletedGameState();
   ScoresFileWriter::AppendCompletedGame(record);
}

}
