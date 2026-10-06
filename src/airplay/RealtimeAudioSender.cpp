#include "RealtimeAudioSender.h"
#include "PtpClock.h"
#include "RealtimeWait.h"
#include "ThreadScheduling.h"
#include <atomic>
#include <cerrno>
#include <future>
#include <system_error>
#include <thread>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif

namespace airplay {
#ifdef Q_OS_WIN
// Own our Winsock reference; do not depend on Qt's initialization lifetime.
struct WinsockRuntime {
  WinsockRuntime() {
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error)
      throw std::system_error(error, std::system_category(),
                              "Audio WSAStartup");
  }
  ~WinsockRuntime() { WSACleanup(); }
};
#endif
struct RealtimeAudioSender::State {
  AudioSendCore core;
  SchedulingResult scheduling;
  RealtimeQueue<RetransmitRequest, 64> requests;
  RetransmitRequest pending{};
  std::atomic<uint64_t> failure{0}, telemetryState{1};
  SendReport latest{};
  bool started = false, stopped = false;
#ifdef Q_OS_WIN
  WinsockRuntime winsock;
#endif
  struct Peer {
#ifdef Q_OS_WIN
    SOCKET data = INVALID_SOCKET, control = INVALID_SOCKET;
#else
    int data = -1, control = -1;
#endif
    sockaddr_in target{};
    ~Peer() {
#ifdef Q_OS_WIN
      if (data != INVALID_SOCKET)
        closesocket(data);
      // control is borrowed from Qt; its owner joins us before closing it.
#else
      if (data >= 0)
        close(data);
      if (control >= 0)
        close(control);
#endif
    }
  };
  std::array<Peer, 2> peers;
  RealtimeWait wait;
  RealtimeQueue<SendReport, 64> reports;
  std::atomic<bool> stopping{false}, active{false};
  std::thread thread;
  int sendError = 0;
  State(const app::Timing &timing, audio::CaptureStream stream,
        uint16_t sequence, uint32_t rtp, size_t peersCount)
      : core(timing, std::move(stream), sequence, rtp, peersCount) {
    std::promise<SchedulingResult> initialized;
    auto result = initialized.get_future();
    thread =
        std::thread([this, initialization = std::move(initialized)]() mutable {
          try {
            AudioThreadScheduling registration;
            const bool ready = registration.result().ready;
            initialization.set_value(registration.result());
            if (!ready)
              return;
            run();
          } catch (...) {
            try {
              initialization.set_exception(std::current_exception());
            } catch (...) {
            }
            fail({SendFailure::Wait, EIO});
          }
        });
    try {
      scheduling = result.get();
    } catch (...) {
      stopping.store(true);
      wait.wake();
      thread.join();
      throw;
    }
  }
  ~State() { stop(); }
  void fail(SendError error) noexcept {
    uint64_t empty = 0;
    failure.compare_exchange_strong(
        empty, uint64_t(error.code) | (uint64_t(uint32_t(error.detail)) << 32),
        std::memory_order_release, std::memory_order_relaxed);
  }
  void stop() noexcept {
    stopped = true;
    stopping.store(true, std::memory_order_release);
    wait.wake();
    if (thread.joinable())
      thread.join();
  }
  SendError serviceRetransmissions(AudioSendCore::Send send,
                                   void *context) noexcept {
    int budget = 32;
    while (budget > 0) {
      // Recheck the absolute audio deadline between retransmitted packets.
      if (wait.now() >= core.deadline())
        break;
      if (!pending.count && !requests.pop(pending))
        break;
      int one = 1;
      if (auto error = core.retransmit(pending, one, send, context);
          error.code != SendFailure::None)
        return error;
      --budget;
    }
    return {};
  }
  static bool send(void *context, size_t index, bool control, uint16_t port,
                   std::span<const unsigned char> packet) noexcept {
    auto &self = *static_cast<State *>(context);
    auto &peer = self.peers[index];
    auto target = peer.target;
    if (control)
      target.sin_port = htons(port);
    const auto size = sendto(
        control ? peer.control : peer.data,
        reinterpret_cast<const char *>(packet.data()), int(packet.size()), 0,
        reinterpret_cast<const sockaddr *>(&target), sizeof(target));
#ifdef Q_OS_WIN
    self.sendError = size == SOCKET_ERROR
                         ? WSAGetLastError()
                         : (size_t(size) == packet.size() ? 0 : WSAEMSGSIZE);
#else
    self.sendError =
        size < 0 ? errno : (size_t(size) == packet.size() ? 0 : EIO);
#endif
    return self.sendError == 0;
  }
  void run() noexcept {
    while (!stopping.load(std::memory_order_acquire) &&
           !active.load(std::memory_order_acquire))
      if (!wait.until(wait.now() + 1000000000)) {
        fail({SendFailure::Wait, RealtimeWait::lastError()});
        return;
      }
    if (stopping.load(std::memory_order_acquire))
      return;
    if (!core.warmup()) {
      fail({SendFailure::Encode});
      return;
    }
    core.begin(wait.now());
    while (!stopping.load(std::memory_order_acquire) &&
           !failure.load(std::memory_order_acquire)) {
      const auto now = wait.now();
#ifdef Q_OS_WIN
      const auto wall = wallNs();
#else
      timespec stamp{};
      if (clock_gettime(CLOCK_REALTIME, &stamp)) {
        fail({SendFailure::Wait, errno});
        break;
      }
      const auto wall = int64_t(stamp.tv_sec) * 1000000000 + stamp.tv_nsec;
#endif
      if (now < 0) {
        fail({SendFailure::Wait, RealtimeWait::lastError()});
        break;
      }
      const auto telemetry = telemetryState.load(std::memory_order_acquire);
      core.telemetry(telemetry & 1, telemetry >> 1);
      auto error = core.step(now, wall, send, this);
      if (error.code == SendFailure::None)
        error = serviceRetransmissions(send, this);
      core.workTime(uint64_t(std::max(int64_t(0), wait.now() - now)));
      if (error.code != SendFailure::None) {
        if (error.code == SendFailure::Send ||
            error.code == SendFailure::Retransmit)
          error.detail = sendError;
        fail(error);
        break;
      }
      // Telemetry is a bounded, lossy observer channel. Never block audio if
      // the UI/control thread is delayed; its next snapshot is cumulative.
      if (reports.push(core.report()))
        core.report(true);
      if (!wait.until(core.deadline())) {
        fail({SendFailure::Wait, RealtimeWait::lastError()});
        break;
      }
    }
  }
};
RealtimeAudioSender::RealtimeAudioSender(const app::Timing &timing,
                                         audio::CaptureStream stream,
                                         uint16_t sequence, uint32_t rtp,
                                         size_t peers)
    : state_(std::make_unique<State>(timing, std::move(stream), sequence, rtp,
                                     peers)) {}
