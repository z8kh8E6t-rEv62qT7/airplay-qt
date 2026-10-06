#pragma once
#include "AudioSendCore.h"
#include "NetworkBinding.h"
#include <QString>
#include <memory>

namespace airplay {
// Control-thread facade. The native worker owns the audio timeline and data
// socket. The control socket owner must stop/join this sender before closing
// it.
class RealtimeAudioSender {
public:
  RealtimeAudioSender(const app::Timing &, audio::CaptureStream,
                      uint16_t sequence, uint32_t rtp, size_t peers);
  ~RealtimeAudioSender();
  RealtimeAudioSender(const RealtimeAudioSender &) = delete;
  RealtimeAudioSender &operator=(const RealtimeAudioSender &) = delete;
  QString schedulingLog() const;
  bool ready() const;
  uint16_t bindPeer(size_t, const QHostAddress &local,
                    const QHostAddress &remote, const NetworkRoute &,
                    qintptr controlDescriptor);
  void preparePeer(size_t, const QByteArray &key, uint16_t dataPort);
  void begin();
  void stop() noexcept;
  bool retransmit(const RetransmitRequest &) noexcept;
  void telemetry(bool, uint64_t revision) noexcept;
  SendError error() const noexcept;
  SendReport poll();

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace airplay
