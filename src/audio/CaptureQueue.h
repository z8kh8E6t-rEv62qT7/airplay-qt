#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace audio {
// Diagnostic fingerprint of the exact planar bytes, without normalization.
inline uint64_t captureChecksum(std::span<const std::byte> left,
                                std::span<const std::byte> right) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  for (const auto channel : {left, right})
    for (const auto value : channel) {
      hash ^= std::to_integer<uint8_t>(value);
      hash *= 1099511628211ULL;
    }
  return hash;
}
// Single ASIO producer, single network consumer. Storage never changes while
// active.
class CaptureQueue {
public:
  CaptureQueue(size_t frames, size_t leftBytes, size_t rightBytes,
               size_t capacity)
      : frames_(frames), leftBytes_(leftBytes), rightBytes_(rightBytes),
        capacity_(capacity), storage_(capacity * (leftBytes + rightBytes)) {
    if (!frames || !leftBytes || !rightBytes || capacity < 2)
      throw std::invalid_argument("Invalid capture queue");
  }
  bool push(const void *left, const void *right,
            uint64_t *copiedChecksum = nullptr) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) >= capacity_)
      return false;
    auto *slot =
        storage_.data() + (write % capacity_) * (leftBytes_ + rightBytes_);
    std::memcpy(slot, left, leftBytes_);
    std::memcpy(slot + leftBytes_, right, rightBytes_);
    if (copiedChecksum)
      *copiedChecksum =
          captureChecksum({slot, leftBytes_}, {slot + leftBytes_, rightBytes_});
    write_.store(write + 1, std::memory_order_release);
    return true;
  }
  bool peek(std::span<const std::byte> &left,
            std::span<const std::byte> &right) const noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire))
      return false;
    const auto *slot =
        storage_.data() + (read % capacity_) * (leftBytes_ + rightBytes_);
    left = {slot, leftBytes_};
    right = {slot + leftBytes_, rightBytes_};
    return true;
  }
  void pop() noexcept { read_.fetch_add(1, std::memory_order_release); }
  uint64_t queuedFrames() const noexcept {
    return (write_.load(std::memory_order_acquire) -
            read_.load(std::memory_order_acquire)) *
           frames_;
  }
  uint64_t capturedFrames() const noexcept {
    return write_.load(std::memory_order_acquire) * frames_;
  }
  std::atomic<int> fault{0};

private:
  size_t frames_, leftBytes_, rightBytes_, capacity_;
  std::vector<std::byte> storage_;
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
} // namespace audio
