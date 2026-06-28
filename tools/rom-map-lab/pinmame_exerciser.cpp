// Experimental, standalone libpinmame ROM exerciser.
//
// This is intentionally not part of the ScoreTracker plugin runtime. It boots a
// ROM through libpinmame, pulses switches, captures NVRAM, and writes JSONL
// telemetry that can be analyzed by map_lab.py or ad-hoc scripts.

#include "libpinmame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

enum class ActionType { Pulse, Set, Wait, StopIfNv, StopIfScoreStable };

struct Action {
  ActionType type;
  int value1 = 0;
  int value2 = 0;
  int value3 = 0;
  int pulseMs = -1;
  int settleMs = -1;
};

struct Options {
  std::string rom;
  std::string vpmPath;
  std::string outDir;
  int bootMs = 5000;
  int settleMs = 250;
  int holdDelayMs = 500;
  int pulseMs = 100;
  int switchMin = 1;
  int switchMax = 128;
  int activeState = 1;
  int keyPulseMs = 120;
  int startGapMs = 2500;
  int coins = 0;
  int starts = 0;
  bool fuzz = false;
  bool coinStart = false;
  std::string recipe;
  std::vector<int> pulses;
  std::vector<int> holdSwitches;
  std::vector<std::pair<int, int>> postStartSwitchSets;
  std::vector<Action> actions;
  std::vector<PINMAME_KEYCODE> keyPulses;
  std::vector<PINMAME_KEYCODE> holdKeys;
};

static std::atomic<int> g_state{0};
static bool g_quietLogs = false;
static bool g_dumpRam = false;
static bool g_ramRegionsListed = false;
static std::vector<std::pair<size_t, size_t>> g_ramWindows;  // (start, length) within a region
static std::vector<std::pair<size_t, size_t>> g_cpuWindows;  // (cpu_address, length) via CPU map
static std::set<PINMAME_KEYCODE> g_pressedKeys;

static std::string JsonEscape(const std::string& s) {
  std::ostringstream out;
  for (char c : s) {
    switch (c) {
    case '\\': out << "\\\\"; break;
    case '"': out << "\\\""; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(static_cast<unsigned char>(c));
      } else {
        out << c;
      }
    }
  }
  return out.str();
}

static void PrintEvent(const std::string& json) {
  std::cout << json << std::endl;
}

static void PINMAMECALLBACK OnStateUpdated(int state, void*) {
  g_state.store(state);
  std::ostringstream out;
  out << "{\"event\":\"state\",\"state\":" << state << "}";
  PrintEvent(out.str());
}

static int PINMAMECALLBACK OnAudioAvailable(PinmameAudioInfo* info, void*) {
  return info ? info->samplesPerFrame : 0;
}

static int PINMAMECALLBACK OnAudioUpdated(void*, int samples, void*) {
  return samples;
}

static int PINMAMECALLBACK IsKeyPressed(PINMAME_KEYCODE keycode, void*) {
  return g_pressedKeys.count(keycode) ? 1 : 0;
}

static bool ParseKeyCode(const std::string& raw, PINMAME_KEYCODE& out) {
  std::string key = raw;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
    out = static_cast<PINMAME_KEYCODE>(PINMAME_KEYCODE_NUMBER_0 + (key[0] - '0'));
    return true;
  }
  if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
    out = static_cast<PINMAME_KEYCODE>(PINMAME_KEYCODE_A + (key[0] - 'A'));
    return true;
  }
  if (key == "START" || key == "1") { out = PINMAME_KEYCODE_NUMBER_1; return true; }
  if (key == "COIN" || key == "COIN1" || key == "5") { out = PINMAME_KEYCODE_NUMBER_5; return true; }
  if (key == "ENTER") { out = PINMAME_KEYCODE_ENTER; return true; }
  if (key == "KEYPADENTER" || key == "KEYPAD_ENTER" || key == "LAUNCH" || key == "PLUNGER") {
    out = PINMAME_KEYCODE_KEYPAD_ENTER;
    return true;
  }
  if (key == "END" || key == "COINDOOR" || key == "COIN_DOOR") { out = PINMAME_KEYCODE_END; return true; }
  if (key == "ESC" || key == "ESCAPE") { out = PINMAME_KEYCODE_ESCAPE; return true; }
  if (key == "SPACE") { out = PINMAME_KEYCODE_SPACE; return true; }
  return false;
}

