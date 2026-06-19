// license:GPLv3+

#include "ScoresFileWriter.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "plugins/LoggingPlugin.h"

using namespace std::string_literals;

namespace ScoreTrackerPlugin {

LPI_USE_CPP();

std::string ScoresFileWriter::CurrentUtcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);

    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string ScoresFileWriter::BrokenBackupSuffix()
{
    std::string suffix = CurrentUtcTimestamp();
    for (char& ch : suffix) {
        if (ch == ':' || ch == '-')
            ch = '_';
    }
    return suffix;
}

nlohmann::json ScoresFileWriter::BuildSignature(const nlohmann::json& root, const nlohmann::json& game)
{
    (void)root;
    (void)game;

    // Official release signing belongs here. The GitHub workflow can inject
    // release key material/build identity, and this function can then return:
    // { "alg": "ed25519", "key": "...", "prev": "...", "value": "..." }.
    //
    // Development builds deliberately omit _signature instead of writing a
    // misleading fake signature.
    return nlohmann::json::object();
}

nlohmann::json ScoresFileWriter::BuildGameObject(const CompletedGameRecord& record)
{
    nlohmann::json game;
    game["date"] = CurrentUtcTimestamp();
    if (!record.rom.empty())
        game["rom"] = record.rom;
    game["scores"] = record.scores;
    game["game_duration"] = record.gameDuration;

    if (record.gameState.is_object() && !record.gameState.empty())
        game["game_state"] = record.gameState;

    return game;
}

bool ScoresFileWriter::AppendCompletedGame(const CompletedGameRecord& record)
{
    if (record.tablePath.empty()) {
        LPI_LOGI_CPP("[INFO] - ScoreTracker: not writing scores.json because table path is unknown"s);
        return false;
    }
    if (record.scores.empty()) {
        LPI_LOGI_CPP("[INFO] - ScoreTracker: not writing scores.json because no final scores were captured"s);
        return false;
    }

    const std::filesystem::path tablePath(record.tablePath);
    const std::filesystem::path scoresPath = tablePath.parent_path() / "scores.json";
    const std::filesystem::path tmpPath = scoresPath.string() + ".tmp";

    nlohmann::json root;
    if (std::filesystem::exists(scoresPath)) {
        std::ifstream in(scoresPath);
        try {
            in >> root;
        } catch (const std::exception& e) {
            const std::filesystem::path backupPath = scoresPath.string() + ".broken." + BrokenBackupSuffix();
            std::error_code ec;
            std::filesystem::rename(scoresPath, backupPath, ec);
            if (ec) {
                std::cerr << "[ScoreTracker] Could not parse or back up " << scoresPath.string() << ": " << e.what() << std::endl;
                LPI_LOGI_CPP(std::string("[INFO] - ScoreTracker: failed to parse and back up scores.json: ") + e.what());
                return false;
            }
            std::cerr << "[ScoreTracker] Existing scores.json was invalid and was moved to " << backupPath.string() << std::endl;
            root = nlohmann::json::object();
        }
    }

    if (!root.is_object())
        root = nlohmann::json::object();
    root["version"] = 1;
    if (!root.contains("games") || !root["games"].is_array())
        root["games"] = nlohmann::json::array();

    nlohmann::json game = BuildGameObject(record);
    nlohmann::json signature = BuildSignature(root, game);
    if (signature.is_object() && !signature.empty())
        game["_signature"] = signature;
    root["games"].push_back(game);

    {
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[ScoreTracker] Could not open " << tmpPath.string() << " for writing" << std::endl;
            return false;
        }
        out << root.dump(2) << '\n';
        out.flush();
        if (!out.good()) {
            std::cerr << "[ScoreTracker] Failed while writing " << tmpPath.string() << std::endl;
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, scoresPath, ec);
    if (ec) {
        std::filesystem::remove(scoresPath, ec);
        ec.clear();
        std::filesystem::rename(tmpPath, scoresPath, ec);
    }
    if (ec) {
        std::cerr << "[ScoreTracker] Could not replace " << scoresPath.string() << ": " << ec.message() << std::endl;
        return false;
    }

    std::cout << "[ScoreTracker] Wrote completed game to " << scoresPath.string() << std::endl;
    LPI_LOGI_CPP(std::string("[INFO] - ScoreTracker: wrote completed game to ") + scoresPath.string());
    return true;
}

} // namespace ScoreTrackerPlugin
