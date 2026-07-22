// license:GPLv3+

#include "ScoresFileWriter.h"
#include "ScoreSignature.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ScoreTracker
{

string ScoresFileWriter::CurrentUtcTimestamp()
{
   const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

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

string ScoresFileWriter::BrokenBackupSuffix()
{
   string suffix = CurrentUtcTimestamp();
   for (char& ch : suffix)
      if (ch == ':' || ch == '-')
         ch = '_';
   return suffix;
}

nlohmann::json ScoresFileWriter::BuildGameObject(const CompletedGameRecord& record)
{
   nlohmann::json game;
   const string date = CurrentUtcTimestamp();
   game["date"] = date;
   if (!record.rom.empty())
      game["rom"] = record.rom;
   game["scores"] = record.scores;
   game["game_duration"] = record.gameDuration;

   if (record.gameState.is_object() && !record.gameState.empty())
      game["game_state"] = record.gameState;

   const ScoreSignatureFields fields { date, record.rom, record.scores, record.gameDuration };
   string signature;
   if (SignScore(fields, signature))
   {
      game["signature"] = {
         { "algorithm", kScoreSignatureAlgorithm },
         { "key_id", kScoreSignatureKeyId },
         { "value", signature }
      };
   }

   return game;
}

bool ScoresFileWriter::AppendCompletedGame(const CompletedGameRecord& record)
{
   if (record.tablePath.empty() && record.outputPath.empty())
   {
      LOGI("Not writing scores.json because the table path is unknown");
      return false;
   }
   if (record.scores.empty())
   {
      LOGI("Not writing scores.json because no final scores were captured");
      return false;
   }

   const std::filesystem::path outputDir = record.outputPath.empty() ? std::filesystem::path(record.tablePath).parent_path() : std::filesystem::path(record.outputPath);
   const std::filesystem::path scoresPath = outputDir / "scores.json";
   const std::filesystem::path tmpPath = scoresPath.string() + ".tmp";

   nlohmann::json root;
   if (std::filesystem::exists(scoresPath))
   {
      std::ifstream in(scoresPath);
      try
      {
         in >> root;
      }
      catch (const std::exception& e)
      {
         // Never destroy user data, even invalid: move it aside and start a fresh file
         const std::filesystem::path backupPath = scoresPath.string() + ".broken." + BrokenBackupSuffix();
         std::error_code ec;
         std::filesystem::rename(scoresPath, backupPath, ec);
         if (ec)
         {
            LOGE("Failed to parse and back up %s: %s", scoresPath.string().c_str(), e.what());
            return false;
         }
         LOGW("Existing scores.json was invalid and was moved to %s", backupPath.string().c_str());
         root = nlohmann::json::object();
      }
   }

   if (!root.is_object())
      root = nlohmann::json::object();
   root["version"] = 1;
   if (!root.contains("games") || !root["games"].is_array())
      root["games"] = nlohmann::json::array();

   root["games"].push_back(BuildGameObject(record));

   {
      std::ofstream out(tmpPath, std::ios::trunc);
      if (!out.is_open())
      {
         LOGE("Could not open %s for writing", tmpPath.string().c_str());
         return false;
      }
      out << root.dump(2) << '\n';
      out.flush();
      if (!out.good())
      {
         LOGE("Failed while writing %s", tmpPath.string().c_str());
         return false;
      }
   }

   std::error_code ec;
   std::filesystem::rename(tmpPath, scoresPath, ec);
   if (ec)
   {
      // On Windows, rename does not replace an existing file
      std::filesystem::remove(scoresPath, ec);
      ec.clear();
      std::filesystem::rename(tmpPath, scoresPath, ec);
   }
   if (ec)
   {
      LOGE("Could not replace %s: %s", scoresPath.string().c_str(), ec.message().c_str());
      return false;
   }

   LOGI("Wrote completed game to %s", scoresPath.string().c_str());
   return true;
}

}