static void PINMAMECALLBACK OnLogMessage(PINMAME_LOG_LEVEL level, const char* format, va_list args, void*) {
  if (g_quietLogs && level == PINMAME_LOG_LEVEL_INFO)
    return;

  char buffer[2048];
  vsnprintf(buffer, sizeof(buffer), format, args);
  const char* levelName = level == PINMAME_LOG_LEVEL_ERROR ? "error" : (level == PINMAME_LOG_LEVEL_DEBUG ? "debug" : "info");
  std::ostringstream out;
  out << "{\"event\":\"log\",\"level\":\"" << levelName << "\",\"message\":\"" << JsonEscape(buffer) << "\"}";
  PrintEvent(out.str());
}

static void PINMAMECALLBACK OnGame(PinmameGame* game, void* userData) {
  if (!game)
    return;
  auto* found = static_cast<int*>(userData);
  *found = game->found;
  std::ostringstream out;
  out << "{\"event\":\"game\",\"name\":\"" << JsonEscape(game->name ? game->name : "")
      << "\",\"description\":\"" << JsonEscape(game->description ? game->description : "")
      << "\",\"manufacturer\":\"" << JsonEscape(game->manufacturer ? game->manufacturer : "")
      << "\",\"year\":\"" << JsonEscape(game->year ? game->year : "")
      << "\",\"found\":" << game->found << "}";
  PrintEvent(out.str());
}

static void SleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static std::vector<uint8_t> ReadNvram() {
  const int max = PinmameGetMaxNVRAM();
  if (max <= 0)
    return {};

  std::vector<PinmameNVRAMState> states(static_cast<size_t>(max));
  const int size = PinmameGetNVRAM(states.data());
  if (size <= 0)
    return {};

  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  for (int i = 0; i < size; ++i)
    bytes[static_cast<size_t>(i)] = states[static_cast<size_t>(i)].currStat;
  return bytes;
}

static size_t DiffCount(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t count = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i])
      ++count;
  }
  return count;
}

static std::vector<size_t> FirstDiffs(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t limit) {
  std::vector<size_t> diffs;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n && diffs.size() < limit; ++i) {
    if (a[i] != b[i])
      diffs.push_back(i);
  }
  return diffs;
}

static void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Dump raw CPU/RAM memory regions for each snapshot. NVRAM-only snapshots miss
// volatile state (e.g. SAM's game-in-play flag lives in CPU RAM, not the
// battery NVRAM). Regions larger than kMaxRamRegionBytes (ROM banks) are
// skipped so 100+ snapshots stay small. Region index/length is stable for a
// given ROM, so the per-region files line up positionally across snapshots.
static void DumpMemoryRegions(const fs::path& outDir, const std::string& label) {
  constexpr size_t kMaxRamRegionBytes = 0x40000;  // 256 KiB; keeps RAM/NVRAM, drops ROM
  constexpr int kMaxRegions = 32;
  std::ostringstream regions;
  bool first = true;
  for (int region = 0; region < kMaxRegions; ++region) {
    const uint8_t* data = PinmameGetRawMemoryRegion(region);
    const size_t len = PinmameGetRawMemoryRegionLength(region);
    if (data == nullptr || len == 0)
      continue;
    if (!g_ramRegionsListed) {
      if (!first)
        regions << ",";
      regions << "{\"region\":" << region << ",\"length\":" << len << "}";
      first = false;
    }
    // With explicit --ram-window slices, carve them out of this region (some
    // platforms, e.g. SAM, expose one huge flat region with RAM at fixed
    // addresses). Otherwise dump whole small regions (gts3-style).
    if (!g_ramWindows.empty()) {
      for (const auto& w : g_ramWindows) {
        const size_t start = w.first;
        const size_t wlen = w.second;
        if (start + wlen > len)
          continue;
        std::ostringstream name;
        name << label << ".region" << region << "_0x" << std::hex << start << ".bin";
        WriteBytes(outDir / name.str(),
                   std::vector<uint8_t>(data + start, data + start + wlen));
      }
      continue;
    }
    if (len > kMaxRamRegionBytes)
      continue;
    std::ostringstream name;
    name << label << ".region" << region << ".bin";
    WriteBytes(outDir / name.str(), std::vector<uint8_t>(data, data + len));
  }
  if (!g_ramRegionsListed) {
    g_ramRegionsListed = true;
    PrintEvent("{\"event\":\"ram_regions\",\"regions\":[" + regions.str() + "]}");
  }
}

// Dump CPU-address windows via PinmameReadMainCPUByte. Unlike raw memory
// regions, this honours the CPU memory map, so addresses match map offsets
// (e.g. SAM NVRAM at 0x02100000, work RAM at 0x0 / 0x300000).
static void DumpCpuWindows(const fs::path& outDir, const std::string& label) {
  for (const auto& w : g_cpuWindows) {
    std::vector<uint8_t> buf(w.second);
    for (size_t i = 0; i < w.second; ++i) {
      uint8_t v = 0;
      PinmameReadMainCPUByte(static_cast<uint32_t>(w.first + i), &v);
      buf[i] = v;
    }
    std::ostringstream name;
    name << label << ".cpu_0x" << std::hex << w.first << ".bin";
    WriteBytes(outDir / name.str(), buf);
  }
}

