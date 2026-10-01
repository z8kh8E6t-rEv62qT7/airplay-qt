#include "audio/AsioCapture.h"
#include "audio/CaptureTiming.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace {
void fail(const QString &message) {
  throw std::runtime_error(message.toStdString());
}
void little(QByteArray &data, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    data.append(char(value >> (8 * i)));
}
class WaveFile {
public:
  WaveFile(const QString &path, bool floating)
      : file_(path), floating_(floating) {
    if (!file_.open(QIODevice::ReadWrite | QIODevice::NewOnly))
      fail(path + ": " + file_.errorString());
    write(header(0));
  }
  void write(const QByteArray &bytes) {
    qint64 offset = 0;
    while (offset < bytes.size()) {
      const auto written =
          file_.write(bytes.constData() + offset, bytes.size() - offset);
      if (written <= 0)
        fail(file_.fileName() + ": " + file_.errorString());
      offset += written;
    }
  }
  void finish(uint32_t frames) {
    if (!file_.isOpen())
      return;
    // Discard any incomplete pair of blocks after an I/O failure.
    const auto data = header(frames);
    if (!file_.resize(data.size() + qint64(frames) * (floating_ ? 8 : 4)) ||
        !file_.seek(0))
      fail(file_.errorString());
    write(data);
    if (!file_.flush())
      fail(file_.errorString());
    file_.close();
  }

private:
  QByteArray header(uint32_t frames) const {
    const uint32_t align = floating_ ? 8 : 4, dataBytes = frames * align;
    QByteArray out = "RIFF";
    little(out, (floating_ ? 50 : 36) + dataBytes, 4);
    out += "WAVEfmt ";
    little(out, floating_ ? 18 : 16, 4);
    little(out, floating_ ? 3 : 1, 2);
    little(out, 2, 2);
    little(out, 44100, 4);
    little(out, 44100 * align, 4);
    little(out, align, 2);
    little(out, floating_ ? 32 : 16, 2);
    if (floating_) {
      little(out, 0, 2);
      out += "fact";
      little(out, 4, 4);
      little(out, frames, 4);
    }
    out += "data";
    little(out, dataBytes, 4);
    return out;
  }
  QFile file_;
  bool floating_;
};

QByteArray interleave(std::span<const std::byte> left,
                      std::span<const std::byte> right) {
  if (left.size() != right.size() || left.size() % 4)
    fail("Invalid Float32 channel lengths");
  QByteArray raw(qsizetype(left.size() * 2), Qt::Uninitialized);
  for (size_t i = 0; i < left.size() / 4; ++i) {
    std::memcpy(raw.data() + i * 8, left.data() + i * 4, 4);
    std::memcpy(raw.data() + i * 8 + 4, right.data() + i * 4, 4);
  }
  return raw;
}

void writeTrace(const QString &path, const audio::CaptureTrace &trace,
                std::span<const uint64_t> consumed, int64_t frequency,
                long blockFrames) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    fail(path + ": " + file.errorString());
  QByteArray output =
      "callback,recording_frame,buffer_index,callback_ms,interval_ms,"
      "duration_us,thread_id,consumer_thread_id,time_callback,direct_process,"
      "time_flags,"
      "sample_position,driver_system_time,queued,source_before_hash,"
      "copied_hash,source_after_hash,consumed_hash,copy_matches_source,"
      "queue_matches_consumer,trace_overflow,qpc_frequency,"
      "timer_period_request_ms,ignore_timer_resolution_disabled\n";
  size_t consumer = 0;
  uint64_t frame = 0;
  const int64_t origin = trace.used ? trace.entries[0].beginTicks : 0;
  for (size_t i = 0; i < trace.used; ++i) {
    const auto &entry = trace.entries[i];
    const bool hasConsumer = entry.queued && consumer < consumed.size();
    const auto hash = hasConsumer ? consumed[consumer] : 0;
    const double interval =
        i ? double(entry.beginTicks - trace.entries[i - 1].beginTicks) * 1000 /
                frequency
          : 0;
    QStringList fields{
        QString::number(i),
        QString::number(frame),
        QString::number(entry.bufferIndex),
        QString::number(double(entry.beginTicks - origin) * 1000 / frequency,
                        'f', 6),
        QString::number(interval, 'f', 6),
        QString::number(double(entry.endTicks - entry.beginTicks) * 1000000 /
                            frequency,
                        'f', 6),
        QString::number(entry.threadId),
        QString::number(GetCurrentThreadId()),
        QString::number(entry.timeCallback),
        QString::number(entry.directProcess),
        QString::number(entry.timeFlags),
        QString::number(entry.samplePosition),
        QString::number(entry.systemTime),
        QString::number(entry.queued),
        QString::number(entry.sourceBefore, 16),
        QString::number(entry.copied, 16),
        QString::number(entry.sourceAfter, 16),
        hasConsumer ? QString::number(hash, 16) : QString{},
        entry.queued ? QString::number(entry.sourceBefore == entry.copied &&
                                       entry.copied == entry.sourceAfter)
                     : QString{},
        hasConsumer ? QString::number(hash == entry.copied) : QString{},
        QString::number(trace.overflow),
        QString::number(frequency),
        "1",
        "1"};
    output += fields.join(',').toUtf8() + '\n';
    if (entry.queued) {
      ++consumer;
      frame += uint64_t(blockFrames);
    }
  }
  if (file.write(output) != output.size() || !file.flush())
    fail(path + ": " + file.errorString());
}

