#include "AudioSendCore.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace airplay {
AudioSendCore::AudioSendCore(const app::Timing &timing,
                             audio::CaptureStream stream, uint16_t sequence,
                             uint32_t rtp, size_t peers)
    : timing_(timing), stream_(std::move(stream)), firstSequence_(sequence),
      firstRtp_(rtp) {
  if (!stream_.queue || stream_.blockFrames < 1 || stream_.blockFrames > 352 ||
      stream_.queue->blockSamples() != size_t(stream_.blockFrames) ||
      peers < 1 || peers > 2 || !timing.validate().isEmpty())
    throw std::invalid_argument("Invalid audio sender configuration");
  pcm_.resize(size_t(timing.backlogSamples) * 2);
  converted_.resize(size_t(stream_.blockFrames) * 2);
  for (size_t i = 0; i < peers; ++i)
    peers_.push_back(std::make_unique<Peer>());
}
bool AudioSendCore::preparePeer(size_t index,
                                std::span<const unsigned char> key) {
  return index < peers_.size() && peers_[index]->encoder.prepare(key);
}
bool AudioSendCore::warmup() noexcept {
  // Providers can initialize thread-local state. Warm on the actual worker,
  // after publication of all peer keys and before starting its timeline.
  const std::array<int16_t, 704> silence{};
  std::array<unsigned char, audioPacketCapacity> output{};
  for (auto &peer : peers_)
    if (!peer->encoder.encode(silence, 0, 0, 0, output))
      return false;
  return true;
}
void AudioSendCore::begin(int64_t now) noexcept {
  lastInput_ = lastPoll_ = deadline_ = now;
  generation_ = stream_.queue->generation.load(std::memory_order_acquire);
  stream_.queue->targetFrames.store(
      uint64_t(std::max(timing_.packetSamples, timing_.prebufferSamples)),
      std::memory_order_release);
}
void AudioSendCore::inputState(InputState state) noexcept {
  if (input_ != state) {
    input_ = state;
    ++inputRevision_;
  }
}
void AudioSendCore::resetInput(bool invalidate) noexcept {
  if (invalidate)
    stream_.queue->generation.fetch_add(1, std::memory_order_acq_rel);
  generation_ = stream_.queue->generation.load(std::memory_order_acquire);
  read_ = size_ = 0;
  ready_ = false;
  stream_.queue->bufferedFrames.store(0, std::memory_order_release);
  stream_.queue->playbackActive.store(false, std::memory_order_release);
  inputState(InputState::Reset);
}
SendError AudioSendCore::step(int64_t now, int64_t wall, Send send,
                              void *context) noexcept {
  maxGap_ = std::max(maxGap_, uint64_t(std::max(int64_t(0), now - lastPoll_)));
  lastPoll_ = now;
  // Waiting for input must block as well; never spin on an overdue packet.
  deadline_ = now + 1000000;
  const bool continuous = stream_.gapPolicy == audio::GapPolicy::Silence;
  auto &queue = *stream_.queue;
  if (continuous &&
      generation_ != queue.generation.load(std::memory_order_acquire))
    resetInput(false);
  if (const auto fault = queue.fault.load())
    return {SendFailure::InputFault, fault};
  if (queue.interrupted.load())
    return {SendFailure::Interrupted};
  if (const auto captured = queue.capturedFrames(); captured != seen_) {
    seen_ = captured;
    lastInput_ = now;
  }
  if (!continuous && now - lastInput_ > timing_.inputTimeoutMs * 1e6)
    return {SendFailure::InputTimeout};
  if (!continuous &&
      queue.queuedFrames() + size_ / 2 > uint64_t(timing_.backlogSamples))
    return {SendFailure::Backlog};
  auto remaining = queue.queuedFrames();
  maxBuffered_ = std::max(maxBuffered_, remaining + size_ / 2);
  std::span<const std::byte> left, right;
  uint64_t generation = 0;
  // A bounded snapshot prevents the producer or a large configured backlog
  // from monopolizing an RT CPU. Further input is picked up on the next wake.
  for (int blocks = 0;
       blocks < 64 && remaining >= uint64_t(stream_.blockFrames) &&
       queue.peek(left, right, &generation);
       ++blocks) {
    remaining -= uint64_t(stream_.blockFrames);
    if (continuous && generation != generation_) {
      queue.pop();
      continue;
    }
    if (!audio::convert(left, stream_.left, right, stream_.right, converted_))
      return {SendFailure::InvalidPcm};
    queue.pop();
    if (continuous && !ready_ && !size_)
      inputState(InputState::Buffering);
    if (size_ + converted_.size() > pcm_.size()) {
      if (continuous) {
        resetInput(true);
        continue;
      }
      return {SendFailure::Backlog};
    }
    for (size_t i = 0; i < converted_.size(); ++i) {
      const auto value = converted_[i];
      pcm_[(read_ + size_++) % pcm_.size()] = value;
      if (telemetryEnabled_) {
        auto &peak = i % 2 ? rightPeak_ : leftPeak_;
        peak = std::max(peak, std::abs(double(value)) / 32768);
      }
    }
  }
  if (continuous &&
      generation_ != queue.generation.load(std::memory_order_acquire))
    resetInput(false);
  const auto target =
      size_t(std::max(timing_.packetSamples, timing_.prebufferSamples));
  if (continuous && !ready_ && size_ / 2 >= target) {
    ready_ = true;
    inputState(InputState::Playing);
  }
  if (!streaming_ && (continuous || size_ / 2 >= target)) {
    streaming_ = true;
    started_ = now;
    anchor_ = wall;
    audible_ = wall + int64_t(std::llround(timing_.leadMs * 1e6));
  }
  if (streaming_) {
    if (std::abs(double(wall - anchor_ - (now - started_))) >
        timing_.lateMs * 1e6)
      return {SendFailure::ClockJump};
    for (int batch = 0; batch < 128; ++batch) {
      const auto due = started_ + int64_t(sent_ / 44100 * 1000000000 +
                                          sent_ % 44100 * 1000000000 / 44100);
      if (now < due) {
        deadline_ = std::min(deadline_, due);
        break;
      }
      if (now - due > timing_.lateMs * 1e6)
        return {SendFailure::Late};
      if (continuous &&
          generation_ != queue.generation.load(std::memory_order_acquire))
        resetInput(false);
      const size_t count = size_t(timing_.packetSamples) * 2;
      if (continuous && ready_ && size_ < count)
        resetInput(true);
      if (!continuous && size_ < count)
        break;
      if (telemetryEnabled_) {
        const auto late = uint64_t(now - due);
        maxLateness_ = std::max(maxLateness_, late);
        const size_t bucket = late == 0 ? 0 : late <= 100000 ? 1
            : late <= 500000 ? 2 : late <= 1000000 ? 3
            : late <= 5000000 ? 4 : 5;
        ++deadlineHistogram_[bucket];
      }
      if (counter_ == UINT64_MAX)
        return {SendFailure::NonceExhausted};
      std::array<int16_t, 704> storage{};
      auto frame = std::span(storage).first(count);
      if (!continuous || ready_) {
        for (auto &sample : frame) {
          sample = pcm_[read_];
          read_ = (read_ + 1) % pcm_.size();
          --size_;
        }
      }
      const auto sequence = uint16_t(firstSequence_ + counter_);
      for (size_t i = 0; i < peers_.size(); ++i) {
        auto &peer = *peers_[i];
        auto &packet = peer.history[sequence % 1024];
        packet.size = uint16_t(peer.encoder.encode(frame, sequence,
                                                   firstRtp_ + uint32_t(sent_),
                                                   counter_, packet.bytes));
        packet.sequence = sequence;
        if (!packet.size)
          return {SendFailure::Encode};
        if (!send(context, i, false, 0,
                  std::span(packet.bytes).first(packet.size)))
          return {SendFailure::Send};
      }
      ++counter_;
      if (telemetryEnabled_)
        ++packets_;
      sent_ += count / 2;
    }
  }
  const auto buffered = queue.queuedFrames() + size_ / 2;
  maxBuffered_ = std::max(maxBuffered_, buffered);
  if (continuous) {
    queue.bufferedFrames.store(buffered, std::memory_order_release);
    queue.playbackActive.store(ready_, std::memory_order_release);
  }
  return {};
}
SendError AudioSendCore::retransmit(RetransmitRequest &request, int &budget,
                                    Send send, void *context) noexcept {
  if (request.peer >= peers_.size())
    return {SendFailure::Retransmit};
  auto &history = peers_[request.peer]->history;
  while (request.count && budget > 0) {
    --budget;
    --request.count;
    const auto seq = request.first++;
    const auto &packet = history[seq % history.size()];
    if (!packet.size || packet.sequence != seq) {
      if (telemetryEnabled_)
        ++expired_;
      continue;
    }
    std::array<unsigned char, audioPacketCapacity + 4> response{};
    response[0] = 0x80;
    response[1] = 0xd6;
    response[2] = request.request >> 8;
    response[3] = request.request;
    std::copy_n(packet.bytes.begin(), packet.size, response.begin() + 4);
    if (!send(context, request.peer, true, request.port,
              std::span(response).first(packet.size + 4)))
      return {SendFailure::Retransmit};
    if (telemetryEnabled_)
      ++retransmitted_;
  }
  return {};
}
void AudioSendCore::telemetry(bool enabled, uint64_t revision) noexcept {
  if (telemetryRevision_ == revision && telemetryEnabled_ == enabled)
    return;
  telemetryEnabled_ = enabled;
  telemetryRevision_ = revision;
  leftPeak_ = rightPeak_ = 0;
}
SendReport AudioSendCore::report(bool resetPeaks) noexcept {
  SendReport result{streaming_,
                    input_,
                    inputRevision_,
                    telemetryRevision_,
                    telemetryEnabled_,
                    audible_,
                    leftPeak_,
                    rightPeak_,
                    stream_.queue->queuedFrames() + size_ / 2,
                    packets_,
                    retransmitted_,
                    expired_,
                    maxGap_,
                    maxWork_,
                    maxBuffered_,
                    sent_,
                    size_ / 2};
  result.deadlineHistogram = deadlineHistogram_;
  result.maxLatenessNs = maxLateness_;
  if (resetPeaks)
    leftPeak_ = rightPeak_ = 0;
  return result;
}
} // namespace airplay
