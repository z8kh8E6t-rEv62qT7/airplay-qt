#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace airplay {
// One producer and one consumer; neither side modifies the other's cursor.
template <class T, size_t Capacity> class RealtimeQueue {
  static_assert(Capacity > 0 && std::is_trivially_copyable_v<T>);

public:
  bool push(const T &value) noexcept {
    const auto w = write_.load(std::memory_order_relaxed);
    const auto used = w - read_.load(std::memory_order_acquire);
    if (used == Capacity)
      return false;
    if (used + 1 > highWater_)
      highWater_ = size_t(used + 1);
    values_[w % Capacity] = value;
    write_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(T &value) noexcept {
    const auto r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire))
      return false;
    value = values_[r % Capacity];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }
  // Producer-only observation, sampled before publishing each element.
  size_t producerHighWater() const noexcept { return highWater_; }

private:
  size_t highWater_ = 0;
  std::array<T, Capacity> values_{};
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
};
} // namespace airplay
