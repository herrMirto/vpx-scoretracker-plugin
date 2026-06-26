// license:GPLv3+

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugins/MsgPlugin.h"
#include "plugins/ScriptablePlugin.h"

// Forward declarations for Mongoose
struct mg_mgr;
struct mg_connection;

namespace ScoreTrackerPlugin {

// Captures scores and game state from EM / original tables that report
// through B2S.Server script calls (B2SSetScorePlayer, B2SSetGameOver, ...)
// instead of a PinMAME ROM.
//
// It works by re-registering the "B2S.Server" COM object override with a
// proxy class definition: every member forwards to the real B2S server
// implementation (plugin "b2s" or "b2slegacy"), but score/state setters are
// observed on the way through. The captured state is broadcast over the same
// WebSocket JSON protocol as ScoreTracker, so downstream consumers work
// unchanged for EM tables.
class B2STracker {
public:
    B2STracker(const MsgPluginAPI* api, unsigned int endpointId);
    ~B2STracker();

    // Wrap the B2S server class and (re-)claim the "B2S.Server" COM override.
    // Idempotent; call at plugin load and again on every plugin-loaded event
    // so we stay the last (winning) writer of the override.
    void InstallProxy(const ScriptablePluginAPI* scriptApi);

    // True while the NVRAM ScoreTracker runs for a PinMAME game; the EM
    // tracker then stays dormant (it would otherwise fight for the port).
    void SetPinmameActive(bool active);

    // Controller game lifecycle (the b2s plugins broadcast these with the
    // B2SName as gameId once the backglass registers its first state).
    void OnGameStart(const std::string& gameId, int wsPort, const std::string& tablePath);
    void OnGameEnd();
    // Stop and clear a provisional fallback without emitting a result. Used
    // when a higher-priority NVRAM map becomes available for the table.
    void DiscardSession();

private:
    enum class Op {
        None,
        SetName,      // B2SName property setter → table identity
        ScorePlayer,  // B2SSetScorePlayer(player, score)
        ScorePlayerN, // B2SSetScorePlayer1..6(score), fixed player index
        ScoreDigit,   // B2SSetScoreDigit(digit, value) — observed, not decoded
        GameOver,
        BallInPlay,
        CanPlay,
        Credits,
        PlayerUp,
        Tilt,
    };

    struct MemberOp {
        Op op = Op::None;
        int fixedPlayer = 0;
    };

    // Single dispatcher installed as Call on every proxied member.
    static void MSGPIAPI InterceptCall(void* me, int memberIndex, ScriptVariant* pArgs, ScriptVariant* pRet);
    static void* MSGPIAPI CreateProxiedObject();

    void BuildProxy(const ScriptClassDef* baseDef);
    void Capture(const MemberOp& mop, unsigned int nArgs, ScriptVariant* pArgs);
    void SetScore(int player, int64_t value);
    void EnsureStarted();
    void ResetSession(bool emitSummary);

    void Loop();
    void StartWebServer(int port);
    void StopWebServer();
    void SendSummaryLocked(bool writeScoresFile); // m_stateMutex must be held
    nlohmann::json BuildCompletedGameStateLocked() const;

    const MsgPluginAPI* m_msgApi;
    unsigned int m_endpointId;
    const ScriptablePluginAPI* m_scriptApi = nullptr;

    // Proxy over the real B2S server class (intentionally leaked on rebuild:
    // the script host may keep references for the lifetime of the process).
    ScriptClassDef* m_proxyDef = nullptr;
    const ScriptClassDef* m_baseDef = nullptr;
    std::vector<MemberOp> m_memberOps;

    std::atomic<bool> m_pinmameActive{false};
    int m_wsPort = 8889;

    // Captured EM game state
    std::mutex m_stateMutex;
    std::string m_tableName;   // from B2SName, falls back to controller gameId
    std::string m_gameId;
    std::string m_tablePath;
    std::array<int64_t, 6> m_scores{};
    int m_maxPlayerSeen = 1;
    int m_playerUp = 1;
    int m_ballInPlay = 0;
    int m_canPlay = 0;
    int m_credits = 0;
    int m_tilt = 0;
    bool m_gameOver = false;
    bool m_hasBeenInPlay = false;
    bool m_summarySent = false;
    bool m_scoreDigitWarned = false;
    std::array<int64_t, 6> m_highestScores{};
    std::chrono::steady_clock::time_point m_gameStartRealTime;

    // Broadcast loop + web server (started lazily on first captured call)
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::thread m_webThread;
    std::atomic<bool> m_webRunning{false};
    struct mg_mgr* m_mgr = nullptr;
    std::mutex m_clientsMutex;
    std::vector<struct mg_connection*> m_clients;
    std::string m_lastJsonState;
    std::vector<std::string> m_pendingEvents; // summary payloads to broadcast

    static B2STracker* s_instance;

    friend void B2SWebEventHandler(struct mg_connection* c, int ev, void* ev_data);
};

} // namespace ScoreTrackerPlugin