static std::vector<uint8_t> Snapshot(const fs::path& outDir, const std::string& label, const std::vector<uint8_t>* previous = nullptr) {
  std::vector<uint8_t> bytes = ReadNvram();
  fs::path file = outDir / (label + ".nv");
  if (!bytes.empty())
    WriteBytes(file, bytes);
  if (g_dumpRam)
    DumpMemoryRegions(outDir, label);
  if (!g_cpuWindows.empty())
    DumpCpuWindows(outDir, label);

  std::ostringstream out;
  out << "{\"event\":\"snapshot\",\"label\":\"" << JsonEscape(label)
      << "\",\"nvram_size\":" << bytes.size()
      << ",\"file\":\"" << JsonEscape(file.string()) << "\"";
  if (previous) {
    out << ",\"diff_count\":" << DiffCount(*previous, bytes);
    auto diffs = FirstDiffs(*previous, bytes, 16);
    out << ",\"first_diffs\":[";
    for (size_t i = 0; i < diffs.size(); ++i) {
      if (i)
        out << ",";
      out << diffs[i];
    }
    out << "]";
  }
  out << "}";
  PrintEvent(out.str());
  return bytes;
}

static void PulseSwitch(int sw, int activeState, int pulseMs, int settleMs) {
  const int inactiveState = activeState ? 0 : 1;
  PinmameSetSwitch(sw, activeState);
  SleepMs(pulseMs);
  PinmameSetSwitch(sw, inactiveState);
  SleepMs(settleMs);
}

static void PulseKey(PINMAME_KEYCODE key, int pulseMs, int settleMs) {
  g_pressedKeys.insert(key);
  SleepMs(pulseMs);
  g_pressedKeys.erase(key);
  SleepMs(settleMs);
}

static void Usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " --rom ROM [options]\n\n"
      << "Options:\n"
      << "  --vpm-path PATH       PinMAME root, default $HOME/.pinmame/\n"
      << "  --out-dir PATH        Output directory, default /tmp/pinmame-exercise-ROM\n"
      << "  --boot-ms N           Wait after boot, default 5000\n"
      << "  --settle-ms N         Wait after switch release, default 250\n"
      << "  --hold-delay-ms N      Delay initial held switches, default 500\n"
      << "  --pulse-ms N          Switch active duration, default 100\n"
      << "  --pulse-switch N      Pulse one switch; can be repeated\n"
      << "  --hold-switch N       Hold one switch active for the whole run; can be repeated\n"
      << "  --post-start-set-switch N=0|1\n"
      << "                        Set a switch after coins/starts/keys, before pulses; can repeat\n"
      << "  --action SPEC         Ordered action after normal pulses; repeatable:\n"
      << "                        pulse:N[:pulse_ms[:settle_ms]], set:N=0|1[:settle_ms], wait:ms,\n"
      << "                        stop-if-nv:OFFSET:MASK:0|1,\n"
      << "                        stop-if-score-stable:OFFSET:LENGTH:SWITCH[:pulse_ms[:settle_ms]]\n"
      << "  --press-key KEY       Press key; can be repeated. Supports 0-9, COIN, START\n"
      << "  --hold-key KEY        Hold key for the whole run; can be repeated\n"
      << "  --coin-start          Press COIN/5 then START/1 before switch pulses\n"
      << "  --coins N             Press COIN/5 N times before starts\n"
      << "  --starts N            Press START/1 N times after coins\n"
      << "  --start-gap-ms N      Extra wait between Start presses, default 2500\n"
      << "  --recipe NAME         Apply a preset, currently stern-sam-trough-launch\n"
      << "  --key-pulse-ms N      Key active duration, default 120\n"
      << "  --fuzz-switches A-B   Pulse all switches in range, e.g. 1-128\n"
      << "  --active-state 0|1    Active switch state, default 1\n"
      << "  --quiet-logs          Suppress libpinmame info logs\n"
      << "  --dump-ram            Also dump CPU/RAM regions per snapshot (<label>.regionN.bin)\n"
      << "  --ram-window S:L      Dump raw-region slice [S,S+L) (hex ok); repeatable; implies --dump-ram\n"
      << "  --cpu-window A:L      Dump CPU-address window [A,A+L) via PinmameReadMainCPUByte; repeatable\n";
}

