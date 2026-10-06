#include "PcmConverter.h"
#include "app/Message.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace audio {
PcmFormat format(long type) {
  const bool big = type < 16;
  const long base = big ? type : type - 16;
  switch (base) {
  case 0:
    return {2, 16, big, false};
  case 1:
    return {3, 24, big, false};
  case 2:
    return {4, 32, big, false};
  case 3:
    return {4, 32, big, true};
  case 4:
    return {8, 64, big, true};
  case 8:
    return {4, 16, big, false};
  case 9:
    return {4, 18, big, false};
  case 10:
    return {4, 20, big, false};
  case 11:
    return {4, 24, big, false};
  default:
    throw i18n::MessageError(
        i18n::text(i18n::Id::UnsupportedASIOFormatDSDIsNotAccepted));
  }
}
namespace {
bool validFormat(PcmFormat f) noexcept {
  return f.floating ? (f.bytes == 4 || f.bytes == 8)
                    : (f.bytes >= 1 && f.bytes <= 4 && f.bits > 0 &&
                       f.bits <= f.bytes * 8);
}
bool readSample(const std::byte *source, PcmFormat f, int16_t &result) noexcept {
  uint64_t raw = 0;
  for (int i = 0; i < f.bytes; ++i)
    raw |= uint64_t(std::to_integer<unsigned char>(source[i]))
           << (8 * (f.bigEndian ? f.bytes - 1 - i : i));
  double value;
  if (f.floating) {
    value = f.bytes == 4 ? double(std::bit_cast<float>(uint32_t(raw)))
                         : std::bit_cast<double>(raw);
    if (!std::isfinite(value)) return false;
    value = std::clamp(value, -1., 1.) * 32768.;
  } else {
    // ASIO Int32{MSB,LSB}{16,18,20,24} is right-aligned, signed PCM.
    const uint64_t mask = (uint64_t(1) << f.bits) - 1;
    const uint64_t sign = uint64_t(1) << (f.bits - 1);
    const int64_t signedValue = int64_t((raw & mask) ^ sign) - int64_t(sign);
    value = std::ldexp(double(signedValue), 16 - f.bits);
  }
  result = static_cast<int16_t>(std::clamp(std::round(value), -32768., 32767.));
  return true;
}
} // namespace
int16_t sample(const std::byte *source, PcmFormat f) {
  int16_t result = 0;
  if (!validFormat(f) || !readSample(source, f, result))
    throw i18n::MessageError(
        i18n::text(i18n::Id::AudioInputContainsNaNInfStreamingStopped));
  return result;
}
bool convert(std::span<const std::byte> left, PcmFormat lf,
             std::span<const std::byte> right, PcmFormat rf,
             std::span<int16_t> pcm) noexcept {
  if (!validFormat(lf) || !validFormat(rf) || left.size() % lf.bytes ||
      right.size() % rf.bytes || left.size() / lf.bytes != right.size() / rf.bytes ||
      left.size() / lf.bytes > pcm.size() / 2)
    return false;
  for (size_t i = 0; i < left.size() / lf.bytes; ++i) {
    const auto *l = left.data() + i * lf.bytes;
    const auto *r = right.data() + i * rf.bytes;
    if (!readSample(l, lf, pcm[2 * i]) || !readSample(r, rf, pcm[2 * i + 1]))
      return false;
  }
  return true;
}
} // namespace audio
