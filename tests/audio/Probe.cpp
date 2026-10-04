#include "audio/AsioCapture.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Inspect ASIO inputs 31/32 without recording or sending audio.
int main(int argc, char **argv) {
  QApplication application(argc, argv);
  try {
    QWidget window;
    audio::AsioCapture capture;
    QString selected;
    for (const auto &driver : audio::AsioCapture::enumerate())
      if (driver.name.contains("VASIO-32", Qt::CaseInsensitive))
        selected = driver.id;
    if (selected.isEmpty())
      throw std::runtime_error("VASIO-32 not registered");
    const auto channels =
        capture.open(selected, reinterpret_cast<void *>(window.winId()));
    if (channels.size() < 32)
      throw std::runtime_error("Driver has fewer than 32 input channels");
    for (int index : {30, 31}) {
      const auto &channel = channels[index];
      const auto format = audio::format(channel.type);
      std::printf("INPUT channel=%d sdk_index=%d isInput=1 name=%s type=%ld "
                  "bytes=%d valid_bits=%d float=%d big_endian=%d\n",
                  index + 1, index, channel.name.render().toUtf8().constData(),
                  channel.type, format.bytes, format.bits, format.floating,
                  format.bigEndian);
    }
    auto stream = capture.prepare(30, 31, 352, 65536);
    std::printf("BUFFER frames=%ld rate=44100\n", stream.blockFrames);
    uint64_t callbacks = 0, leftNonzero = 0, rightNonzero = 0;
    double leftPeak = 0, rightPeak = 0;
    int result = 0;
    QElapsedTimer elapsed;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &application, [&] {
      try {
        if (stream.queue->fault.load())
          throw std::runtime_error("ASIO callback reported a fault");
        std::span<const std::byte> l, r;
        while (stream.queue->peek(l, r)) {
          leftNonzero += std::count_if(
              l.begin(), l.end(), [](std::byte b) { return b != std::byte{}; });
          rightNonzero += std::count_if(
              r.begin(), r.end(), [](std::byte b) { return b != std::byte{}; });
          const auto pcm = audio::convert(l, stream.left, r, stream.right);
          for (size_t i = 0; i < pcm.size(); i += 2) {
            leftPeak = std::max(leftPeak, std::abs(double(pcm[i])) / 32768);
            rightPeak =
                std::max(rightPeak, std::abs(double(pcm[i + 1])) / 32768);
          }
          ++callbacks;
          stream.queue->pop();
        }
        if (elapsed.elapsed() >= 5000) {
          poll.stop();
          if (const auto error = capture.stop(); !error.isEmpty())
            throw std::runtime_error(error.render().toStdString());
          std::printf(
              "RESULT elapsed_ms=%lld callbacks=%llu captured_frames=%llu "
              "left_nonzero_bytes=%llu right_nonzero_bytes=%llu left_peak=%.9f "
              "right_peak=%.9f\n",
              elapsed.elapsed(), callbacks, stream.queue->capturedFrames(),
              leftNonzero, rightNonzero, leftPeak, rightPeak);
          std::fflush(stdout);
          application.quit();
        }
      } catch (const std::exception &e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
        result = 1;
        poll.stop();
        capture.stop();
        application.exit(result);
      }
    });
    std::fflush(stdout);
    capture.start();
    elapsed.start();
    poll.start();
    application.exec();
    return result;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ERROR %s\n", e.what());
    return 1;
  }
}