// Offline output check uses the same writer and converter without opening ASIO.
void checkOutput(const QString &prefix) {
  const std::array<float, 5> left{0, .5f, -.5f, 1.25f, -1.25f},
      right{.25f, -.25f, 0, -1, 1};
  const auto l = std::as_bytes(std::span(left)),
             r = std::as_bytes(std::span(right));
  const auto pcm = audio::convert(l, audio::format(19), r, audio::format(19));
  WaveFile raw(prefix + "-raw-f32.wav", true),
      converted(prefix + "-pcm16.wav", false);
  raw.write(interleave(l, r));
  converted.write(QByteArray(reinterpret_cast<const char *>(pcm.data()),
                             qsizetype(pcm.size() * 2)));
  raw.finish(5);
  converted.finish(5);
  audio::CaptureTrace trace(3);
  trace.used = 3;
  for (size_t i = 0; i < 3; ++i) {
    auto &entry = trace.entries[i];
    entry.beginTicks = 1000000 + int64_t(i) * 23000;
    entry.endTicks = entry.beginTicks + 25;
    entry.bufferIndex = long(i % 2);
    entry.queued = i != 1;
    entry.threadId = 123;
    entry.sourceBefore = entry.copied = entry.sourceAfter = 42;
  }
  const std::array<uint64_t, 1> consumed{42};
  writeTrace(prefix + "-callbacks.csv", trace, consumed, 1000000, 1024);
}
} // namespace

