// Same harness can be copied into the archived pre-change source tree. The
// compile-time API selection below is benchmark-only, never a product backend.
#include "airplay/RealtimeAudioSender.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace airplay;
namespace {
struct Receivers {
  std::array<quint16, 2> ports{};
  std::array<uint64_t, 2> audio{}, retransmits{}, maxGapNs{};
  std::array<std::array<uint64_t, 6>, 2> gapHistogram{};
  std::atomic<bool> ready{false}, stop{false};
  bool valid = false;
  void run(int peers) {
    std::array<QUdpSocket, 2> sockets;
    valid = true;
    for (int i = 0; i < peers; ++i) {
      valid &= sockets[i].bind(QHostAddress::LocalHost, 0);
      ports[i] = sockets[i].localPort();
    }
    std::array<int64_t, 2> last{};
    QElapsedTimer clock;
    clock.start();
    ready.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire)) {
      for (int i = 0; i < peers; ++i) {
        for (int batch = 0; batch < 256 && sockets[i].hasPendingDatagrams();
             ++batch) {
          const auto data = sockets[i].receiveDatagram().data();
          if (data.size() < 2)
            continue;
          if (uint8_t(data[1]) == 0xd6) {
            ++retransmits[i];
          } else {
            const auto now = clock.nsecsElapsed();
            if (audio[i]) {
              const auto gap = uint64_t(now - last[i]);
              maxGapNs[i] = std::max(maxGapNs[i], gap);
              const size_t bucket = gap <= 1000000    ? 0
                                    : gap <= 2000000  ? 1
                                    : gap <= 5000000  ? 2
                                    : gap <= 10000000 ? 3
                                    : gap <= 50000000 ? 4
                                                      : 5;
              ++gapHistogram[i][bucket];
            }
            last[i] = now;
            ++audio[i];
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }
};
struct QtBaselineSockets {
  std::array<QUdpSocket, 2> data, control;
  std::array<quint16, 2> ports{};
  static bool send(void *context, size_t peer, bool retransmit, uint16_t port,
                   std::span<const unsigned char> packet) noexcept {
    auto &self = *static_cast<QtBaselineSockets *>(context);
    auto &socket = retransmit ? self.control[peer] : self.data[peer];
    return socket.writeDatagram(reinterpret_cast<const char *>(packet.data()),
                                qint64(packet.size()), QHostAddress::LocalHost,
                                retransmit ? port : self.ports[peer]) ==
           qint64(packet.size());
  }
};
template <class Sender> constexpr bool oldWindowsBackend() {
#ifdef Q_OS_WIN
  return requires(Sender &sender) {
    sender.poll(QtBaselineSockets::send, nullptr);
  };
#else
  return false;
#endif
}
template <class Sender>
SendReport poll(Sender &sender, QtBaselineSockets &sockets) {
  if constexpr (oldWindowsBackend<Sender>())
    return sender.poll(QtBaselineSockets::send, &sockets);
  else
    return sender.poll();
}
template <class Report>
void addDeadlineMetrics(QJsonObject &output, const Report &report) {
  if constexpr (requires {
                  report.deadlineHistogram;
                  report.requestHighWater;
                }) {
    QJsonArray histogram;
    for (auto count : report.deadlineHistogram)
      histogram.append(qint64(count));
    output["deadline_buckets_0_100us_500us_1ms_5ms_over"] = histogram;
    output["max_lateness_ns"] = qint64(report.maxLatenessNs);
    output["request_high_water"] = qint64(report.requestHighWater);
  }
}
QJsonObject run(int peers, const QString &scenario, int durationMs) {
  Receivers receivers;
  std::thread receiver([&] { receivers.run(peers); });
  while (!receivers.ready.load(std::memory_order_acquire))
    std::this_thread::yield();
  // All joined before any referenced storage or socket owner is destroyed.
  struct ReceiverJoin {
    Receivers &state;
    std::thread &thread;
    ~ReceiverJoin() {
      state.stop.store(true);
      if (thread.joinable())
        thread.join();
    }
  } receiverJoin{receivers, receiver};
  if (!receivers.valid)
    throw std::runtime_error("Benchmark receiver bind failed");
  app::Timing timing;
  timing.lateMs =
      500; // Observe 150ms control stalls without terminating baseline.
  audio::CaptureStream stream{
      std::make_shared<audio::CaptureQueue>(64, 128, 128, 1024),
      audio::format(16), audio::format(16), 64};
  stream.gapPolicy = audio::GapPolicy::Silence;
  QtBaselineSockets sockets;
  sockets.ports = receivers.ports;
  RealtimeAudioSender sender(timing, stream, 0, 0, size_t(peers));
  if (!sender.ready())
    throw std::runtime_error(sender.schedulingLog().toStdString());
  for (int i = 0; i < peers; ++i) {
    if (!sockets.control[i].bind(QHostAddress::LocalHost, 0))
      throw std::runtime_error("Benchmark control bind failed");
    if constexpr (oldWindowsBackend<RealtimeAudioSender>()) {
      if (!sockets.data[i].bind(QHostAddress::LocalHost, 0))
        throw std::runtime_error("Benchmark data bind failed");
    } else {
      sender.bindPeer(size_t(i), QHostAddress::LocalHost,
                      QHostAddress::LocalHost, {},
                      sockets.control[i].socketDescriptor());
    }
    sender.preparePeer(size_t(i), QByteArray(32, 'k'), sockets.ports[i]);
  }
  std::atomic<bool> stopInput{false};
  std::thread producer([&] {
    if (scenario == "silence")
      return;
    std::array<int16_t, 64> pcm{};
    pcm.fill(100);
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t frames = 0; !stopInput.load(); frames += 64) {
      stream.queue->append(pcm.data(), pcm.data(), pcm.size());
      const auto ns = int64_t((frames + 64) * 1000000000 / 44100);
      std::this_thread::sleep_until(start + std::chrono::nanoseconds(ns));
    }
  });
  struct ProducerJoin {
    std::atomic<bool> &stop;
    std::thread &thread;
    ~ProducerJoin() {
      stop.store(true);
      thread.join();
    }
  } producerJoin{stopInput, producer};
  sender.begin();
  QElapsedTimer elapsed;
  elapsed.start();
  bool stalled = false;
  int64_t lastRequestMs = 0;
  uint64_t requestCount = 0;
  QEventLoop loop;
  QTimer timer;
  timer.setInterval(1);
  timer.setTimerType(Qt::PreciseTimer);
  QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
    if (elapsed.elapsed() >= durationMs ||
        sender.error().code != SendFailure::None) {
      loop.quit();
      return;
    }
    const auto report = poll(sender, sockets);
    if (scenario == "control-stall" && !stalled &&
        elapsed.elapsed() >= durationMs / 3) {
      stalled = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (scenario == "retransmit" && elapsed.elapsed() - lastRequestMs >= 20 &&
        report.packets >= 32) {
      lastRequestMs = elapsed.elapsed();
      for (int i = 0; i < peers; ++i) {
        if (!sender.retransmit({uint16_t(i), sockets.ports[i],
                                uint16_t(requestCount++),
                                uint16_t(report.packets - 32), 32}))
          break;
      }
    }
  });
  timer.start();
  loop.exec();
  timer.stop();
  sender.stop();
  receivers.stop.store(true);
  // Receiver metrics are read after join by explicitly completing its worker.
  receiver.join();
  const auto report = poll(sender, sockets);
  QJsonArray received, retransmitted, gaps, histograms;
  for (int i = 0; i < peers; ++i) {
    received.append(qint64(receivers.audio[i]));
    retransmitted.append(qint64(receivers.retransmits[i]));
    gaps.append(qint64(receivers.maxGapNs[i]));
    QJsonArray histogram;
    for (auto count : receivers.gapHistogram[i])
      histogram.append(qint64(count));
    histograms.append(histogram);
  }
  QJsonObject output{
      {"scenario", scenario},
      {"peers", peers},
      {"backend",
       oldWindowsBackend<RealtimeAudioSender>() ? "qt-baseline" : "native"},
      {"duration_ms", qint64(elapsed.elapsed())},
      {"received", received},
      {"received_retransmits", retransmitted},
      {"receiver_max_gap_ns", gaps},
      {"receiver_gap_buckets_1ms_2ms_5ms_10ms_50ms_over", histograms},
      {"max_work_ns", qint64(report.maxWorkNs)},
      {"max_gap_ns", qint64(report.maxGapNs)},
      {"buffer_high_water_frames", qint64(report.maxBuffered)},
      {"sent_packets", qint64(report.packets)},
      {"retransmitted", qint64(report.retransmitted)},
      {"send_error", int(sender.error().code)},
      {"native_error", sender.error().detail}};
  addDeadlineMetrics(output, report);
  return output;
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  bool valid = false;
  const int duration =
      argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt(&valid) : 1000;
  if (argc > 1 && (!valid || duration < 500 || duration > 60000))
    return 2;
  try {
    for (int peers : {1, 2})
      for (const QString &scenario :
           {QString("input"), QString("silence"), QString("retransmit"),
            QString("control-stall")}) {
        const auto result = run(peers, scenario, duration);
        const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
        std::puts(json.constData());
        if (result["send_error"].toInt())
          return 1;
      }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
