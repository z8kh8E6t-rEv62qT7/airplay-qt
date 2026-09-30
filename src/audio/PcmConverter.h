#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace audio {
struct PcmFormat {
  int bytes;
  int bits;
  bool bigEndian;
  bool floating;
};
PcmFormat format(long asioType);
int16_t sample(const std::byte *source, PcmFormat format);
std::vector<int16_t> convert(std::span<const std::byte> left,
                             PcmFormat leftFormat,
                             std::span<const std::byte> right,
                             PcmFormat rightFormat);
} // namespace audio
