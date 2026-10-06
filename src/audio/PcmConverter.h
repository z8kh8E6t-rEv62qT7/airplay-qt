#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace audio {
struct PcmFormat {
  int bytes;
  int bits;
  bool bigEndian;
  bool floating;
};
PcmFormat format(long asioType);
int16_t sample(const std::byte *source, PcmFormat format);
// Writes exactly two samples per input frame. The destination may be larger.
// Returns false for invalid formats, lengths, capacity or non-finite samples.
bool convert(std::span<const std::byte> left, PcmFormat leftFormat,
             std::span<const std::byte> right, PcmFormat rightFormat,
             std::span<int16_t> destination) noexcept;
} // namespace audio
