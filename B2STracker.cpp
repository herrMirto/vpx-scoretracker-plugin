// license:GPLv3+

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "B2STracker.h"
#include "ScoresFileWriter.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>

#include <nlohmann/json.hpp>

#include "plugins/LoggingPlugin.h"
#include "mongoose/mongoose.h"

using namespace std::string_literals;

namespace ScoreTrackerPlugin {

LPI_USE_CPP();

B2STracker* B2STracker::s_instance = nullptr;

static std::string ToLower(const char* s)
{
    std::string out(s ? s : "");
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

void B2SWebEventHandler(struct mg_connection* c, int ev, void* ev_data) {
    if (c->mgr == nullptr || c->mgr->userdata == nullptr) return;
    B2STracker* tracker = static_cast<B2STracker*>(c->mgr->userdata);

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = static_cast<struct mg_http_message*>(ev_data);
        if (hm->uri.len == 3 && strncmp(hm->uri.buf, "/ws", 3) == 0) {
            mg_ws_upgrade(c, hm, nullptr);
        } else {
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "ScoreTracker WebSocket server is running. Connect to /ws.");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        std::lock_guard<std::mutex> lock(tracker->m_clientsMutex);
        tracker->m_clients.push_back(c);
        if (!tracker->m_lastJsonState.empty()) {
            mg_ws_send(c, tracker->m_lastJsonState.c_str(), tracker->m_lastJsonState.length(), WEBSOCKET_OP_TEXT);
        }
    } else if (ev == MG_EV_CLOSE) {
        std::lock_guard<std::mutex> lock(tracker->m_clientsMutex);
        auto it = std::find(tracker->m_clients.begin(), tracker->m_clients.end(), c);
        if (it != tracker->m_clients.end()) {
            tracker->m_clients.erase(it);
        }
    }
}

B2STracker::B2STracker(const MsgPluginAPI* api, unsigned int endpointId)
    : m_msgApi(api)
    , m_endpointId(endpointId)
{
    s_instance = this;
}

B2STracker::~B2STracker()
{
    OnGameEnd();
    s_instance = nullptr;
}

// ---------------------------------------------------------------------------
// COM override proxy
// ---------------------------------------------------------------------------

void B2STracker::InstallProxy(const ScriptablePluginAPI* scriptApi)
{
    if (scriptApi == nullptr)
        return;
    m_scriptApi = scriptApi;

    // Prefer the legacy server when both B2S plugins are loaded: it loads
    // after the new one (directory order), so in a vanilla setup its override
    // is the one tables would get.
    const ScriptClassDef* baseDef = scriptApi->GetClassDef("B2SLegacy_Server");
    if (baseDef == nullptr)
        baseDef = scriptApi->GetClassDef("B2S_Server");
    if (baseDef == nullptr || baseDef->CreateObject == nullptr)
        return; // no B2S plugin loaded (yet) — retry on the next plugin-loaded event

    if (baseDef != m_baseDef)
        BuildProxy(baseDef);

    // Re-claim the override: last writer wins, and the b2s plugins may have
    // (re-)registered theirs after us.
    scriptApi->SetCOMObjectOverride("B2S.Server", m_proxyDef);
}

void B2STracker::BuildProxy(const ScriptClassDef* baseDef)
{
    const size_t size = sizeof(ScriptClassDef) + baseDef->nMembers * sizeof(ScriptClassMemberDef);
    ScriptClassDef* proxy = static_cast<ScriptClassDef*>(malloc(size));
    memcpy(proxy, baseDef, size);
    proxy->name.name = "ScoreTracker_B2SServer";
    // Keep the registered base type ID. DynamicDispatch resolves member names
    // through this ID; assigning 0 makes every copied B2S member invisible.
    proxy->name.id = baseDef->name.id;
    proxy->CreateObject = CreateProxiedObject;

    m_memberOps.assign(baseDef->nMembers, MemberOp{});
    for (unsigned int i = 0; i < baseDef->nMembers; i++)
    {
        proxy->members[i].Call = InterceptCall;

        const std::string name = ToLower(baseDef->members[i].name.name);
        const unsigned int nArgs = baseDef->members[i].nArgs;
        MemberOp& mop = m_memberOps[i];
        if (name == "b2sname" && nArgs == 1)
            mop.op = Op::SetName;
        else if (name == "b2ssetscoreplayer" && nArgs == 2)
            mop.op = Op::ScorePlayer;
        else if (name.rfind("b2ssetscoreplayer", 0) == 0 && name.length() == 18 && nArgs == 1
                 && name[17] >= '1' && name[17] <= '6')
        {
            mop.op = Op::ScorePlayerN;
            mop.fixedPlayer = name[17] - '0';
        }
        else if (name == "b2ssetscoredigit")
            mop.op = Op::ScoreDigit;
        else if (name == "b2ssetgameover")
            mop.op = Op::GameOver;
        else if (name == "b2ssetballinplay")
            mop.op = Op::BallInPlay;
        else if (name == "b2ssetcanplay")
            mop.op = Op::CanPlay;
        else if (name == "b2ssetcredits")
            mop.op = Op::Credits;
        else if (name == "b2ssetplayerup")
            mop.op = Op::PlayerUp;
        else if (name == "b2ssettilt")
            mop.op = Op::Tilt;
    }

    // The previous proxy (if any) is intentionally leaked: the script host
    // may still hold a pointer to it.
    m_baseDef = baseDef;
    m_proxyDef = proxy;

    LPI_LOGI_CPP(std::string("[INFO] - B2S score capture proxy installed over ") + baseDef->name.name);
    std::cout << "[ScoreTracker] B2S score capture proxy installed over " << baseDef->name.name << std::endl;
}

void* MSGPIAPI B2STracker::CreateProxiedObject()
{
    B2STracker* me = s_instance;
    if (me == nullptr || me->m_baseDef == nullptr || me->m_baseDef->CreateObject == nullptr)
        return nullptr;
    return me->m_baseDef->CreateObject();
}

void MSGPIAPI B2STracker::InterceptCall(void* me, int memberIndex, ScriptVariant* pArgs, ScriptVariant* pRet)
{
    B2STracker* tracker = s_instance;
    if (tracker == nullptr || tracker->m_baseDef == nullptr
        || memberIndex < 0 || static_cast<unsigned int>(memberIndex) >= tracker->m_baseDef->nMembers)
        return;

    const MemberOp& mop = tracker->m_memberOps[memberIndex];
    if (mop.op != Op::None)
        tracker->Capture(mop, tracker->m_baseDef->members[memberIndex].nArgs, pArgs);

    tracker->m_baseDef->members[memberIndex].Call(me, memberIndex, pArgs, pRet);
}

// ---------------------------------------------------------------------------
// State capture
// ---------------------------------------------------------------------------

void B2STracker::Capture(const MemberOp& mop, unsigned int nArgs, ScriptVariant* pArgs)
{
    // The proxy must still forward every B2S call, but it must not inspect or
    // retain fallback data while an NVRAM map is authoritative.
    if (m_pinmameActive.load())
        return;

    // Convenience setters have 1-arg (value) and 2-arg (custom id, value)
    // variants; the value is always the last argument.
    const int lastArg = (nArgs >= 1) ? pArgs[nArgs - 1].vInt : 0;

    bool meaningful = true;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        switch (mop.op)
        {
        case Op::SetName:
            if (pArgs[0].vString.string != nullptr)
                m_tableName = pArgs[0].vString.string;
            meaningful = false;
            break;
        case Op::ScorePlayer:
            SetScore(pArgs[0].vInt, pArgs[1].vInt);
            break;
        case Op::ScorePlayerN:
            SetScore(mop.fixedPlayer, pArgs[0].vInt);
            break;
        case Op::ScoreDigit:
            if (!m_scoreDigitWarned)
            {
                m_scoreDigitWarned = true;
                LPI_LOGI_CPP("[INFO] - Table reports scores via B2SSetScoreDigit; digit decoding is not supported yet"s);
            }
            meaningful = false;
            break;
        case Op::GameOver:
        {
            const bool over = (lastArg != 0);
            if (m_gameOver && !over)
            {
                // New game starting: reset per-game session statistics.
                m_highestScores.fill(0);
                m_summarySent = false;
                m_gameStartRealTime = std::chrono::steady_clock::now();
            }
            m_gameOver = over;
            break;
        }
        case Op::BallInPlay:
            m_ballInPlay = lastArg;
            break;
        case Op::CanPlay:
            m_canPlay = lastArg;
            break;
        case Op::Credits:
            m_credits = lastArg;
            break;
        case Op::PlayerUp:
            if (lastArg >= 1 && lastArg <= 6)
                m_playerUp = lastArg;
            break;
        case Op::Tilt:
            m_tilt = lastArg;
            break;
        default:
            meaningful = false;
            break;
        }
    }

