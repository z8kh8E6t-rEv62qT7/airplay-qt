#include "AudioPacketEncoder.h"
#include <algorithm>
#include <new>

namespace airplay {
size_t encodeAlac(std::span<const int16_t> samples,
                  std::span<unsigned char> out) noexcept {
  if (samples.empty() || samples.size() % 2 || samples.size() > 704 ||
      out.size() < samples.size() * 2 + 8)
    return 0;
  uint64_t bits = 0;
  int count = 0;
  size_t cursor = 0;
  const auto put = [&](uint64_t value, int width) {
    bits = (bits << width) | value;
    count += width;
    while (count >= 8) {
      count -= 8;
      out[cursor++] = static_cast<unsigned char>(bits >> count);
      bits &= (uint64_t(1) << count) - 1;
    }
  };
  put(1, 3);
  put(0, 4);
  put(0, 12);
  put(1, 1);
  put(0, 2);
  put(1, 1);
  put(samples.size() / 2, 32);
  for (auto sample : samples)
    put(uint16_t(sample), 16);
  put(7, 3);
  if (count)
    put(0, 8 - count);
  return cursor;
}
AudioPacketEncoder::AudioPacketEncoder() : context_(EVP_CIPHER_CTX_new()) {
  if (!context_)
    throw std::bad_alloc();
}
AudioPacketEncoder::~AudioPacketEncoder() { EVP_CIPHER_CTX_free(context_); }
bool AudioPacketEncoder::prepare(std::span<const unsigned char> key) noexcept {
  ready_ = false;
  if (key.size() != 32)
    return false;
  const std::array<unsigned char, 12> iv{};
  if (EVP_EncryptInit_ex(context_, EVP_chacha20_poly1305(), nullptr, key.data(),
                         iv.data()) != 1)
    return false;
  ready_ = true;
  // Initialize provider lazy state before entering the realtime loop. This
  // packet is discarded, never transmitted; the first real packet uses nonce 0.
  std::array<int16_t, 704> silence{};
  std::array<unsigned char, audioPacketCapacity> output{};
  return encode(silence, 0, 0, 0, output) != 0;
}
size_t AudioPacketEncoder::encode(std::span<const int16_t> samples,
                                  uint16_t sequence, uint32_t timestamp,
                                  uint64_t counter,
                                  std::span<unsigned char> out) noexcept {
  if (!ready_ || out.size() < audioPacketCapacity)
    return 0;
  const auto length = encodeAlac(samples, alac_);
  if (!length)
    return 0;
  std::fill_n(out.data(), 12, 0);
  out[0] = 0x80;
  out[1] = counter == 0 ? 0xe0 : 0x60;
  out[2] = sequence >> 8;
  out[3] = sequence;
  for (int i = 0; i < 4; ++i)
    out[4 + i] = timestamp >> (24 - 8 * i);
  std::array<unsigned char, 12> iv{};
  for (int i = 0; i < 8; ++i)
    iv[4 + i] = counter >> (8 * i);
  int size = 0, finalSize = 0;
  // A NULL cipher and key reuse the initialized provider and key; only the
  // per-packet nonce changes. No context reset/allocation on this path.
  if (EVP_EncryptInit_ex(context_, nullptr, nullptr, nullptr, iv.data()) != 1 ||
      EVP_EncryptUpdate(context_, nullptr, &size, out.data() + 4, 8) != 1 ||
      EVP_EncryptUpdate(context_, out.data() + 12, &size, alac_.data(),
                        int(length)) != 1 ||
      size != int(length) ||
      EVP_EncryptFinal_ex(context_, out.data() + 12 + size, &finalSize) != 1 ||
      finalSize != 0 ||
      EVP_CIPHER_CTX_ctrl(context_, EVP_CTRL_AEAD_GET_TAG, 16,
                          out.data() + 12 + size) != 1)
    return 0;
  std::copy(iv.begin() + 4, iv.end(), out.begin() + 28 + size);
  return 36 + length;
}
} // namespace airplay
