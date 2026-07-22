// license:GPLv3+

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ScoreTracker
{

inline constexpr const char* kScoreSignatureAlgorithm = "ed25519";
inline constexpr const char* kScoreSignatureKeyId = "scoretracker-release-v1";
inline constexpr const char* kScoreSignaturePublicKeyHex = "73a0a766bcaaeccbbd1692b43d8920ba2b372e29d49d99214118a40fedab799b";

struct ScoreSignatureFields
{
   std::string date;
   std::string rom;
   std::vector<int64_t> scores;
   int64_t gameDuration = 0;
};

std::string BuildScoreSignaturePayload(const ScoreSignatureFields& fields);
bool SignScore(const ScoreSignatureFields& fields, std::string& signatureHex);

}
