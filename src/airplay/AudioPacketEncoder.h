#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <openssl/evp.h>
#include <span>

namespace airplay {
// 352 stereo frames, ALAC framing, RTP header, AEAD tag and nonce suffix.
inline constexpr size_t audioPacketCapacity = 1452;
size_t encodeAlac(std::span<const int16_t>, std::span<unsigned char>) noexcept;
class AudioPacketEncoder {
public:
  AudioPacketEncoder();
  ~AudioPacketEncoder();
  AudioPacketEncoder(const AudioPacketEncoder &) = delete;
  AudioPacketEncoder &operator=(const AudioPacketEncoder &) = delete;
  bool prepare(std::span<const unsigned char> key) noexcept;
  size_t encode(std::span<const int16_t> samples, uint16_t sequence,
                uint32_t timestamp, uint64_t counter,
                std::span<unsigned char> output) noexcept;

private:
  EVP_CIPHER_CTX *context_ = nullptr;
  std::array<unsigned char, 1416> alac_{};
  bool ready_ = false;
};
} // namespace airplay