static bool ParseRange(const std::string& s, int& lo, int& hi) {
  const size_t dash = s.find('-');
  try {
    if (dash == std::string::npos) {
      lo = hi = std::stoi(s);
    } else {
      lo = std::stoi(s.substr(0, dash));
      hi = std::stoi(s.substr(dash + 1));
    }
  } catch (...) {
    return false;
  }
  if (lo > hi)
    std::swap(lo, hi);
  return true;
}

static bool ParseSwitchSet(const std::string& s, int& sw, int& state) {
  const size_t equals = s.find('=');
  if (equals == std::string::npos)
    return false;
  try {
    sw = std::stoi(s.substr(0, equals));
    state = std::stoi(s.substr(equals + 1)) ? 1 : 0;
  } catch (...) {
    return false;
  }
  return sw > 0;
}

static std::vector<std::string> Split(const std::string& s, char delimiter) {
  std::vector<std::string> values;
  std::stringstream stream(s);
  std::string value;
  while (std::getline(stream, value, delimiter))
    values.push_back(value);
  return values;
}

static bool ParseAction(const std::string& raw, Action& action) {
  auto parts = Split(raw, ':');
  try {
    if (parts.size() == 2 && parts[0] == "wait") {
      action.type = ActionType::Wait;
      action.value1 = std::stoi(parts[1]);
      return action.value1 >= 0;
    }
    if (parts.size() >= 2 && parts.size() <= 4 && parts[0] == "pulse") {
      action.type = ActionType::Pulse;
      action.value1 = std::stoi(parts[1]);
      if (parts.size() >= 3)
        action.pulseMs = std::stoi(parts[2]);
      if (parts.size() >= 4)
        action.settleMs = std::stoi(parts[3]);
      return action.value1 > 0;
    }
    if (parts.size() >= 2 && parts.size() <= 3 && parts[0] == "set") {
      action.type = ActionType::Set;
      if (!ParseSwitchSet(parts[1], action.value1, action.value2))
        return false;
      if (parts.size() == 3)
        action.settleMs = std::stoi(parts[2]);
      return true;
    }
    if (parts.size() == 4 && parts[0] == "stop-if-nv") {
      action.type = ActionType::StopIfNv;
      action.value1 = static_cast<int>(std::stoul(parts[1], nullptr, 0));
      action.value2 = static_cast<int>(std::stoul(parts[2], nullptr, 0));
      action.pulseMs = std::stoi(parts[3]) ? 1 : 0;
      return action.value1 >= 0 && action.value2 > 0 && action.value2 <= 0xff;
    }
    if (parts.size() >= 4 && parts.size() <= 6 && parts[0] == "stop-if-score-stable") {
      action.type = ActionType::StopIfScoreStable;
      action.value1 = static_cast<int>(std::stoul(parts[1], nullptr, 0));
      action.value2 = std::stoi(parts[2]);
      action.value3 = std::stoi(parts[3]);
      if (parts.size() >= 5)
        action.pulseMs = std::stoi(parts[4]);
      if (parts.size() >= 6)
        action.settleMs = std::stoi(parts[5]);
      return action.value1 >= 0 && action.value2 > 0 && action.value3 > 0;
    }
  } catch (...) {
    return false;
  }
  return false;
}