RealtimeAudioSender::~RealtimeAudioSender() = default;
QString RealtimeAudioSender::schedulingLog() const {
  return state_->scheduling.description;
}
bool RealtimeAudioSender::ready() const { return state_->scheduling.ready; }
uint16_t RealtimeAudioSender::bindPeer(size_t index, const QHostAddress &local,
                                       const QHostAddress &remote,
                                       const NetworkRoute &route,
                                       qintptr controlDescriptor) {
  auto &s = *state_;
  if (s.started || index >= s.peers.size())
    throw std::logic_error("Invalid sender binding state");
  auto &p = s.peers[index];
#ifdef Q_OS_WIN
  if (p.data != INVALID_SOCKET)
#else
  if (p.data >= 0)
#endif
    throw std::logic_error("Sender socket already bound");
#ifdef Q_OS_WIN
  p.data = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                      WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  if (p.data == INVALID_SOCKET)
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "Audio UDP socket");
  u_long nonblocking = 1;
  if (ioctlsocket(p.data, FIONBIO, &nonblocking))
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "Audio UDP nonblocking mode");
  BOOL exclusive = TRUE;
  if (setsockopt(p.data, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)))
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "Audio UDP exclusive bind");
#else
  p.data = socket(AF_INET, SOCK_DGRAM, 0);
  if (p.data < 0)
    throw std::system_error(errno, std::generic_category(), "Audio UDP socket");
  if (fcntl(p.data, F_SETFD, FD_CLOEXEC) || fcntl(p.data, F_SETFL, O_NONBLOCK))
    throw std::system_error(errno, std::generic_category(), "Audio UDP flags");
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(local.toIPv4Address());
  if (::bind(p.data, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
#ifdef Q_OS_WIN
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "Audio UDP bind");
#else
    throw std::system_error(errno, std::generic_category(), "Audio UDP bind");
#endif
  }
  route.bindInterface(qintptr(p.data));