    // A PinMAME game owns the websocket; EM capture stays dormant then.
    if (meaningful && !m_pinmameActive.load())
        EnsureStarted();
}

void B2STracker::SetScore(int player, int64_t value)
{
    if (player < 1 || player > 6)
        return;
    m_scores[player - 1] = value;
    m_maxPlayerSeen = (std::max)(m_maxPlayerSeen, player);
    if (value > m_highestScores[player - 1])
        m_highestScores[player - 1] = value;
}

void B2STracker::SetPinmameActive(bool active)
{
    const bool previous = m_pinmameActive.exchange(active);
    if (previous == active)
        return;
    if (active)
        LPI_LOGI_CPP("[INFO] - B2S fallback disabled; NVRAM map is authoritative. B2S calls will only be forwarded."s);
    else
        LPI_LOGI_CPP("[INFO] - B2S fallback armed because no NVRAM map is active."s);
}

void B2STracker::OnGameStart(const std::string& gameId, int wsPort, const std::string& tablePath)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_gameId = gameId;
    m_tablePath = tablePath;
    if (wsPort > 0)
        m_wsPort = wsPort;
}

void B2STracker::OnGameEnd()
{
    ResetSession(true);
}

void B2STracker::DiscardSession()
{
    LPI_LOGI_CPP("[INFO] - Discarding provisional B2S fallback state; switching to authoritative NVRAM map"s);
    ResetSession(false);
}