static Options ParseArgs(int argc, char** argv) {
  Options opt;
  const char* home = getenv("HOME");
  opt.vpmPath = home ? std::string(home) + "/.pinmame/" : ".";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto requireValue = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        Usage(argv[0]);
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--rom")
      opt.rom = requireValue("--rom");
    else if (arg == "--vpm-path")
      opt.vpmPath = requireValue("--vpm-path");
    else if (arg == "--out-dir")
      opt.outDir = requireValue("--out-dir");
    else if (arg == "--boot-ms")
      opt.bootMs = std::stoi(requireValue("--boot-ms"));
    else if (arg == "--settle-ms")
      opt.settleMs = std::stoi(requireValue("--settle-ms"));
    else if (arg == "--hold-delay-ms")
      opt.holdDelayMs = std::stoi(requireValue("--hold-delay-ms"));
    else if (arg == "--pulse-ms")
      opt.pulseMs = std::stoi(requireValue("--pulse-ms"));
    else if (arg == "--key-pulse-ms")
      opt.keyPulseMs = std::stoi(requireValue("--key-pulse-ms"));
    else if (arg == "--coins")
      opt.coins = std::stoi(requireValue("--coins"));
    else if (arg == "--starts")
      opt.starts = std::stoi(requireValue("--starts"));
    else if (arg == "--start-gap-ms")
      opt.startGapMs = std::stoi(requireValue("--start-gap-ms"));
    else if (arg == "--recipe")
      opt.recipe = requireValue("--recipe");
    else if (arg == "--pulse-switch")
      opt.pulses.push_back(std::stoi(requireValue("--pulse-switch")));
    else if (arg == "--hold-switch")
      opt.holdSwitches.push_back(std::stoi(requireValue("--hold-switch")));
    else if (arg == "--post-start-set-switch") {
      int sw = 0;
      int state = 0;
      std::string value = requireValue("--post-start-set-switch");
      if (!ParseSwitchSet(value, sw, state)) {
        std::cerr << "Bad switch assignment, expected N=0 or N=1: " << value << "\n";
        std::exit(2);
      }
      opt.postStartSwitchSets.emplace_back(sw, state);
    }
    else if (arg == "--action") {
      Action action;
      std::string value = requireValue("--action");
      if (!ParseAction(value, action)) {
        std::cerr << "Bad action: " << value << "\n";
        std::exit(2);
      }
      opt.actions.push_back(action);
    }
    else if (arg == "--press-key") {
      PINMAME_KEYCODE code;
      std::string value = requireValue("--press-key");
      if (!ParseKeyCode(value, code)) {
        std::cerr << "Unsupported key: " << value << "\n";
        std::exit(2);
      }
      opt.keyPulses.push_back(code);
    } else if (arg == "--hold-key") {
      PINMAME_KEYCODE code;
      std::string value = requireValue("--hold-key");
      if (!ParseKeyCode(value, code)) {
        std::cerr << "Unsupported key: " << value << "\n";
        std::exit(2);
      }
      opt.holdKeys.push_back(code);
    } else if (arg == "--coin-start") {
      opt.coinStart = true;
    }
    else if (arg == "--fuzz-switches") {
      opt.fuzz = true;
      if (!ParseRange(requireValue("--fuzz-switches"), opt.switchMin, opt.switchMax)) {
        std::cerr << "Bad switch range\n";
        std::exit(2);
      }
    } else if (arg == "--active-state") {
      opt.activeState = std::stoi(requireValue("--active-state")) ? 1 : 0;
    } else if (arg == "--quiet-logs") {
      g_quietLogs = true;
    } else if (arg == "--dump-ram") {
      g_dumpRam = true;
    } else if (arg == "--ram-window") {
      g_dumpRam = true;
      std::string spec = requireValue("--ram-window");
      auto colon = spec.find(':');
      if (colon == std::string::npos) {
        std::cerr << "Bad --ram-window (want START:LEN)\n";
        std::exit(2);
      }
      size_t start = std::stoul(spec.substr(0, colon), nullptr, 0);
      size_t len = std::stoul(spec.substr(colon + 1), nullptr, 0);
      g_ramWindows.emplace_back(start, len);
    } else if (arg == "--cpu-window") {
      g_dumpRam = true;
      std::string spec = requireValue("--cpu-window");
      auto colon = spec.find(':');
      if (colon == std::string::npos) {
        std::cerr << "Bad --cpu-window (want ADDR:LEN)\n";
        std::exit(2);
      }
      size_t start = std::stoul(spec.substr(0, colon), nullptr, 0);
      size_t len = std::stoul(spec.substr(colon + 1), nullptr, 0);
      g_cpuWindows.emplace_back(start, len);
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      Usage(argv[0]);
      std::exit(2);
    }
  }

  if (opt.rom.empty()) {
    Usage(argv[0]);
    std::exit(2);
  }
  if (opt.outDir.empty())
    opt.outDir = "/tmp/pinmame-exercise-" + opt.rom;
  if (!opt.vpmPath.empty() && opt.vpmPath.back() != '/')
    opt.vpmPath.push_back('/');
  if (opt.coinStart) {
    opt.coins = std::max(opt.coins, 1);
    opt.starts = std::max(opt.starts, 1);
  }
  if (!opt.recipe.empty()) {
    if (opt.recipe == "stern-sam-trough-launch") {
      for (const char* key : {"E", "S", "D", "F", "G", "H", "J"}) {
        PINMAME_KEYCODE code;
        ParseKeyCode(key, code);
        opt.holdKeys.push_back(code);
      }
      opt.coins = std::max(opt.coins, 2);
      opt.starts = std::max(opt.starts, 1);
      PINMAME_KEYCODE launch;
      ParseKeyCode("LAUNCH", launch);
      opt.keyPulses.push_back(launch);
    } else {
      std::cerr << "Unknown recipe: " << opt.recipe << "\n";
      std::exit(2);
    }
  }
  return opt;
}

