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
int16_t sample(const std::byte *source, PcmFormat f) {
  uint64_t raw = 0;
  for (int i = 0; i < f.bytes; ++i)
    raw |= uint64_t(std::to_integer<unsigned char>(source[i]))
           << (8 * (f.bigEndian ? f.bytes - 1 - i : i));
  double value;
  if (f.floating) {
    value = f.bytes == 4 ? double(std::bit_cast<float>(uint32_t(raw)))
                         : std::bit_cast<double>(raw);
    if (!std::isfinite(value))
      throw i18n::MessageError(
          i18n::text(i18n::Id::AudioInputContainsNaNInfStreamingStopped));
    value = std::clamp(value, -1., 1.) * 32768.;
  } else {
    // ASIO Int32{MSB,LSB}{16,18,20,24} is right-aligned, signed PCM.
    const uint64_t mask = (uint64_t(1) << f.bits) - 1;
    const uint64_t sign = uint64_t(1) << (f.bits - 1);
    const int64_t signedValue = int64_t((raw & mask) ^ sign) - int64_t(sign);
    value = std::ldexp(double(signedValue), 16 - f.bits);
  }
  return static_cast<int16_t>(std::clamp(std::round(value), -32768., 32767.));
}
std::vector<int16_t> convert(std::span<const std::byte> left, PcmFormat lf,
                             std::span<const std::byte> right, PcmFormat rf) {
  if (left.size() % lf.bytes || right.size() % rf.bytes ||
      left.size() / lf.bytes != right.size() / rf.bytes)
    throw i18n::MessageError(
        i18n::text(i18n::Id::InputChannelDataLengthsDoNotMatch));
  std::vector<int16_t> pcm(left.size() / lf.bytes * 2);
  for (size_t i = 0; i < pcm.size() / 2; ++i) {
    pcm[2 * i] = sample(left.data() + i * lf.bytes, lf);
    pcm[2 * i + 1] = sample(right.data() + i * rf.bytes, rf);
  }
  return pcm;
}
} // namespace audio