void B2STracker::ResetSession(bool emitSummary)
{
    if (m_running)
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (emitSummary && !m_summarySent && m_hasBeenInPlay)
            {
                SendSummaryLocked(false);
                m_summarySent = true;
            }
        }
        // Flush the final summary to connected clients before shutting down.
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            for (const std::string& evt : m_pendingEvents)
                for (struct mg_connection* conn : m_clients)
                    mg_ws_send(conn, evt.c_str(), evt.length(), WEBSOCKET_OP_TEXT);
            m_pendingEvents.clear();
        }
        StopWebServer();
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_scores.fill(0);
    m_highestScores.fill(0);
    m_maxPlayerSeen = 1;
    m_playerUp = 1;
    m_ballInPlay = m_canPlay = m_credits = m_tilt = 0;
    m_gameOver = false;
    m_hasBeenInPlay = false;
    m_summarySent = false;
    m_lastJsonState.clear();
}

// ---------------------------------------------------------------------------
// Broadcast loop (same payload shape as ScoreTracker)
// ---------------------------------------------------------------------------

void B2STracker::EnsureStarted()
{
    if (m_running.exchange(true))
        return;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_gameStartRealTime = std::chrono::steady_clock::now();
    }
    LPI_LOGI_CPP(std::string("[INFO] - EM (B2S) score tracking started for ") + (m_tableName.empty() ? m_gameId : m_tableName));
    std::cout << "[ScoreTracker] EM (B2S) score tracking started" << std::endl;

    StartWebServer(m_wsPort);
    m_thread = std::thread(&B2STracker::Loop, this);
}

