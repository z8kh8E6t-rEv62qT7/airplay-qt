#include "app/Controller.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <cstdio>

// Explicitly invoked acceptance harness. Never part of automatic CTest runs.
int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (argc != 5 && argc != 6) {
    std::fprintf(stderr, "Usage: test_live LEFT RIGHT SECONDS HOST[:PORT] "
                         "[HOST[:PORT]] (1-based channels, VASIO-32)\n");
    return 2;
  }
  bool leftOk = false, rightOk = false, durationOk = false;
  const int left = QString::fromLocal8Bit(argv[1]).toInt(&leftOk),
            right = QString::fromLocal8Bit(argv[2]).toInt(&rightOk);
  const int duration = QString::fromLocal8Bit(argv[3]).toInt(&durationOk);
  if (!leftOk || !rightOk || !durationOk || left < 1 || right < 1 ||
      left > 65536 || right > 65536 || duration < 1 ||
      duration > 3600)
    return 2;
  try {
    QList<airplay::ReceiverEndpoint> endpoints;
    for (int i = 4; i < argc; ++i)
      endpoints.append(
          airplay::parseReceiverEndpoint(QString::fromLocal8Bit(argv[i])));
    airplay::validateEndpoints(endpoints);
    QWidget window;
    window.setWindowTitle("AirPlayQt 实机验收");
    window.resize(400, 80);
    app::Controller controller;
    QTimer stopTimer;
    stopTimer.setSingleShot(true);
    stopTimer.setTimerType(Qt::PreciseTimer);
    QElapsedTimer elapsed;
    int result = 0;
    bool running = false;
    quint64 lastReport = 0;
    double initialVolume = 0;
    enum class VolumePhase { Initial, Lower, Mute, Restore, Done };
    auto volumePhase = VolumePhase::Initial;
    QObject::connect(&controller, &app::Controller::volumeApplied, &application,
                     [&](double db) {
                       std::fprintf(stdout, "VOLUME acknowledged_db=%.6f\n",
                                    db);
                       std::fflush(stdout);
                       if (volumePhase == VolumePhase::Initial)
                         initialVolume = db;
                       else if (volumePhase == VolumePhase::Lower) {
                         QTimer::singleShot(1000, &controller, [&] {
                           if (controller.busy()) {
                             volumePhase = VolumePhase::Mute;
                             controller.volume(-144);
                           }
                         });
                       } else if (volumePhase == VolumePhase::Mute) {
                         QTimer::singleShot(1000, &controller, [&] {
                           if (controller.busy()) {
                             volumePhase = VolumePhase::Restore;
                             controller.volume(initialVolume);
                           }
                         });
                       } else if (volumePhase == VolumePhase::Restore)
                         volumePhase = VolumePhase::Done;
                     });
    QObject::connect(&controller, &app::Controller::log, &application,
                     [](const i18n::Message &text) {
                       std::fprintf(stdout, "%s\n",
                                    text.render().toUtf8().constData());
                       std::fflush(stdout);
                     });
    QObject::connect(&controller, &app::Controller::group, &application,
                     [](const i18n::Message &text) {
                       std::fprintf(stdout, "GROUP %s\n",
                                    text.render().toUtf8().constData());
                       std::fflush(stdout);
                     });
    QObject::connect(&controller, &app::Controller::error, &application,
                     [&](const i18n::Message &text) {
                       std::fprintf(stderr, "ERROR %s\n",
                                    text.render().toUtf8().constData());
                       result = 1;
                       if (!controller.busy())
                         application.exit(result);
                     });
    QObject::connect(&controller, &app::Controller::streamingChanged,
                     &application, [&](bool active) {
                       if (active && !running) {
                         running = true;
                         elapsed.start();
                         stopTimer.start(duration * 1000);
                         if (duration >= 10)
                           QTimer::singleShot(2000, &controller, [&] {
                             if (controller.busy()) {
                               volumePhase = VolumePhase::Lower;
                               controller.volume(
                                   std::max(-144., initialVolume - 3));
                             }
                           });
                       }
                     });
    QObject::connect(&stopTimer, &QTimer::timeout, &controller,
                     &app::Controller::stop);
    QObject::connect(&controller, &app::Controller::stopped, &application, [&] {
      std::fprintf(stdout, "FINISHED elapsed_ms=%lld result=%d\n",
                   running ? elapsed.elapsed() : 0, result);
      application.exit(result);
    });
    QObject::connect(
        &controller, &app::Controller::telemetry, &application,
        [&](double l, double r, double backlog, quint64 packets,
            quint64 retransmitted, quint64 expired) {
          if (packets - lastReport >= 1250 || lastReport == 0) {
            lastReport = packets;
            std::fprintf(stdout,
                         "STATS elapsed_ms=%lld L=%.6f R=%.6f backlog_ms=%.3f "
                         "packets=%llu retransmits=%llu expired=%llu\n",
                         elapsed.elapsed(), l, r, backlog * 1000, packets,
                         retransmitted, expired);
            std::fflush(stdout);
          }
        });
    app::Settings settings = controller.initialize();
    settings.driverId.clear();
    for (const auto &driver : controller.drivers())
      if (driver.name.contains("VASIO-32", Qt::CaseInsensitive))
        settings.driverId = driver.id;
    if (settings.driverId.isEmpty())
      throw std::runtime_error("VB-Matrix VASIO-32 registration not found");
    settings.left = left - 1;
    settings.right = right - 1;
    QTimer::singleShot(0, &application, [&] {
      controller.selectDriver(settings.driverId,
                              reinterpret_cast<void *>(window.winId()));
      controller.start(settings, endpoints);
    });
    return application.exec();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "ERROR %s\n", e.what());
    return 1;
  }
}
