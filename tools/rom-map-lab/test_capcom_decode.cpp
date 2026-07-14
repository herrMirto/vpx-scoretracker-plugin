// Decode test for the Capcom most-recent-first final_scores support: feeds NvramTracker real
// post-game .nv files through stubbed libpinmame calls and asserts the recorded player scores.
// Build (from repo root; paths reference this machine's vpinball checkout, maps fork and tables):
//   c++ -std=gnu++20 -O1 -I ~/vpinball/third-party/include -I ~/vpinball/plugins -I . \
//     tools/rom-map-lab/test_capcom_decode.cpp NvramTracker.cpp ScoresFileWriter.cpp common.cpp \
//     -o /tmp/test_capcom && /tmp/test_capcom
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdarg>
#include "common.h"
#define private public
#include "NvramTracker.h"
#undef private

#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdarg>

namespace ScoreTracker
{
void LPILog(const char* func, int line, const unsigned int level, const char* fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   vprintf(fmt, args);
   va_end(args);
   printf("\n");
}
}

static std::vector<uint8_t> g_nv;
static std::vector<uint8_t> g_ram(0x80000, 0);

extern "C" {
int PinmameIsRunning() { return 1; }
int PinmameGetMaxNVRAM() { return (int)g_nv.size(); }
int PinmameGetNVRAM(PinmameNVRAMState* s)
{
   for (size_t i = 0; i < g_nv.size(); ++i) { s[i].nvramNo = (int)i; s[i].currStat = g_nv[i]; s[i].oldStat = 0; }
   return (int)g_nv.size();
}
int PinmameReadMainCPUByte(const uint32_t addr, uint8_t* v)
{
   if (addr < g_ram.size()) { *v = g_ram[addr]; return 1; }
   if (addr >= 0x30000000u && addr - 0x30000000u < g_nv.size()) { *v = g_nv[addr - 0x30000000u]; return 1; }
   return 0;
}
}

using ScoreTracker::NvramTracker;