// Record VASIO-32 inputs 31/32 and callback diagnostics.
int main(int argc, char **argv) {
  static_assert(std::endian::native == std::endian::little);
  QApplication application(argc, argv);
  if (argc != 3) {
    std::fprintf(stderr, "Usage: test_record SECONDS OUTPUT_PREFIX (1..300 "
                         "seconds, ASIO inputs 31/32)\n");
    return 2;
  }
  const auto prefix =
      QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
  try {
    if (!QDir().mkpath(QFileInfo(prefix).absolutePath()))
      fail("Cannot create recording directory");
    if (QByteArray(argv[1]) == "--check-output") {
      audio::CaptureTiming timerPrecision;
      checkOutput(prefix);
      timerPrecision.restore();
      timerPrecision.verifyRestored();
      std::printf("TIMER restored=1\n");
      return 0;
    }
    bool valid = false;
    const int seconds = QString::fromLatin1(argv[1]).toInt(&valid);
    if (!valid || seconds < 1 || seconds > 300)
      fail("Duration must be 1..300 seconds");
    if (QFileInfo::exists(prefix + "-callbacks.csv"))
      fail("Callback trace already exists; choose a new output prefix");
    QWidget window;
    audio::AsioCapture capture;
    QString selected;
    for (const auto &driver : audio::AsioCapture::enumerate())
      if (driver.name.contains("VASIO-32", Qt::CaseInsensitive))
        selected = driver.id;
    if (selected.isEmpty())
      fail("VASIO-32 not registered");
    const auto channels =
        capture.open(selected, reinterpret_cast<void *>(window.winId()));
    if (channels.size() < 32)
      fail("Driver has fewer than 32 input channels");
    for (int i : {30, 31}) {
      const auto format = audio::format(channels[i].type);
      if (!format.floating || format.bytes != 4 || format.bigEndian)
        fail("This diagnostic requires native little-endian Float32 inputs");
      std::printf("INPUT %d: %s, ASIO type %ld\n", i + 1,
                  channels[i].name.render().toUtf8().constData(),
                  channels[i].type);
    }
    auto stream = capture.prepare(30, 31, 1.);
    std::printf("TIMER request_ms=1 ignore_timer_resolution_disabled=1\n");
    std::fflush(stdout);
    const size_t traceCapacity =
        (uint64_t(seconds) * 44100 + stream.blockFrames - 1) /
            stream.blockFrames +
        128;
    audio::CaptureTrace trace(traceCapacity);
    std::vector<uint64_t> consumed(traceCapacity);
    size_t consumedCount = 0;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
      fail("QueryPerformanceFrequency failed");
    capture.setTrace(&trace);
    WaveFile raw(prefix + "-raw-f32.wav", true),
        converted(prefix + "-pcm16.wav", false);
    const uint32_t target = uint32_t(seconds) * 44100;
    uint32_t frames = 0;
    uint64_t overRange = 0, nonzero = 0;
    double peak = 0;
    int result = 0;
    const auto stopCapture = [&] {
      if (const auto error = capture.stop(); !error.isEmpty()) {
        std::fprintf(stderr, "ERROR %s\n", error.render().toUtf8().constData());
        result = 1;
      }
    };
    QTimer poll;
    QElapsedTimer elapsed;
    poll.setInterval(2);
    poll.setTimerType(Qt::PreciseTimer);
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
      try {
        if (const int fault = stream.queue->fault.load())
          fail(QString("ASIO capture fault %1; recording is incomplete")
                   .arg(fault));
        std::span<const std::byte> left, right;
        while (frames < target && stream.queue->peek(left, right)) {
          if (consumedCount == consumed.size())
            fail("Consumer trace capacity exceeded");
          consumed[consumedCount++] = audio::captureChecksum(left, right);
          const size_t count =
              std::min(size_t(target - frames), left.size() / 4);
          left = left.first(count * 4);
          right = right.first(count * 4);
          const auto pcm =
              audio::convert(left, stream.left, right, stream.right);
          for (const auto channel : {left, right})
            for (size_t i = 0; i < count; ++i) {
              float value;
              std::memcpy(&value, channel.data() + i * 4, 4);
              peak = std::max(peak, std::abs(double(value)));
              overRange += std::abs(value) > 1;
              nonzero += value != 0;
            }
          raw.write(interleave(left, right));
          converted.write(QByteArray(reinterpret_cast<const char *>(pcm.data()),
                                     qsizetype(pcm.size() * 2)));
          frames += uint32_t(count);
          stream.queue->pop();
        }
        if (frames == target) {
          poll.stop();
          stopCapture();
          application.quit();
        } else if (elapsed.elapsed() > qint64(seconds + 5) * 1000)
          fail("Capture timeout; recording is incomplete");
      } catch (const std::exception &e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        result = 1;
        poll.stop();
        stopCapture();
        application.quit();
      }
    });
    try {
      capture.start();
      elapsed.start();
      poll.start();
      std::printf("RECORDING %d seconds, 44100 Hz, stereo, block=%ld\n",
                  seconds, stream.blockFrames);
      std::fflush(stdout);
      application.exec();
    } catch (const std::exception &e) {
      std::fprintf(stderr, "ERROR %s\n", e.what());
      result = 1;
    }
    poll.stop();
    stopCapture();
    if (result == 0)
      std::printf("TIMER restored=1\n");
    // Both files end on exactly the same successfully captured frame.
    raw.finish(frames);
    converted.finish(frames);
    writeTrace(prefix + "-callbacks.csv", trace,
               std::span<const uint64_t>(consumed.data(), consumedCount),
               frequency.QuadPart, stream.blockFrames);
    std::printf("RESULT frames=%u seconds=%.6f nonzero_samples=%llu "
                "raw_peak=%.9f over_range_samples=%llu complete=%d\n",
                frames, double(frames) / 44100, nonzero, peak, overRange,
                frames == target && result == 0);
    std::printf("RAW %s-raw-f32.wav\nPCM16 %s-pcm16.wav\n",
                prefix.toUtf8().constData(), prefix.toUtf8().constData());
    std::printf("TRACE %s-callbacks.csv entries=%zu overflow=%zu\n",
                prefix.toUtf8().constData(), trace.used, trace.overflow);
    std::fflush(stdout);
    return result;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ERROR %s\n", e.what());
    return 1;
  }
}
