// license:GPLv3+

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>
#include "plugins/MsgPlugin.h"
#include "libpinmame.h"

// Forward declarations for Mongoose
struct mg_mgr;
struct mg_connection;

namespace ScoreTrackerPlugin {

enum class EncodingType
{
   Int,
   Bcd,
   Bool
};

struct ResolvedAddress
{
   int address;
   bool isRam;
   int fileOffset;
   std::string nibble;
};

struct NVRAMSegment
{
   int address;
   int size;
   int file_base;
   std::string nibble;
};

struct MemoryLayoutEntry
{
   int address;
   int size;
   std::string type;
   std::string nibble;
};

struct Descriptor
{
   std::string label;
   EncodingType encoding = EncodingType::Int;
   std::string nibble;
   unsigned int mask = 0;
   bool has_mask = false;
   bool is_little_endian = false;
   bool invert = false;
   double scale = 1.0;
   double offset = 0.0;
   std::vector<int> addresses;

   std::vector<ResolvedAddress> resolvedAddresses;
   std::string resolvedNibbleMode;
};

class ScoreTracker
{
public:
   enum class MapStatus
   {
      Available,
      NotFound,
      Error,
   };

   ScoreTracker(const MsgPluginAPI* api, unsigned int endpointId);
   ~ScoreTracker();

   // Lightweight capability check used to choose one authoritative source
   // before either NVRAM or a fallback tracker is activated.
   static MapStatus ProbeMap(const std::string& gameId, const std::string& mapsPath, std::string& detail);

   // Returns true when monitoring started (a JSON map exists for the gameId).
   bool Start(const std::string& gameId, const std::string& mapsPath, int port, int pollIntervalMs, bool enableWebSocket,
      const std::string& tablePath);
   void Stop();

private:
   void Loop();
   void StartWebServer(int port);
   void StopWebServer();

   // Mapping and parsing helpers
   bool LoadMap(const std::string& gameId);
   void ResolveDescriptor(Descriptor& d);
   std::vector<int> ResolveAddresses(const nlohmann::json& desc);
   Descriptor ParseDescriptor(const nlohmann::json& desc);
   int ParseIntVal(const nlohmann::json& val, int defaultVal = 0);
   
   // Decoding helpers
   bool ReadUnits(const std::vector<uint8_t>& nvram, const Descriptor& desc, std::vector<uint8_t>& units);
   int64_t DecodeInt(const std::vector<uint8_t>& units, bool is_little_endian);
   int64_t DecodeBCD(const std::vector<uint8_t>& units, const std::string& nibbleMode, bool is_little_endian);
   int64_t Decode(const std::vector<uint8_t>& nvram, const Descriptor& desc);

   // Platform memory region helper
   std::string GetAddrRegionType(int addr);
   NVRAMSegment* GetSegmentForAddr(int addr);

   // Send the summary payload and optionally append it to scores.json.
   // authoritativeCount is the number of leading entries in finalScores that came
   // from game_state.final_scores (a ROM-frozen value) rather than the live
   // "scores" descriptors, so they should not be overwritten by the peak-score merge.
   void SendSummaryPayload(const std::vector<int64_t>& finalScores, bool writeScoresFile, size_t authoritativeCount = 0);

   // Overlays game_state.final_scores onto the live "scores" snapshot (see
   // SendSummaryPayload) and zeroes out any player slot beyond what
   // final_scores covers -- those live "scores" readings have no authoritative
   // source and don't track real play, so reporting 0 (no data) beats
   // reporting a different wrong number.
   std::vector<int64_t> BuildFinalScoresSnapshot(const std::vector<int64_t>& playerScores);
   nlohmann::json BuildCompletedGameState() const;

   // MsgPlugin APIs
   const MsgPluginAPI* m_msgApi;
   unsigned int m_endpointId;

   std::string m_gameId;
   std::string m_mapsPath;
   std::string m_tablePath;
   
   // Loaded map data
   nlohmann::json m_mapData;
   nlohmann::json m_platformData;
   std::vector<NVRAMSegment> m_segments;
   std::vector<MemoryLayoutEntry> m_layout;
   int m_lowRamMirrorLimit = 0;

   // Descriptors
   std::vector<Descriptor> m_scoresDesc;
   // Per-player score snapshot captured by the ROM at game-over (game_state.final_scores).
   // Several platforms clear/reuse the live "scores" location as soon as the game ends,
   // so this frozen copy is the only reliable read once game-over is confirmed.
   std::vector<Descriptor> m_finalScoresDesc;
   std::vector<std::pair<std::string, Descriptor>> m_gameStateDescriptors;
   bool m_hasRamFields = false;

   // Threading
   std::thread m_thread;
   std::atomic<bool> m_running{false};

   // Web server
   std::thread m_webThread;
   std::atomic<bool> m_webRunning{false};
   struct mg_mgr* m_mgr = nullptr;
   std::mutex m_clientsMutex;
   std::vector<struct mg_connection*> m_clients;

   // Last state
   std::mutex m_stateMutex;
   std::string m_lastJsonState;
   std::mutex m_nvramMutex;
   std::vector<uint8_t> m_lastNvram;
   bool m_enableWebSocket = false;
   int m_pollIntervalMs = 250;

   // Reusable buffers for inner-loop to avoid frequent allocations
   std::vector<uint8_t> m_loopNvram;
   std::vector<PinmameNVRAMState> m_loopNvramBuffer;
   std::vector<uint8_t> m_decodeUnits;

   // Summary/Session tracking
   std::chrono::steady_clock::time_point m_sessionStartRealTime;
   std::vector<int64_t> m_highestScores;
   std::unordered_map<std::string, int64_t> m_maxGameStateValues;
   bool m_gameOverLast = false;
   bool m_gameOverPending = false;
   std::chrono::steady_clock::time_point m_gameOverSince;
   bool m_summarySent = false;
   bool m_hasBeenInPlay = false;
   bool m_monitoringStarted = false;

   friend void WebEventHandler(struct mg_connection* c, int ev, void* ev_data);
};

} // namespace ScoreTrackerPlugin