int main()
{
   std::ifstream f("/Users/andremichi/tables/Big Bang Bar (Capcom 1996)/pinmame/nvram/bbb109.nv", std::ios::binary);
   g_nv.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
   printf("nv bytes: %zu\n", g_nv.size());
   assert(g_nv.size() > 0x2080);

   NvramTracker t;
   bool ok = t.Start("bbb109", "/Users/andremichi/workspace/pinmame-nvram-maps-andre", "/tmp/tbl.vpx", "/tmp");
   assert(ok);
   assert(t.m_finalScoresMostRecentFirst);
   assert(t.m_finalScoresDesc.size() == 4);

   // one poll so m_nvram fills from the stubbed snapshot
   g_ram[0x12F2] = 0x40; // in-game
   g_ram[0x620EA] = 2;   // 2 players
   t.Poll();

   size_t auth = 0;

   // mid-session (no game over observed): ring must NOT be consulted
   t.m_maxGameStateValues["player_count"] = 2;
   std::vector<int64_t> live = { 123456 };
   auto snap = t.BuildFinalScoresSnapshot(live, false, auth);
   assert(auth == 0 && snap.size() == 1 && snap[0] == 123456);
   printf("mid-session passthrough OK\n");

   // 2-player game over: expect [P1=10241410, P2=22878190]
   snap = t.BuildFinalScoresSnapshot(live, true, auth);
   printf("2P snapshot: auth=%zu scores=[%lld, %lld]\n", auth, (long long)snap[0], (long long)(snap.size() > 1 ? snap[1] : -1));
   assert(auth == 2 && snap.size() == 2);
   assert(snap[0] == 28744730 && snap[1] == 14990600);

   // 1-player game over: newest entry only
   t.m_maxGameStateValues["player_count"] = 1;
   snap = t.BuildFinalScoresSnapshot(live, true, auth);
   assert(auth == 1 && snap.size() == 1 && snap[0] == 14990600);
   printf("1P snapshot OK: [%lld]\n", (long long)snap[0]);

   // missing player_count + baseline unchanged since session start: no pushes happened,
   // so nothing is attributed (live snapshot passthrough)
   t.m_maxGameStateValues.erase("player_count");
   snap = t.BuildFinalScoresSnapshot(live, true, auth);
   assert(auth == 0 && snap.size() == 1 && snap[0] == 123456);
   printf("no-count unchanged-list passthrough OK\n");

   // missing player_count AND no baseline: legacy newest-entry-only fallback
   t.m_hasFinalScoresBaseline = false;
   snap = t.BuildFinalScoresSnapshot(live, true, auth);
   assert(auth == 1 && snap.size() == 1 && snap[0] == 14990600);
   t.m_hasFinalScoresBaseline = true;
   printf("no-count no-baseline fallback OK\n");

   // 4-player claim clamps to ring size and reverses fully
   t.m_maxGameStateValues["player_count"] = 4;
   snap = t.BuildFinalScoresSnapshot(live, true, auth);
   assert(auth == 4 && snap.size() == 4);
   printf("4P snapshot: [%lld, %lld, %lld, %lld]\n", (long long)snap[0], (long long)snap[1], (long long)snap[2], (long long)snap[3]);
   assert(snap[0] == 4170370 && snap[1] == 44459640 && snap[2] == 28744730 && snap[3] == 14990600); // == the real recorded 4-player game

   // --- Breakshot: no player_count byte; count inferred from pushes onto the baseline ---
   {
      std::ifstream f2("/Users/andremichi/tables/Breakshot (Capcom 1996)/pinmame/nvram/bsv103.nv", std::ios::binary);
      g_nv.assign(std::istreambuf_iterator<char>(f2), std::istreambuf_iterator<char>());
      NvramTracker b;
      assert(b.Start("bsv103", "/Users/andremichi/workspace/pinmame-nvram-maps-andre", "/tmp/tbl.vpx", "/tmp"));
      assert(b.m_finalScoresMostRecentFirst && b.m_finalScoresDesc.size() == 4);
      g_ram.assign(g_ram.size(), 0);
      g_ram[0x12F2] = 0x40;
      b.Poll();
      size_t auth2 = 0;
      std::vector<int64_t> live2 = {};

      // real 4-player game: baseline = pre-game list, current nv has 4 pushes on top
      b.m_finalScoresBaseline = { 3114990, 3292410, 858390, 0 };
      b.m_hasFinalScoresBaseline = true;
      auto s2 = b.BuildFinalScoresSnapshot(live2, true, auth2);
      printf("bsv 4P inferred: auth=%zu [%lld, %lld, %lld, %lld]\n", auth2,
         (long long)s2[0], (long long)s2[1], (long long)s2[2], (long long)s2[3]);
      assert(auth2 == 4 && s2.size() == 4);
      assert(s2[0] == 765350 && s2[1] == 396250 && s2[2] == 272400 && s2[3] == 168090);

      // 1-push scenario: baseline already contains all but the newest entry
      b.m_finalScoresBaseline = { 272400, 396250, 765350, 3114990 };
      s2 = b.BuildFinalScoresSnapshot(live2, true, auth2);
      assert(auth2 == 1 && s2.size() == 1 && s2[0] == 168090);
      printf("bsv 1P inferred OK\n");

      // no push (aborted game): keep the live snapshot
      b.m_finalScoresBaseline = { 168090, 272400, 396250, 765350 };
      s2 = b.BuildFinalScoresSnapshot(live2, true, auth2);
      assert(auth2 == 0 && s2.empty());
      printf("bsv no-push passthrough OK\n");

      // no baseline captured: legacy newest-entry-only fallback
      b.m_hasFinalScoresBaseline = false;
      s2 = b.BuildFinalScoresSnapshot(live2, true, auth2);
      assert(auth2 == 1 && s2.size() == 1 && s2[0] == 168090);
      printf("bsv no-baseline fallback OK\n");

      // credits decode from the real nv (user reported 5 on the machine)
      const auto credits = b.m_decodedValues.find("credits");
      assert(credits != b.m_decodedValues.end() && credits->second == 5);
      printf("bsv credits=5 OK\n");
   }

   // --- Pinball Magic: fresh table (all-zero baseline), pure inference, real 4P game ---
   {
      std::ifstream f3("/Users/andremichi/tables/Pinball Magic (Capcom 1995)/pinmame/nvram/pmv112.nv", std::ios::binary);
      g_nv.assign(std::istreambuf_iterator<char>(f3), std::istreambuf_iterator<char>());
      NvramTracker p;
      assert(p.Start("pmv112", "/Users/andremichi/workspace/pinmame-nvram-maps-andre", "/tmp/tbl.vpx", "/tmp"));
      assert(p.m_finalScoresMostRecentFirst && p.m_finalScoresDesc.size() == 4);
      g_ram.assign(g_ram.size(), 0);
      g_ram[0x12F2] = 0x40;
      p.Poll();
      size_t auth3 = 0;
      std::vector<int64_t> live3 = {};
      p.m_finalScoresBaseline = { 0, 0, 0, 0 }; // list was empty before the first-ever game
      p.m_hasFinalScoresBaseline = true;
      auto s3 = p.BuildFinalScoresSnapshot(live3, true, auth3);
      printf("pmv 4P inferred: auth=%zu [%lld, %lld, %lld, %lld]\n", auth3,
         (long long)s3[0], (long long)s3[1], (long long)s3[2], (long long)s3[3]);
      assert(auth3 == 4 && s3.size() == 4);
      assert(s3[0] == 62576260 && s3[1] == 18396190 && s3[2] == 15376900 && s3[3] == 17265150);
      const auto credits = p.m_decodedValues.find("credits");
      assert(credits != p.m_decodedValues.end() && credits->second == 4);
      printf("pmv credits=4 OK\n");
   }

   printf("ALL TESTS PASSED\n");
   return 0;
}