#ifdef Q_OS_WIN
  if (controlDescriptor == -1)
    throw std::invalid_argument("Audio control socket is not bound");
  p.control = SOCKET(controlDescriptor);
#else
  p.control = fcntl(int(controlDescriptor), F_DUPFD_CLOEXEC, 0);
  if (p.control < 0)
    throw std::system_error(errno, std::generic_category(),
                            "Audio control socket duplication");
  // dup shares file status flags: Qt's socket is already nonblocking. Never
  // change flags or receive from this duplicate; Qt is the only reader.
  const int flags = fcntl(p.control, F_GETFL);
  if (flags < 0)
    throw std::system_error(errno, std::generic_category(),
                            "Audio control socket flags");
  if (!(flags & O_NONBLOCK))
    throw std::runtime_error("Audio control socket must be nonblocking");
#endif
  p.target.sin_family = AF_INET;
  p.target.sin_addr.s_addr = htonl(remote.toIPv4Address());
#ifdef Q_OS_WIN
  int size = sizeof(address);
#else
  socklen_t size = sizeof(address);
#endif
  if (getsockname(p.data, reinterpret_cast<sockaddr *>(&address), &size)) {
#ifdef Q_OS_WIN
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "Audio UDP local port");
#else
    throw std::system_error(errno, std::generic_category(),
                            "Audio UDP local port");
#endif
  }
  return ntohs(address.sin_port);
}
void RealtimeAudioSender::preparePeer(size_t index, const QByteArray &key,
                                      uint16_t port) {
  if (state_->started)
    throw std::logic_error("Cannot reconfigure an active sender");
  if (!state_->core.preparePeer(
          index, {reinterpret_cast<const unsigned char *>(key.constData()),
                  size_t(key.size())}))
    throw Error(i18n::text(i18n::Id::OpenSSLOperationFailed));
  state_->peers.at(index).target.sin_port = htons(port);
}
void RealtimeAudioSender::begin() {
  auto &s = *state_;
  if (s.started || s.stopped || !ready())
    throw std::logic_error("Sender not ready");
  s.started = true;
  s.active.store(true, std::memory_order_release);
  s.wait.wake();
}
void RealtimeAudioSender::stop() noexcept { state_->stop(); }
bool RealtimeAudioSender::retransmit(
    const RetransmitRequest &request) noexcept {
  if (!state_->requests.push(request)) {
    state_->fail({SendFailure::QueueFull});
    return false;
  }
  return true;
}
void RealtimeAudioSender::telemetry(bool enabled, uint64_t revision) noexcept {
  state_->telemetryState.store((revision << 1) | uint64_t(enabled),
                               std::memory_order_release);
}
SendError RealtimeAudioSender::error() const noexcept {
  const auto error = state_->failure.load(std::memory_order_acquire);
  return {SendFailure(uint32_t(error)), int(uint32_t(error >> 32))};
}
SendReport RealtimeAudioSender::poll() {
  auto &s = *state_;
  if (s.stopping.load(std::memory_order_acquire) && !s.thread.joinable()) {
    auto result = s.core.report(true);
    result.requestHighWater = s.requests.producerHighWater();
    return result;
  }
  SendReport report;
  while (s.reports.pop(report)) {
    if (report.telemetryRevision == s.latest.telemetryRevision) {
      report.leftPeak = std::max(report.leftPeak, s.latest.leftPeak);
      report.rightPeak = std::max(report.rightPeak, s.latest.rightPeak);
    }
    s.latest = report;
  }
  auto result = s.latest;
  result.requestHighWater = s.requests.producerHighWater();
  s.latest.leftPeak = s.latest.rightPeak = 0;
  return result;
}
} // namespace airplay
