#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
// Single audio producer, single network consumer. Storage never changes while
// active.
class CaptureQueue {
public:
  static size_t capacityFor(int packetSamples, int backlogSamples) {
    if (packetSamples < 1 || packetSamples > 352 || backlogSamples < 1 ||
        backlogSamples > (1 << 30))
      throw std::invalid_argument("Invalid capture samples");
    return (size_t(backlogSamples) + size_t(packetSamples) - 1) /
               size_t(packetSamples) +
           2;
  }
  CaptureQueue(size_t frames, size_t leftBytes, size_t rightBytes,
               size_t capacity)
      : frames_(frames), leftBytes_(leftBytes), rightBytes_(rightBytes),
        capacity_(capacity),
        storage_(storageSize(frames, leftBytes, rightBytes, capacity)),
        generations_(capacity) {}
  // Reblock a planar hardware callback without allocating or publishing a
  // partially written block. The optional fingerprint covers this callback's
  // copied bytes, including the tail retained for the next callback. A producer
  // uses either append or push; it must not mix them while a partial block exists.
  bool append(const void *left, const void *right, size_t count,
              uint64_t *copiedChecksum = nullptr) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto read = read_.load(std::memory_order_acquire);
    const size_t freeFrames = (capacity_ - size_t(write - read)) * frames_;
    if (freeFrames < partial_ || count > freeFrames - partial_)
      return false;
    const size_t start = partial_;
    const size_t leftStride = leftBytes_ / frames_,
                 rightStride = rightBytes_ / frames_;
    size_t offset = 0;
    while (offset < count) {
      const auto position = start + offset;
      const auto within = position % frames_;
      const auto chunk = std::min(count - offset, frames_ - within);
      auto *slot =
          storage_.data() + ((write + position / frames_) % capacity_) *
                                (leftBytes_ + rightBytes_);
      std::memcpy(slot + within * leftStride,
                  static_cast<const std::byte *>(left) + offset * leftStride,
                  chunk * leftStride);
      std::memcpy(slot + leftBytes_ + within * rightStride,
                  static_cast<const std::byte *>(right) + offset * rightStride,
                  chunk * rightStride);
      generations_[(write + position / frames_) % capacity_] = 0;
      offset += chunk;
      if ((start + offset) % frames_ == 0)
        write_.store(write + (start + offset) / frames_,
                     std::memory_order_release);
    }
    if (copiedChecksum) {
      uint64_t hash = 14695981039346656037ULL;
      for (int channel = 0; channel < 2; ++channel) {
        const auto stride = channel ? rightStride : leftStride;
        for (size_t i = 0; i < count; ++i) {
          const auto position = start + i;
          const auto *sample = storage_.data() +
                               ((write + position / frames_) % capacity_) *
                                   (leftBytes_ + rightBytes_) +
                               (channel ? leftBytes_ : 0) +
                               (position % frames_) * stride;
          for (size_t byte = 0; byte < stride; ++byte) {
            hash ^= std::to_integer<uint8_t>(sample[byte]);
            hash *= 1099511628211ULL;
          }
        }
      }
      *copiedChecksum = hash;
    }
    partial_ = (start + count) % frames_;
    write_.store(write + (start + count) / frames_, std::memory_order_release);
    return true;
  }
  size_t blockSamples() const noexcept { return frames_; }
  bool push(const void *left, const void *right,
            uint64_t *copiedChecksum = nullptr, uint64_t generation = 0) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) >= capacity_)
      return false;
    auto *slot =
        storage_.data() + (write % capacity_) * (leftBytes_ + rightBytes_);
    std::memcpy(slot, left, leftBytes_);
    std::memcpy(slot + leftBytes_, right, rightBytes_);
    generations_[write % capacity_] = generation;
    if (copiedChecksum)
      *copiedChecksum =
          captureChecksum({slot, leftBytes_}, {slot + leftBytes_, rightBytes_});
    write_.store(write + 1, std::memory_order_release);
    return true;
  }
  bool peek(std::span<const std::byte> &left,
            std::span<const std::byte> &right,
            uint64_t *generation = nullptr) const noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire))
      return false;
    const auto *slot =
        storage_.data() + (read % capacity_) * (leftBytes_ + rightBytes_);
    left = {slot, leftBytes_};
    right = {slot + leftBytes_, rightBytes_};
    if (generation) *generation = generations_[read % capacity_];
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
  // VST host suspension ends this stream without being an input failure.
  // Sticky for the queue's lifetime; resumption always gets a new queue.
  std::atomic<bool> interrupted{false};
  // Continuous inputs invalidate old blocks without either thread resetting
  // the other thread's SPSC cursor. The consumer publishes clock feedback.
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> bufferedFrames{0};
  std::atomic<uint64_t> targetFrames{2048};
  std::atomic<bool> playbackActive{false};

private:
  static size_t storageSize(size_t frames, size_t left, size_t right,
                            size_t capacity) {
    const auto maximum = std::numeric_limits<size_t>::max();
    if (!frames || !left || !right || capacity < 2 || left % frames ||
        right % frames || left > maximum - right ||
        capacity > maximum / (left + right) || capacity > maximum / frames)
      throw std::invalid_argument("Invalid capture queue capacity");
    return capacity * (left + right);
  }
  size_t frames_, leftBytes_, rightBytes_, capacity_;
  size_t partial_ = 0;
  std::vector<std::byte> storage_;
  std::vector<uint64_t> generations_;
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
} // namespace audio