int main(int argc, char** argv) {
  Options opt = ParseArgs(argc, argv);
  fs::create_directories(opt.outDir);

  PinmameConfig config = {
      PINMAME_AUDIO_FORMAT_FLOAT,
      44100,
      "",
      &OnStateUpdated,
      nullptr,
      nullptr,
      &OnAudioAvailable,
      &OnAudioUpdated,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &IsKeyPressed,
      &OnLogMessage,
      nullptr,
  };
  snprintf((char*)config.vpmPath, PINMAME_MAX_PATH, "%s", opt.vpmPath.c_str());

  PrintEvent("{\"event\":\"config\",\"rom\":\"" + JsonEscape(opt.rom) +
             "\",\"vpm_path\":\"" + JsonEscape(opt.vpmPath) +
             "\",\"out_dir\":\"" + JsonEscape(opt.outDir) + "\"}");

  PinmameSetConfig(&config);
  PinmameSetCheat(0);
  PinmameSetHandleKeyboard((opt.coins > 0 || opt.starts > 0 || !opt.keyPulses.empty() || !opt.holdKeys.empty()) ? 1 : 0);
  PinmameSetHandleMechanics(0);
  PinmameSetDmdMode(PINMAME_DMD_MODE_RAW);

  int found = 0;
  PinmameGetGame(opt.rom.c_str(), &OnGame, &found);
  if (!found) {
    PrintEvent("{\"event\":\"error\",\"message\":\"ROM not found by PinMAME\"}");
    return 3;
  }

  PINMAME_STATUS status = PinmameRun(opt.rom.c_str());
  if (status != PINMAME_STATUS_OK) {
    std::ostringstream out;
    out << "{\"event\":\"error\",\"message\":\"PinmameRun failed\",\"status\":" << status << "}";
    PrintEvent(out.str());
    return 4;
  }

  if (!opt.holdSwitches.empty())
    SleepMs(opt.holdDelayMs);
  for (int sw : opt.holdSwitches) {
    PinmameSetSwitch(sw, opt.activeState);
    std::ostringstream out;
    out << "{\"event\":\"hold_switch\",\"switch\":" << sw << ",\"active_state\":" << opt.activeState << "}";
    PrintEvent(out.str());
  }
  if (!opt.holdSwitches.empty())
    SleepMs(opt.settleMs);
  SleepMs(opt.bootMs);
  for (int sw : opt.holdSwitches) {
    PinmameSetSwitch(sw, opt.activeState);
    std::ostringstream out;
    out << "{\"event\":\"hold_switch_reassert\",\"switch\":" << sw
        << ",\"active_state\":" << opt.activeState << "}";
    PrintEvent(out.str());
  }
  if (!opt.holdSwitches.empty())
    SleepMs(opt.settleMs);
  for (PINMAME_KEYCODE key : opt.holdKeys)
    g_pressedKeys.insert(key);
  if (!opt.holdKeys.empty()) {
    std::ostringstream out;
    out << "{\"event\":\"hold_keys\",\"count\":" << opt.holdKeys.size() << "}";
    PrintEvent(out.str());
    SleepMs(opt.settleMs);
  }
  std::vector<uint8_t> baseline = Snapshot(opt.outDir, "boot");

  auto keyName = [](PINMAME_KEYCODE code) -> std::string {
    if (code >= PINMAME_KEYCODE_NUMBER_0 && code <= PINMAME_KEYCODE_NUMBER_9) {
      char c = static_cast<char>('0' + (code - PINMAME_KEYCODE_NUMBER_0));
      return std::string(1, c);
    }
    return std::to_string(static_cast<int>(code));
  };

  auto pressAndSnapshot = [&](PINMAME_KEYCODE key, const std::string& prefix) {
    std::ostringstream beforeLabel;
    beforeLabel << prefix << "_key" << keyName(key) << "_before";
    std::vector<uint8_t> before = Snapshot(opt.outDir, beforeLabel.str(), &baseline);

    std::ostringstream event;
    event << "{\"event\":\"key\",\"key\":\"" << JsonEscape(keyName(key))
          << "\",\"pulse_ms\":" << opt.keyPulseMs
          << ",\"settle_ms\":" << opt.settleMs << "}";
    PrintEvent(event.str());

    PulseKey(key, opt.keyPulseMs, opt.settleMs);

    std::ostringstream afterLabel;
    afterLabel << prefix << "_key" << keyName(key) << "_after";
    std::vector<uint8_t> after = Snapshot(opt.outDir, afterLabel.str(), &before);
    baseline = after;
  };

  auto setSwitchAndSnapshot = [&](int sw, int state, const std::string& prefix) {
    std::ostringstream beforeLabel;
    beforeLabel << prefix << "_sw" << sw << "_set_before";
    std::vector<uint8_t> before = Snapshot(opt.outDir, beforeLabel.str(), &baseline);

    std::ostringstream event;
    event << "{\"event\":\"set_switch\",\"switch\":" << sw
          << ",\"state\":" << state
          << ",\"settle_ms\":" << opt.settleMs << "}";
    PrintEvent(event.str());

    PinmameSetSwitch(sw, state);
    SleepMs(opt.settleMs);

    std::ostringstream afterLabel;
    afterLabel << prefix << "_sw" << sw << "_set_after";
    std::vector<uint8_t> after = Snapshot(opt.outDir, afterLabel.str(), &before);
    baseline = after;
  };

  int setSeq = 0;
  bool postStartApplied = false;
  auto applyPostStartSets = [&]() {
    for (const auto& item : opt.postStartSwitchSets) {
      std::ostringstream prefix;
      prefix << "poststart" << (++setSeq);
      setSwitchAndSnapshot(item.first, item.second, prefix.str());
    }
    postStartApplied = true;
  };

  int keySeq = 0;
  for (int i = 0; i < opt.coins; ++i) {
    pressAndSnapshot(PINMAME_KEYCODE_NUMBER_5, "coin" + std::to_string(i + 1) + "_" + std::to_string(++keySeq));
  }
  for (int i = 0; i < opt.starts; ++i) {
    pressAndSnapshot(PINMAME_KEYCODE_NUMBER_1, "start" + std::to_string(i + 1) + "_" + std::to_string(++keySeq));
    if (i == 0 && opt.starts > 1)
      applyPostStartSets();
    if (i + 1 < opt.starts)
      SleepMs(opt.startGapMs);
  }
  for (PINMAME_KEYCODE key : opt.keyPulses) {
    pressAndSnapshot(key, "manualkey" + std::to_string(++keySeq));
  }

  if (!postStartApplied)
    applyPostStartSets();

  auto pulseAndSnapshot = [&](int sw, const std::string& prefix) {
    std::ostringstream beforeLabel;
    beforeLabel << prefix << "_sw" << sw << "_before";
    std::vector<uint8_t> before = Snapshot(opt.outDir, beforeLabel.str(), &baseline);

    std::ostringstream event;
    event << "{\"event\":\"pulse\",\"switch\":" << sw
          << ",\"active_state\":" << opt.activeState
          << ",\"pulse_ms\":" << opt.pulseMs
          << ",\"settle_ms\":" << opt.settleMs << "}";
    PrintEvent(event.str());

    PulseSwitch(sw, opt.activeState, opt.pulseMs, opt.settleMs);

    std::ostringstream afterLabel;
    afterLabel << prefix << "_sw" << sw << "_after";
    std::vector<uint8_t> after = Snapshot(opt.outDir, afterLabel.str(), &before);
    baseline = after;
  };

  int seq = 0;
  for (int sw : opt.pulses) {
    std::ostringstream prefix;
    prefix << "manual" << (++seq);
    pulseAndSnapshot(sw, prefix.str());
  }

  int actionSeq = 0;
  bool nvStopArmed = false;
  for (const Action& action : opt.actions) {
    if (action.type != ActionType::StopIfNv)
      continue;
    if (static_cast<size_t>(action.value1) >= baseline.size())
      continue;
    const bool current = (baseline[static_cast<size_t>(action.value1)] & action.value2) != 0;
    const bool expected = action.pulseMs != 0;
    if (current != expected)
      nvStopArmed = true;
  }
  bool stoppedOnNv = false;
  bool pendingNvMatch = false;
  for (const Action& action : opt.actions) {
    std::ostringstream prefix;
    prefix << "action" << (++actionSeq);
    if (action.type == ActionType::Wait) {
      std::ostringstream event;
      event << "{\"event\":\"wait\",\"ms\":" << action.value1 << "}";
      PrintEvent(event.str());
      SleepMs(action.value1);
      baseline = Snapshot(opt.outDir, prefix.str() + "_wait_after", &baseline);
    } else if (action.type == ActionType::Set) {
      const int waitMs = action.settleMs >= 0 ? action.settleMs : opt.settleMs;
      std::ostringstream beforeLabel;
      beforeLabel << prefix.str() << "_sw" << action.value1 << "_set_before";
      std::vector<uint8_t> before = Snapshot(opt.outDir, beforeLabel.str(), &baseline);
      std::ostringstream event;
      event << "{\"event\":\"action_set\",\"switch\":" << action.value1
            << ",\"state\":" << action.value2 << ",\"settle_ms\":" << waitMs << "}";
      PrintEvent(event.str());
      PinmameSetSwitch(action.value1, action.value2);
      SleepMs(waitMs);
      std::ostringstream afterLabel;
      afterLabel << prefix.str() << "_sw" << action.value1 << "_set_after";
      baseline = Snapshot(opt.outDir, afterLabel.str(), &before);
    } else if (action.type == ActionType::Pulse) {
      const int pulseMs = action.pulseMs >= 0 ? action.pulseMs : opt.pulseMs;
      const int settleMs = action.settleMs >= 0 ? action.settleMs : opt.settleMs;
      std::ostringstream beforeLabel;
      beforeLabel << prefix.str() << "_sw" << action.value1 << "_before";
      std::vector<uint8_t> before = Snapshot(opt.outDir, beforeLabel.str(), &baseline);
      std::ostringstream event;
      event << "{\"event\":\"action_pulse\",\"switch\":" << action.value1
            << ",\"active_state\":" << opt.activeState
            << ",\"pulse_ms\":" << pulseMs << ",\"settle_ms\":" << settleMs << "}";
      PrintEvent(event.str());
      PulseSwitch(action.value1, opt.activeState, pulseMs, settleMs);
      std::ostringstream afterLabel;
      afterLabel << prefix.str() << "_sw" << action.value1 << "_after";
      baseline = Snapshot(opt.outDir, afterLabel.str(), &before);
    } else if (action.type == ActionType::StopIfNv) {
      baseline = Snapshot(opt.outDir, prefix.str() + "_nv_check", &baseline);
      const bool inRange = static_cast<size_t>(action.value1) < baseline.size();
      const bool current = inRange &&
          ((baseline[static_cast<size_t>(action.value1)] & action.value2) != 0);
      const bool expected = action.pulseMs != 0;
      if (inRange && current != expected)
        nvStopArmed = true;
      const bool matched = inRange && current == expected;
      std::ostringstream event;
      event << "{\"event\":\"stop_if_nv\",\"offset\":" << action.value1
            << ",\"mask\":" << action.value2
            << ",\"expected\":" << (expected ? 1 : 0)
            << ",\"current\":" << (current ? 1 : 0)
            << ",\"armed\":" << (nvStopArmed ? "true" : "false")
            << ",\"matched\":" << (matched ? "true" : "false") << "}";
      PrintEvent(event.str());
      pendingNvMatch = nvStopArmed && matched;
    } else {
      if (!pendingNvMatch) {
        std::ostringstream event;
        event << "{\"event\":\"stop_if_score_stable\",\"offset\":" << action.value1
              << ",\"length\":" << action.value2
              << ",\"switch\":" << action.value3
              << ",\"candidate_matched\":false,\"score_changed\":null}";
        PrintEvent(event.str());
        continue;
      }
      const int pulseMs = action.pulseMs >= 0 ? action.pulseMs : opt.pulseMs;
      const int settleMs = action.settleMs >= 0 ? action.settleMs : opt.settleMs;
      baseline = Snapshot(opt.outDir, prefix.str() + "_score_stable_before", &baseline);
      const size_t offset = static_cast<size_t>(action.value1);
      const size_t length = static_cast<size_t>(action.value2);
      std::vector<uint8_t> scoreBefore;
      if (offset + length <= baseline.size())
        scoreBefore.assign(baseline.begin() + offset, baseline.begin() + offset + length);
      PulseSwitch(action.value3, opt.activeState, pulseMs, settleMs);
      baseline = Snapshot(opt.outDir, prefix.str() + "_score_stable_after", &baseline);
      bool scoreChanged = true;
      if (!scoreBefore.empty() && offset + length <= baseline.size()) {
        scoreChanged = !std::equal(
            scoreBefore.begin(), scoreBefore.end(), baseline.begin() + offset);
      }
      std::ostringstream event;
      event << "{\"event\":\"stop_if_score_stable\",\"offset\":" << action.value1
            << ",\"length\":" << action.value2
            << ",\"switch\":" << action.value3
            << ",\"candidate_matched\":" << (pendingNvMatch ? "true" : "false")
            << ",\"score_changed\":" << (scoreChanged ? "true" : "false") << "}";
      PrintEvent(event.str());
      if (pendingNvMatch && !scoreChanged) {
        stoppedOnNv = true;
        break;
      }
      pendingNvMatch = false;
    }
  }
  if (stoppedOnNv)
    PrintEvent("{\"event\":\"game_over_watch_matched\"}");

  if (opt.fuzz) {
    for (int sw = opt.switchMin; sw <= opt.switchMax; ++sw) {
      std::ostringstream prefix;
      prefix << "fuzz" << std::setw(3) << std::setfill('0') << sw;
      pulseAndSnapshot(sw, prefix.str());
    }
  }

  Snapshot(opt.outDir, "final", &baseline);
  for (PINMAME_KEYCODE key : opt.holdKeys)
    g_pressedKeys.erase(key);
  for (int sw : opt.holdSwitches)
    PinmameSetSwitch(sw, opt.activeState ? 0 : 1);
  PinmameStop();
  PrintEvent("{\"event\":\"stopped\"}");
  return 0;
}
