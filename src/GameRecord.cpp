// license:GPLv3+
#include "GameRecord.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <regex>
#include <set>
#include <sstream>

namespace ScoreTracker {
std::filesystem::path Utf8Path(const std::string& path) { return std::filesystem::path(std::u8string(path.begin(), path.end())); }
std::string PathUtf8(const std::filesystem::path& path) {
   const auto bytes = path.u8string();
   return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string NewGameId() {
   std::random_device random;
   std::ostringstream out;
   for (int i = 0; i < 4; ++i) out << std::hex << std::setw(8) << std::setfill('0') << uint32_t(random());
   return out.str();
}
int64_t UnixMilliseconds() {
   return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t MonotonicMilliseconds() {
   return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::string Timestamp(int64_t ms) {
   const std::time_t seconds = ms / 1000;
   std::tm tm{};
#ifdef _WIN32
   gmtime_s(&tm, &seconds);
#else
   gmtime_r(&seconds, &tm);
#endif
   std::ostringstream out;
   out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms % 1000 << 'Z';
   return out.str();
}
nlohmann::ordered_json GameJson(const CompletedGameRecord& g) {
   nlohmann::ordered_json j = {{"id", g.id}, {"table", PathUtf8(Utf8Path(g.tablePath).filename())}};
   if (!g.rom.empty()) j["rom"] = g.rom;
   j["started_at"] = Timestamp(g.startedUnixMs);
   j["ended_at"] = Timestamp(g.endedUnixMs);
   j["duration_ms"] = g.durationMs;
   j["source"] = g.source;
   j["scores"] = nlohmann::ordered_json::array();
   for (size_t i = 0; i < g.scores.size(); ++i) j["scores"].push_back({{"player", i + 1}, {"score", g.scores[i]}});
   return j;
}
bool ValidGameJson(const nlohmann::json& g) {
   if (!g.is_object()) return false;
   static const std::set<std::string> fields {"id", "table", "rom", "started_at", "ended_at", "duration_ms", "source", "scores"};
   for (auto it = g.begin(); it != g.end(); ++it) if (!fields.contains(it.key())) return false;
   const auto text = [&](const char* key) { return g.contains(key) && g[key].is_string() && !g[key].get_ref<const std::string&>().empty(); };
   if (!text("id") || !text("table") || !text("started_at") || !text("ended_at") || !text("source")) return false;
   const auto id = g["id"].get<std::string>();
   if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return false;
   if (g["source"] != "pinmame" && g["source"] != "table_script") return false;
   const std::string table = g["table"].get<std::string>();
   if (table.find_first_of("/\\") != std::string::npos) return false;
   static const std::regex timestamp(R"(^[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])T([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]\.[0-9]{3}Z$)");
   const auto validTime = [&](const std::string& time) {
      if (!std::regex_match(time, timestamp)) return false;
      const std::chrono::year_month_day date {std::chrono::year(std::stoi(time.substr(0, 4))),
         std::chrono::month(unsigned(std::stoi(time.substr(5, 2)))),
         std::chrono::day(unsigned(std::stoi(time.substr(8, 2))))};
      return date.ok();
   };
   if (!validTime(g["started_at"].get<std::string>()) || !validTime(g["ended_at"].get<std::string>())) return false;
   if (g["source"] == "pinmame" && !text("rom")) return false;
   if (g.contains("rom") && (!g["rom"].is_string() || g["rom"].get_ref<const std::string&>().empty())) return false;
   if (!g.contains("duration_ms") || !g["duration_ms"].is_number_integer() || g["duration_ms"].get<int64_t>() < 0) return false;
   if (!g.contains("scores") || !g["scores"].is_array() || g["scores"].empty() || g["scores"].size() > 8) return false;
   unsigned player = 0;
   for (const auto& s : g["scores"]) {
      if (!s.is_object() || s.size() != 2 || !s.contains("player") || !s["player"].is_number_integer()
         || s["player"].get<int64_t>() != ++player || !s.contains("score")
         || !s["score"].is_number_integer() || s["score"].get<int64_t>() < 0) return false;
   }
   return true;
}
}
