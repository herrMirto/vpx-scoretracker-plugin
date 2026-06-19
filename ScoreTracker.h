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

struct NVRAMSegment {
    int address;
    int size;
    int file_base;
    std::string nibble;
};

struct MemoryLayoutEntry {
    int address;
    int size;
    std::string type;
    std::string nibble;
};

struct Descriptor {
    std::string label;
    std::string encoding;
    std::string nibble;
    unsigned int mask = 0;
    bool has_mask = false;
    std::string endian = "big";
    bool invert = false;
    double scale = 1.0;
    double offset = 0.0;
    std::vector<int> addresses;
};

class ScoreTracker {
public:
    ScoreTracker(const MsgPluginAPI* api, unsigned int endpointId);
    ~ScoreTracker();

    // Returns true when monitoring started (a JSON map exists for the gameId).
    bool Start(const std::string& gameId, const std::string& mapsPath, int port, const std::string& tablePath);
    void Stop();

private:
    void Loop();
    void StartWebServer(int port);
    void StopWebServer();

    // Mapping and parsing helpers
    bool LoadMap(const std::string& gameId);
    std::vector<int> ResolveAddresses(const nlohmann::json& desc);
    Descriptor ParseDescriptor(const nlohmann::json& desc);
    int ParseIntVal(const nlohmann::json& val, int defaultVal = 0);
    
    // Decoding helpers
    bool ReadUnits(const std::vector<uint8_t>& nvram, const Descriptor& desc, std::vector<uint8_t>& units, std::string& nibbleMode);
    int64_t DecodeInt(const std::vector<uint8_t>& units, const std::string& endian);
    int64_t DecodeBCD(const std::vector<uint8_t>& units, const std::string& nibbleMode, const std::string& endian);
    int64_t Decode(const std::vector<uint8_t>& nvram, const Descriptor& desc);

    // Platform memory region helper
    std::string GetAddrRegionType(int addr);
    NVRAMSegment* GetSegmentForAddr(int addr);

    // Send the summary payload and optionally append it to scores.json.
    void SendSummaryPayload(const std::vector<int64_t>& finalScores, bool writeScoresFile);
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
    std::vector<std::pair<std::string, Descriptor>> m_gameStateDescriptors;

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

    // Summary/Session tracking
    std::chrono::steady_clock::time_point m_sessionStartRealTime;
    std::vector<int64_t> m_highestScores;
    std::unordered_map<std::string, int64_t> m_maxGameStateValues;
    bool m_gameOverLast = false;
    bool m_summarySent = false;
    bool m_hasBeenInPlay = false;

    friend void WebEventHandler(struct mg_connection* c, int ev, void* ev_data);
};

} // namespace ScoreTrackerPlugin
