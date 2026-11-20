// license:GPLv3+
#include "ScoresFileWriter.h"
#include "common.h"
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace ScoreTracker {
namespace {
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;
class HistoryLock {
public:
   explicit HistoryLock(const fs::path& directory) {
#ifdef _WIN32
      m_handle = CreateFileW((directory / ".scoretracker.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      OVERLAPPED overlap{};
      if (m_handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open history lock");
      if (!LockFileEx(m_handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap)) {
         CloseHandle(m_handle); m_handle = INVALID_HANDLE_VALUE;
         throw std::runtime_error("Cannot lock history");
      }
#else
      m_fd = open((directory / ".scoretracker.lock").c_str(), O_CREAT | O_RDWR, 0600);
      if (m_fd < 0) throw std::system_error(errno, std::generic_category(), "Cannot open history lock");
      if (flock(m_fd, LOCK_EX) != 0) {
         const int error = errno; close(m_fd); m_fd = -1;
         throw std::system_error(error, std::generic_category(), "Cannot lock history");
      }
#endif
   }
   ~HistoryLock() {
#ifdef _WIN32
      if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
#else
      if (m_fd >= 0) { flock(m_fd, LOCK_UN); close(m_fd); }
#endif
   }
   HistoryLock(const HistoryLock&) = delete;
   HistoryLock& operator=(const HistoryLock&) = delete;
private:
#ifdef _WIN32
   HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
   int m_fd = -1;
#endif
};
void SyncDirectory(const fs::path& path) {
#ifndef _WIN32
   const int fd = open(path.c_str(), O_RDONLY);
   if (fd < 0) throw std::system_error(errno, std::generic_category(), "Cannot open history directory");
   const int result = fsync(fd), error = errno;
   close(fd);
   if (result != 0) throw std::system_error(error, std::generic_category(), "Cannot sync history directory");
#endif
}
void AtomicWrite(const fs::path& target, const Json& document) {
   const auto temp = target.parent_path() / (target.filename().string() + "." + NewGameId() + ".tmp");
   const std::string bytes = document.dump(2) + '\n';
   try {
#ifdef _WIN32
      HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create temporary history");
      size_t offset = 0;
      bool good = true;
      while (offset < bytes.size()) {
         DWORD written = 0;
         const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
         if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) { good = false; break; }
         offset += written;
      }
      good = good && FlushFileBuffers(file);
      const bool closed = CloseHandle(file);
      if (!good || !closed) throw std::runtime_error("Cannot flush temporary history");
      if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
         throw std::runtime_error("Cannot replace history");
#else
      const int fd = open(temp.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
      if (fd < 0) throw std::system_error(errno, std::generic_category(), "Cannot create temporary history");
      size_t offset = 0;
      int error = 0;
      while (offset < bytes.size()) {
         const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
         if (written < 0 && errno == EINTR) continue;
         if (written <= 0) { error = written < 0 ? errno : EIO; break; }
         offset += size_t(written);
      }
      if (error == 0 && fsync(fd) != 0) error = errno;
      if (close(fd) != 0 && error == 0) error = errno;
      if (error) throw std::system_error(error, std::generic_category(), "Cannot flush temporary history");
      fs::rename(temp, target);
      SyncDirectory(target.parent_path());
#endif
   } catch (...) {
      std::error_code ignored; fs::remove(temp, ignored);
      throw;
   }
}
Json Read(const fs::path& path) {
   std::ifstream in(path, std::ios::binary);
   if (!in) throw std::runtime_error("Cannot read " + path.string());
   bool duplicate = false;
   std::vector<std::set<std::string>> objects;
   auto value = Json::parse(in, [&](int, Json::parse_event_t event, Json& parsed) {
      if (event == Json::parse_event_t::object_start) objects.emplace_back();
      else if (event == Json::parse_event_t::object_end) objects.pop_back();
      else if (event == Json::parse_event_t::key && !objects.back().insert(parsed.get<std::string>()).second) duplicate = true;
      return true;
   });
   if (duplicate) throw std::runtime_error("Duplicate JSON keys; original preserved");
   return value;
}
Json ReadStoredGame(const fs::path& path) {
   Json game = Read(path);
   // Ignore retired descriptive metadata in existing journals. New records never
   // serialize it; scores and stable ids retain their original meaning.
   if (game.is_object()) game.erase("evidence");
   return game;
}
Json ReadHistory(const fs::path& path, StorageReport* report = nullptr) {
   if (!fs::exists(path)) return {{"schema_version", 1}, {"games", Json::array()}};
   Json document = Read(path);
   // The recognizable experimental format is archived, never migrated. Preflight
   // is read-only; only a real completed-game write under HistoryLock renames it.
   if (document.is_object() && document.size() == 1 && document.contains("games") && document["games"].is_array()) {
      if (report) {
         const auto backup = path.parent_path() / ("scores.legacy-" + NewGameId() + ".json");
         fs::rename(path, backup);
         report->archivedHistory = backup;
         SyncDirectory(path.parent_path());
         LOGI("Experimental history preserved at %s; creating a new v1 history without migration", PathUtf8(backup).c_str());
      }
      return {{"schema_version", 1}, {"games", Json::array()}};
   }
   if (!document.is_object() || document.size() != 2 || !document.contains("schema_version")
      || !document["schema_version"].is_number_integer() || document["schema_version"] != 1
      || !document.contains("games") || !document["games"].is_array())
      throw std::runtime_error("Unsupported or malformed history; original preserved");
   std::set<std::string> ids;
   for (auto& game : document["games"]) {
      if (game.is_object()) game.erase("evidence");
      if (!ValidGameJson(game) || !ids.insert(game["id"].template get<std::string>()).second)
         throw std::runtime_error("Invalid or duplicate game in history; original preserved");
   }
   return document;
}
bool Insert(Json& history, const Json& game) {
   for (const auto& existing : history["games"]) {
      if (existing["id"] != game["id"]) continue;
      if (nlohmann::json(existing) != nlohmann::json(game)) throw std::runtime_error("Conflicting completed-game id");
      return false;
   }
   history["games"].push_back(game);
   return true;
}
void Drain(const fs::path& directory, Json& history, StorageReport& report, const std::string& requestedId = {}) {
   const auto pending = directory / ".scoretracker-pending";
   if (!fs::exists(pending)) return;
   for (const auto& file : fs::directory_iterator(pending)) {
      if (file.path().extension() != ".json") continue;
      const auto game = ReadStoredGame(file.path());
      if (!ValidGameJson(game) || file.path().stem() != game["id"].get<std::string>())
         throw std::runtime_error("Invalid pending game; original preserved");
      // Rewrite even on duplicate: establish durability before discarding the journal.
      const bool inserted = Insert(history, game);
      AtomicWrite(directory / "scores.json", history);
      if (inserted) ++report.savedGames;
      if (game["id"] == requestedId) report.requestedGameSaved = true;
      fs::remove(file.path());
      SyncDirectory(pending);
   }
}
}
bool ScoresFileWriter::AppendCompletedGame(const CompletedGameRecord& record, StorageReport* output) {
   StorageReport local;
   StorageReport& report = output ? *output : local;
   report = {};
   try {
      if (record.tablePath.empty() || record.startedUnixMs < 0 || record.endedUnixMs < 0 || record.durationMs < 0)
         throw std::runtime_error("Missing table or invalid completed-game timestamps");
      const auto game = GameJson(record);
      if (!ValidGameJson(game)) throw std::runtime_error("Invalid completed game");
      const fs::path directory = record.outputPath.empty() ? Utf8Path(record.tablePath).parent_path() : Utf8Path(record.outputPath);
      if (directory.empty()) throw std::runtime_error("Unknown output directory");
      fs::create_directories(directory);
      HistoryLock lock(directory);
      // Journal the result before inspecting history: malformed histories must not
      // prevent recovery of newly completed games after the user repairs the file.
      const auto pending = directory / ".scoretracker-pending";
      fs::create_directories(pending);
      SyncDirectory(directory);
      const auto entry = pending / (record.id + ".json");
      if (fs::exists(entry)) {
         if (nlohmann::json(ReadStoredGame(entry)) != nlohmann::json(game)) throw std::runtime_error("Conflicting pending id");
      } else AtomicWrite(entry, game);
      Json history = ReadHistory(directory / "scores.json", &report);
      Drain(directory, history, report, record.id);
      return true;
   } catch (const std::exception& e) {
      report.error = e.what();
      LOGE("Completed-game write failed for table %s (output %s): %s", record.tablePath.c_str(), record.outputPath.c_str(), e.what());
      return false;
   }
}
bool ScoresFileWriter::RetryPending(const fs::path& directory, StorageReport* output) {
   StorageReport local;
   StorageReport& report = output ? *output : local;
   report = {};
   try {
      if (directory.empty() || !fs::exists(directory)) return true;
      const auto pending = directory / ".scoretracker-pending";
      if (!fs::exists(pending)) return true;
      bool hasPendingGames = false;
      for (const auto& file : fs::directory_iterator(pending))
         if (file.path().extension() == ".json") { hasPendingGames = true; break; }
      if (!hasPendingGames) return true;
      HistoryLock lock(directory);
      auto history = ReadHistory(directory / "scores.json", &report);
      Drain(directory, history, report);
      return true;
   } catch (const std::exception& e) {
      report.error = e.what();
      LOGE("Pending games could not be recovered from %s: %s", PathUtf8(directory).c_str(), e.what());
      return false;
   }
}
bool ScoresFileWriter::CheckHistory(const fs::path& directory, StorageReport* output) {
   StorageReport local;
   StorageReport& report = output ? *output : local;
   report = {};
   try {
      if (!directory.empty()) ReadHistory(directory / "scores.json");
      return true;
   } catch (const std::exception& e) {
      report.error = e.what();
      LOGE("History is not writable in its current format at %s: %s", PathUtf8(directory / "scores.json").c_str(), e.what());
      return false;
   }
}
}
