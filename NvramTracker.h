// license:GPLv3+

#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "common.h"
#include "libpinmame.h"

namespace ScoreTracker
{

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
   string nibble;
};

struct NvramSegment
{
   int address;
   int size;
   int fileBase;
   string nibble;
};

struct MemoryLayoutEntry
{
   int address = 0;
   int size = 0;
   string type;
   string nibble;
};

struct Descriptor
{
   string label;
   EncodingType encoding = EncodingType::Int;
   string nibble;
   unsigned int mask = 0;
   bool hasMask = false;
   bool isLittleEndian = false;
   bool invert = false;
   double scale = 1.0;
   double offset = 0.0;
   vector<int> addresses;

   vector<ResolvedAddress> resolvedAddresses;
   string resolvedNibbleMode;
};

// Tracks the live game state of the running PinMAME machine by decoding its NVRAM (and, for some
// platforms, main CPU RAM) using a PinMAME NVRAM map (see https://github.com/tomlogic/pinmame-nvram-maps),
// then appends each completed game to a scores.json file.
//
// All methods are expected to be called from the main thread: polling is driven by a scheduled
// MsgPluginAPI::RunOnMainThread callback, machine memory is read through the libpinmame API.
class NvramTracker final
{
public:
   enum class MapStatus
   {
      Available,
      NotFound,
      Error,
   };

   NvramTracker() = default;
   ~NvramTracker();

   // Lightweight capability check used before creating a tracker.
   static MapStatus ProbeMap(const string& gameId, const string& mapsPath, string& detail);

   // Returns true when monitoring started (a valid JSON map exists for the gameId).
   bool Start(const string& gameId, const string& mapsPath, const string& tablePath, const string& outputPath);

   // Finalizes the session: if a played game was not yet persisted (VPX exited before the
   // game-over was confirmed), it is written as an exit fallback.
   void Stop();

   // Periodic inspection of the machine state; does nothing when no machine is running.
   void Poll();

private:
   // Mapping and parsing helpers
   bool LoadMap(const string& gameId);
   void ResolveDescriptor(Descriptor& d);
   vector<int> ResolveAddresses(const nlohmann::json& desc);
   Descriptor ParseDescriptor(const nlohmann::json& desc);
   static int ParseIntVal(const nlohmann::json& val, int defaultVal = 0);

   // Decoding helpers
   bool ReadUnits(const Descriptor& desc, vector<uint8_t>& units);
   static int64_t DecodeInt(const vector<uint8_t>& units, bool isLittleEndian);
   static int64_t DecodeBCD(const vector<uint8_t>& units, const string& nibbleMode, bool isLittleEndian);
   int64_t Decode(const Descriptor& desc);

   // Platform memory region helpers
   string GetAddrRegionType(int addr) const;
   const NvramSegment* GetSegmentForAddr(int addr) const;

   // Persist the completed game to scores.json (when writeScoresFile is set and the session
   // qualifies). authoritativeCount is the number of leading entries in finalScores that came
   // from game_state.final_scores (a ROM-frozen value) rather than the live "scores"
   // descriptors, so they must not be overwritten by the peak-score merge.
   void FinalizeSession(const vector<int64_t>& finalScores, bool writeScoresFile, size_t authoritativeCount);

   // Overlays game_state.final_scores onto the live "scores" snapshot (see FinalizeSession)
   // and zeroes out any player slot beyond what final_scores covers -- those live "scores"
   // readings have no authoritative source and don't track real play, so reporting 0 (no data)
   // beats reporting a different wrong number. Sets authoritativeCount to the number of leading
   // entries that came from final_scores. gameOverObserved must say whether the machine actually
   // reached game-over this session: a most-recent-first buffer still holds the previous games
   // until then and would otherwise be reported for an unfinished session.
   vector<int64_t> BuildFinalScoresSnapshot(const vector<int64_t>& playerScores, bool gameOverObserved, size_t& authoritativeCount);
   nlohmann::json BuildCompletedGameState() const;

   string m_gameId;
   string m_mapsPath;
   string m_tablePath;
   string m_outputPath;

   // Loaded map data
   nlohmann::json m_mapData;
   nlohmann::json m_platformData;
   vector<NvramSegment> m_segments;
   vector<MemoryLayoutEntry> m_layout;
   int m_lowRamMirrorLimit = 0;

   // Descriptors
   vector<Descriptor> m_scoresDesc;
   // Per-player score snapshot captured by the ROM at game-over (game_state.final_scores).
   // Several platforms clear/reuse the live "scores" location as soon as the game ends,
   // so this frozen copy is the only reliable read once game-over is confirmed.
   vector<Descriptor> m_finalScoresDesc;
   // game_state.final_scores_order == "most_recent_first": final_scores is the machine's
   // recent-games buffer (Capcom), newest completed entry first, one push per player at
   // game-over -- not a per-player array. Player i of an N-player game sits at entry N-1-i.
   bool m_finalScoresMostRecentFirst = false;
   // Decoded final_scores entries captured when the session enters play. For a
   // most-recent-first buffer the number of entries pushed on top of this baseline at
   // game-over IS the player count -- available even on titles with no usable
   // player_count byte (Breakshot), and immune to count-byte read glitches.
   vector<int64_t> m_finalScoresBaseline;
   bool m_hasFinalScoresBaseline = false;
   vector<std::pair<string, Descriptor>> m_gameStateDescriptors;
   bool m_hasRamFields = false;

   // Reusable buffers to avoid per-poll allocations
   vector<uint8_t> m_nvram;
   vector<PinmameNVRAMState> m_nvramBuffer;
   vector<uint8_t> m_decodeUnits;
   vector<int64_t> m_playerScores;
   std::unordered_map<string, int64_t> m_decodedValues;

   // Session tracking
   std::chrono::steady_clock::time_point m_sessionStartRealTime;
   vector<int64_t> m_highestScores;
   std::unordered_map<string, int64_t> m_maxGameStateValues;
   vector<int64_t> m_prevPlayerScores;
   // Last raw player_count reading; a value only enters m_maxGameStateValues after two
   // consecutive identical reads (see Poll) so a single glitched byte can't stick.
   int64_t m_lastPlayerCountRead = -1;
   int m_gameOverAnomalies = 0;
   bool m_ignoreGameOver = false;
   bool m_hasLastState = false;
   bool m_gameOverLast = false;
   bool m_gameOverPending = false;
   std::chrono::steady_clock::time_point m_gameOverSince;
   bool m_summarySent = false;
   bool m_hasBeenInPlay = false;
   bool m_monitoringStarted = false;
};

}
