#pragma once
#include "AudioPacketEncoder.h"
#include "RealtimeQueue.h"
#include "app/Settings.h"
#include "audio/CaptureStream.h"
#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace airplay {
enum class SendFailure {
  None,
  InputFault,
  Interrupted,
  InputTimeout,
  Backlog,
  InvalidPcm,
  ClockJump,
  Late,
  NonceExhausted,
  Encode,
  Send,
  Retransmit,
  QueueFull,
  Wait
};
struct SendError {
  SendFailure code = SendFailure::None;
  int detail = 0;
};
struct RetransmitRequest {
  uint16_t peer = 0, port = 0, request = 0, first = 0, count = 0;
};
enum class InputState { Waiting, Buffering, Playing, Reset };
struct SendReport {
  bool streaming = false;
  InputState input = InputState::Waiting;
  uint64_t inputRevision = 0, telemetryRevision = 0;
  bool telemetryEnabled = true;
  int64_t audible = 0;
  double leftPeak = 0, rightPeak = 0;
  uint64_t buffered = 0, packets = 0, retransmitted = 0, expired = 0;
  uint64_t maxGapNs = 0, maxWorkNs = 0, maxBuffered = 0;
  uint64_t sentFrames = 0, pcmFrames = 0;
  // RTP frame deadlines (one sample per packet time, independent of peer count).
  // Buckets: on time, <=100us, <=500us, <=1ms, <=5ms, >5ms.
  std::array<uint64_t, 6> deadlineHistogram{};
  uint64_t maxLatenessNs = 0, requestHighWater = 0;
};
// Invoked only by the owning thread. No Qt objects, exceptions, allocation,
// mutexes or logging in step()/retransmit(). Preparation may allocate.
class AudioSendCore {
public:
  using Send = bool (*)(void *, size_t peer, bool control, uint16_t port,
                        std::span<const unsigned char>) noexcept;
  AudioSendCore(const app::Timing &, audio::CaptureStream,
                uint16_t firstSequence, uint32_t firstRtp, size_t peers);
  bool preparePeer(size_t, std::span<const unsigned char> key);
  bool warmup() noexcept;
  void begin(int64_t monotonic) noexcept;
  SendError step(int64_t now, int64_t wall, Send, void *) noexcept;
  SendError retransmit(RetransmitRequest &, int &budget, Send, void *) noexcept;
  int64_t deadline() const noexcept { return deadline_; }
  void telemetry(bool enabled, uint64_t revision) noexcept;
  SendReport report(bool resetPeaks = false) noexcept;
  void workTime(uint64_t ns) noexcept { maxWork_ = std::max(maxWork_, ns); }

private:
  struct Packet {
    std::array<unsigned char, audioPacketCapacity> bytes{};
    uint16_t sequence = 0, size = 0;
  };
  struct Peer {
    AudioPacketEncoder encoder;
    std::array<Packet, 1024> history;
  };
  void resetInput(bool invalidate) noexcept;
  void inputState(InputState) noexcept;
  app::Timing timing_;
  audio::CaptureStream stream_;
  std::vector<int16_t> pcm_, converted_;
  size_t read_ = 0, size_ = 0;
  std::vector<std::unique_ptr<Peer>> peers_;
  uint16_t firstSequence_;
  uint32_t firstRtp_;
  uint64_t counter_ = 0, sent_ = 0, seen_ = 0, generation_ = 0;
  int64_t started_ = 0, anchor_ = 0, audible_ = 0;
  int64_t lastInput_ = 0, lastPoll_ = 0, deadline_ = 0;
  bool streaming_ = false, ready_ = false, telemetryEnabled_ = true;
  uint64_t telemetryRevision_ = 0, packets_ = 0, retransmitted_ = 0,
           expired_ = 0;
  double leftPeak_ = 0, rightPeak_ = 0;
  InputState input_ = InputState::Waiting;
  uint64_t inputRevision_ = 0, maxGap_ = 0, maxWork_ = 0, maxBuffered_ = 0;
  std::array<uint64_t, 6> deadlineHistogram_{};
  uint64_t maxLateness_ = 0;
};
} // namespace airplay
