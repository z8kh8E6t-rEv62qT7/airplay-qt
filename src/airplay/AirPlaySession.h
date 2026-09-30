#pragma once
#include "PtpClock.h"
#include "ReceiverEndpoint.h"
#include "RtspClient.h"
#include "audio/CaptureStream.h"
#include <array>
#include <deque>
#include <functional>
namespace airplay {
enum class SessionEnd { Stopped, HostInterrupted, Failure };
struct ReceiverInfo {
  QString name, deviceId, stereoId, members;
  double volume;
};
ReceiverInfo receiverInfo(const QByteArray &plist);
void validateGroup(const ReceiverInfo &left, const ReceiverInfo &right);
struct SessionEnvironment {
  // Tests replace only PTP's OS service; production always uses PtpClock.
  std::function<void()> startClock, stopClock;
};
class AirPlaySession : public QObject {
  Q_OBJECT
public:
  enum class State {
    Preparing,
    Connecting,
    Synchronizing,
    Buffering,
    Streaming,
    Stopping,
    Error,
    Stopped
  };
  Q_ENUM(State)
  AirPlaySession(app::Timing timing, audio::CaptureStream stream,
                 QList<ReceiverEndpoint> endpoints, QObject *parent = nullptr,
                 SessionEnvironment environment = {});
  ~AirPlaySession() override;
  void start();
  void captureStarted();
  void stop(const QString &error = {}, SessionEnd reason = SessionEnd::Stopped);
  void volume(double db);
signals:
  void status(QString text);
  void streamingChanged(bool active);
  void log(QString text);
  void group(QString text);
  void startCapture();
  void stopCapture();
  void volumeApplied(double db);
  void telemetry(double left, double right, double backlog, quint64 packets,
                 quint64 retransmitted, quint64 expired);
  void finished(QString error, int reason);

private:
  struct Peer;
  void opened(int index);
  void reply(int index, const QByteArray &body);
  void setupGroup();
  void request(Peer &peer, const QByteArray &method,
               const QByteArray &body = {}, const QByteArray &type = {});
  void eventConnected(int index);
  void poll();
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  void logRates(bool final = false);
#endif
  void feedback(int index);
  void keepAlive();
  void dispatchVolume();
  void finishStop();
  bool allReady() const;
  bool allStopped() const;
  void setState(State state, const QString &text);
  void fail(const std::exception &error);
  app::Timing timing_;
  SessionEnvironment environment_;
  QList<ReceiverEndpoint> endpoints_;
  audio::CaptureStream stream_;
  std::vector<std::unique_ptr<Peer>> peers_;
  PtpClock clock_;
  QTimer settle_, poll_, teardown_, keepAlive_;
  QElapsedTimer elapsed_;
  State state_ = State::Preparing;
  SessionEnd endReason_ = SessionEnd::Stopped;
  QString error_, identity_, groupId_;
  QHostAddress local_;
  uint64_t clockId_ = 0, counter_ = 0, sentFrames_ = 0, retransmitted_ = 0,
           expired_ = 0;
  uint32_t firstRtp_ = 0;
  uint16_t firstSequence_ = 0;
  qint64 started_ = 0, lastInput_ = 0, lastStats_ = 0, nextSync_ = 0;
  int64_t audible_ = 0, anchorWall_ = 0;
  uint64_t seenFrames_ = 0;
  std::deque<int16_t> pcm_;
  double leftPeak_ = 0, rightPeak_ = 0, volume_ = 0;
  bool volumePending_ = false, firstSync_ = true;
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  struct RateDiagnostics {
    // Include the delay from capture start to the first network poll.
    qint64 reportedAt = 0, lastPoll = 0, maxGap = 0, maxWork = 0;
    uint64_t captured = 0, sent = 0;
  } rates_;
#endif
};
} // namespace airplay