void B2STracker::Loop()
{
    bool gameOverLast = false;

    while (m_running)
    {
        std::string jsonStr;
        std::vector<std::string> events;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);

            const std::string rom = m_tableName.empty() ? (m_gameId.empty() ? "em_table" : m_gameId) : m_tableName;
            const int playerCount = (std::max)(m_maxPlayerSeen, (std::min)(m_canPlay, 6));
            std::vector<int64_t> scores(m_scores.begin(), m_scores.begin() + (std::max)(playerCount, 1));

            const int currentPlayer = (m_playerUp >= 1 && m_playerUp <= static_cast<int>(scores.size())) ? m_playerUp : 1;

            if (!m_gameOver)
                m_hasBeenInPlay = true;

            if (m_gameOver && m_hasBeenInPlay && !gameOverLast && !m_summarySent)
            {
                SendSummaryLocked(true);
                m_summarySent = true;
                LPI_LOGI_CPP(std::string("[INFO] - Game play for EM table ") + rom + " is over");
                std::cout << "[INFO] - Game play for EM table " << rom << " is over" << std::endl;
            }
            gameOverLast = m_gameOver;

            nlohmann::json state;
            state["rom"] = rom;
            state["player"] = "player" + std::to_string(currentPlayer);
            state["score"] = scores[currentPlayer - 1];
            state["ball"] = m_ballInPlay;
            state["is_game_over"] = m_gameOver;
            state["scores"] = scores;
            state["credits"] = m_credits;
            state["tilt"] = m_tilt != 0;
            state["can_play"] = m_canPlay;
            jsonStr = state.dump();

            events.swap(m_pendingEvents);
        }

        bool changed = (jsonStr != m_lastJsonState);
        if (changed || !events.empty())
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            if (changed)
            {
                m_lastJsonState = jsonStr;
                for (struct mg_connection* conn : m_clients)
                    mg_ws_send(conn, jsonStr.c_str(), jsonStr.length(), WEBSOCKET_OP_TEXT);
            }
            for (const std::string& evt : events)
                for (struct mg_connection* conn : m_clients)
                    mg_ws_send(conn, evt.c_str(), evt.length(), WEBSOCKET_OP_TEXT);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

nlohmann::json B2STracker::BuildCompletedGameStateLocked() const
{
    nlohmann::json gameState = nlohmann::json::object();
    if (m_tilt != 0)
        gameState["tilt"] = true;
    return gameState;
}

void B2STracker::SendSummaryLocked(bool writeScoresFile)
{
    const std::string rom = m_tableName.empty() ? (m_gameId.empty() ? "em_table" : m_gameId) : m_tableName;
    const int playerCount = (std::max)(m_maxPlayerSeen, 1);

    nlohmann::json summary;
    summary["event"] = "game_summary";
    summary["rom"] = rom;
    summary["table"] = rom;

    auto now = std::chrono::steady_clock::now();
    const int64_t duration = std::chrono::duration_cast<std::chrono::seconds>(now - m_gameStartRealTime).count();
    summary["duration_seconds"] = duration;

    const std::vector<int64_t> finalScores(m_scores.begin(), m_scores.begin() + playerCount);
    summary["final_scores"] = finalScores;
    summary["highest_scores"] = std::vector<int64_t>(m_highestScores.begin(), m_highestScores.begin() + playerCount);
    summary["max_game_state"] = nlohmann::json::object();

    if (writeScoresFile) {
        CompletedGameRecord record;
        record.tablePath = m_tablePath;
        record.rom = rom;
        record.scores = finalScores;
        record.gameDuration = duration;
        record.gameState = BuildCompletedGameStateLocked();
        ScoresFileWriter::AppendCompletedGame(record);
    }

    const std::string jsonStr = summary.dump();
    std::cout << "[INFO] - EM game summary payload: " << jsonStr << std::endl;
    LPI_LOGI_CPP(std::string("[INFO] - EM game summary payload: ") + jsonStr);

    // Queued instead of sent inline: the websocket clients list has its own
    // lock and the loop/end-of-game paths already hold the state lock here.
    m_pendingEvents.push_back(jsonStr);
}

void B2STracker::StartWebServer(int port)
{
    m_mgr = new mg_mgr();
    mg_mgr_init(m_mgr);
    m_mgr->userdata = this;

    std::string listenUrl = "http://0.0.0.0:" + std::to_string(port);
    struct mg_connection* c = mg_http_listen(m_mgr, listenUrl.c_str(), B2SWebEventHandler, nullptr);
    if (c == nullptr) {
        std::cerr << "[ScoreTracker] EM web server failed to listen on port " << port << std::endl;
        delete m_mgr;
        m_mgr = nullptr;
        return;
    }

    std::cout << "[ScoreTracker] EM WebServer listening on ws://0.0.0.0:" << port << "/ws" << std::endl;
    m_webRunning = true;
    m_webThread = std::thread([this]() {
        while (m_webRunning) {
            mg_mgr_poll(m_mgr, 50);
        }
        mg_mgr_free(m_mgr);
        delete m_mgr;
        m_mgr = nullptr;
    });
}

void B2STracker::StopWebServer()
{
    m_webRunning = false;
    if (m_webThread.joinable())
        m_webThread.join();
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.clear();
    }
}

} // namespace ScoreTrackerPlugin
