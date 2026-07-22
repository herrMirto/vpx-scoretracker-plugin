// license:GPLv3+

#include "ScoreSignature.h"

#include <array>
#include <cctype>
#include <sstream>

#include <ScoreSigningConfig.h>
#if SCORETRACKER_SIGNING_ENABLED
#include <monocypher-ed25519.h>
#include <monocypher.h>
#endif

namespace ScoreTracker
{
namespace
{

#if SCORETRACKER_SIGNING_ENABLED
template <size_t Size>
bool DecodeHex(const std::string& value, std::array<uint8_t, Size>& output)
{
   if (value.size() != Size * 2)
      return false;

   const auto digit = [](char ch) -> int {
      const unsigned char value = static_cast<unsigned char>(ch);
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      if (value >= 'A' && value <= 'F') return value - 'A' + 10;
      return -1;
   };

   for (size_t i = 0; i < Size; ++i)
   {
      const int high = digit(value[i * 2]);
      const int low = digit(value[i * 2 + 1]);
      if (high < 0 || low < 0)
         return false;
      output[i] = static_cast<uint8_t>((high << 4) | low);
   }
   return true;
}

std::string EncodeHex(const uint8_t* bytes, size_t size)
{
   static constexpr char kDigits[] = "0123456789abcdef";
   std::string output(size * 2, '0');
   for (size_t i = 0; i < size; ++i)
   {
      output[i * 2] = kDigits[bytes[i] >> 4];
      output[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
   }
   return output;
}
#endif

}

std::string BuildScoreSignaturePayload(const ScoreSignatureFields& fields)
{
   std::ostringstream payload;
   payload << "scoretracker.game.v1\n";
   payload << "date " << fields.date.size() << '\n' << fields.date << '\n';
   payload << "rom " << fields.rom.size() << '\n' << fields.rom << '\n';
   payload << "duration " << fields.gameDuration << '\n';
   payload << "scores " << fields.scores.size() << '\n';
   for (const int64_t score : fields.scores)
      payload << "score " << score << '\n';
   return payload.str();
}

bool SignScore(const ScoreSignatureFields& fields, std::string& signatureHex)
{
#if SCORETRACKER_SIGNING_ENABLED
   std::array<uint8_t, 32> seed {};
   std::array<uint8_t, 32> expectedPublicKey {};
   if (!DecodeHex(SCORETRACKER_SIGNING_SEED_HEX, seed)
      || !DecodeHex(kScoreSignaturePublicKeyHex, expectedPublicKey))
      return false;

   std::array<uint8_t, 64> secretKey {};
   std::array<uint8_t, 32> publicKey {};
   crypto_ed25519_key_pair(secretKey.data(), publicKey.data(), seed.data());
   if (publicKey != expectedPublicKey)
   {
      crypto_wipe(secretKey.data(), secretKey.size());
      return false;
   }

   const std::string payload = BuildScoreSignaturePayload(fields);
   std::array<uint8_t, 64> signature {};
   crypto_ed25519_sign(signature.data(), secretKey.data(),
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
   crypto_wipe(secretKey.data(), secretKey.size());
   signatureHex = EncodeHex(signature.data(), signature.size());
   return true;
#else
   (void)fields;
   (void)signatureHex;
   return false;
#endif
}

}
