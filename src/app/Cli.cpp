#include "Cli.h"
#include "Controller.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <signal.h>

namespace app {
namespace {
void report(const char *event, QJsonObject fields = {}) {
  fields.insert("event", QString::fromLatin1(event));
  fields.insert("time",
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  const auto line = QJsonDocument(fields).toJson(QJsonDocument::Compact);
  std::fprintf(stdout, "%s\n", line.constData());
  std::fflush(stdout);
}
struct Options {
  QString device;
  int left = 1, right = 2, seconds = 30, startupTimeout = 60;
  QList<airplay::ReceiverEndpoint> receivers;
};
int number(const QCommandLineParser &parser, const char *name, int maximum) {
  bool ok = false;
  const int value = parser.value(name).toInt(&ok);
  if (!ok || value < 1 || value > maximum)
    throw std::invalid_argument(QString("--%1 must be an integer in 1..%2")
                                    .arg(name)
                                    .arg(maximum)
                                    .toStdString());
  return value;
}
Options options(const QCommandLineParser &parser) {
  if (!parser.positionalArguments().isEmpty())
    throw std::invalid_argument("Unexpected positional arguments");
  for (const auto &name : parser.optionNames())
    if (name != "receiver" && parser.optionNames().count(name) > 1)
      throw std::invalid_argument(("Repeated option: --" + name).toStdString());
  Options o;
  o.device = parser.value("device");
  o.left = number(parser, "left", 256);
  o.right = number(parser, "right", 256);
  o.seconds = number(parser, "seconds", 3600);
  o.startupTimeout = number(parser, "startup-timeout", 3600);
  if (o.device.isEmpty())
    throw std::invalid_argument("--device UID is required; use --list-devices");
  if (o.left == o.right)
    throw std::invalid_argument("Select two different input channels");
  try {
    for (const auto &value : parser.values("receiver"))
      o.receivers.append(airplay::parseReceiverEndpoint(value));
    airplay::validateEndpoints(o.receivers);
  } catch (const std::exception &e) {
    throw std::invalid_argument(e.what());
  }
  return o;
}

volatile sig_atomic_t interrupted = 0;
void interrupt(int signal) { interrupted = signal; }
class Interrupts {
public:
  Interrupts() {
    interrupted = 0;
    struct sigaction action{};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &action, &previousInt_) != 0)
      throw std::runtime_error(std::strerror(errno));
    if (sigaction(SIGTERM, &action, &previousTerm_) != 0) {
      const int error = errno;
      sigaction(SIGINT, &previousInt_, nullptr);
      throw std::runtime_error(std::strerror(error));
    }
  }
  ~Interrupts() {
    sigaction(SIGTERM, &previousTerm_, nullptr);
    sigaction(SIGINT, &previousInt_, nullptr);
  }
  Interrupts(const Interrupts &) = delete;
  Interrupts &operator=(const Interrupts &) = delete;

private:
  struct sigaction previousInt_{}, previousTerm_{};
};

int transmit(QApplication &application, const Options &o) {
  Interrupts interrupts;
  Controller controller;
  QObject context;
  QTimer duration, startup, interruptPoll;
  duration.setSingleShot(true);
  duration.setTimerType(Qt::PreciseTimer);
  startup.setSingleShot(true);
  QElapsedTimer elapsed, lastReport;
  int result = 0;
  bool completed = false, finished = false;
  quint64 packets = 0, retransmitted = 0, expired = 0;
  double maxBacklog = 0, peakLeft = 0, peakRight = 0;
  auto finish = [&] {
    if (finished)
      return;
    finished = true;
    duration.stop();
    startup.stop();
    interruptPoll.stop();
    if (!result && (!completed || !packets))
      result = 1;
    report("finished",
           {{"exit_code", result},
            {"streaming_ms", elapsed.isValid() ? elapsed.elapsed() : 0},
            {"packets_per_receiver", qint64(packets)},
            {"retransmitted", qint64(retransmitted)},
            {"expired", qint64(expired)},
            {"max_backlog_ms", maxBacklog * 1000},
            {"peak_left", peakLeft},
            {"peak_right", peakRight}});
    application.exit(result);
  };
  auto stop = [&] {
    controller.stop();
    if (!controller.busy())
      finish();
  };
  QObject::connect(&controller, &Controller::error, &context,
                   [&](const i18n::Message &text) {
                     report("error", {{"message", text.render()}});
                     if (!result)
                       result = 1;
                     if (!controller.busy())
                       finish();
                   });
  QObject::connect(&controller, &Controller::stopped, &context, finish);
  QObject::connect(&controller, &Controller::status, &context,
                   [](const i18n::Message &text) {
                     report("status", {{"message", text.render()}});
                   });
  QObject::connect(&controller, &Controller::log, &context,
                   [](const i18n::Message &text) {
                     report("log", {{"message", text.render()}});
                   });
  QObject::connect(&controller, &Controller::group, &context,
                   [](const i18n::Message &text) {
                     report("group", {{"message", text.render()}});
                   });
  QObject::connect(&controller, &Controller::volumeApplied, &context,
                   [](double value) { report("volume", {{"db", value}}); });
  QObject::connect(&controller, &Controller::streamingChanged, &context,
                   [&](bool active) {
                     if (!active || elapsed.isValid())
                       return;
                     startup.stop();
                     elapsed.start();
                     lastReport.start();
                     duration.start(o.seconds * 1000);
                     report("streaming", {{"seconds", o.seconds}});
                   });
  QObject::connect(&controller, &Controller::telemetry, &context,
                   [&](double left, double right, double backlog, quint64 sent,
                       quint64 retried, quint64 lost) {
                     packets = sent;
                     retransmitted = retried;
                     expired = lost;
                     maxBacklog = std::max(maxBacklog, backlog);
                     peakLeft = std::max(peakLeft, left);
                     peakRight = std::max(peakRight, right);
                     if (!lastReport.isValid() || lastReport.elapsed() < 1000)
                       return;
                     lastReport.restart();
                     report("stats", {{"streaming_ms", elapsed.elapsed()},
                                      {"left", left},
                                      {"right", right},
                                      {"backlog_ms", backlog * 1000},
                                      {"packets_per_receiver", qint64(sent)},
                                      {"retransmitted", qint64(retried)},
                                      {"expired", qint64(lost)}});
                   });
  QObject::connect(&duration, &QTimer::timeout, &context, [&] {
    completed = true;
    stop();
  });
  QObject::connect(&startup, &QTimer::timeout, &context, [&] {
    result = 1;
    report("error", {{"message", "Startup/permission timeout"}});
    stop();
  });
  QObject::connect(&interruptPoll, &QTimer::timeout, &context, [&] {
    if (interrupted && !finished) {
      result = 128 + interrupted;
      report("interrupted", {{"signal", int(interrupted)}});
      interruptPoll.stop();
      stop();
    }
  });
  interruptPoll.start(50);
  QTimer::singleShot(0, &context, [&] {
    try {
      auto settings = controller.initialize();
      if (finished)
        return;
      const auto devices = controller.drivers();
      if (std::none_of(devices.begin(), devices.end(),
                       [&](const auto &d) { return d.id == o.device; }))
        throw std::runtime_error("Input device UID not found");
      settings.driverId = o.device;
      settings.left = o.left - 1;
      settings.right = o.right - 1;
      QJsonArray targets;
      for (const auto &receiver : o.receivers)
        targets.append(receiver.text());
      report("start", {{"device_uid", o.device},
                       {"left", o.left},
                       {"right", o.right},
                       {"seconds", o.seconds},
                       {"receivers", targets}});
      controller.selectDriver(o.device, nullptr);
      if (finished)
        return;
      startup.start(o.startupTimeout * 1000);
      controller.start(settings, o.receivers, false);
    } catch (const std::exception &e) {
      report("error", {{"message", QString::fromUtf8(e.what())}});
      result = 1;
      stop();
    }
  });
  application.exec();
  // Controller destruction drains capture and the network thread before exit.
  return finished ? result : 1;
}
} // namespace

int runCli(QApplication &application) {
  application.setQuitOnLastWindowClosed(false);
  QCommandLineParser parser;
  parser.setApplicationDescription(
      "AirPlayQt macOS live acceptance test. JSON lines on stdout; no GUI "
      "settings are saved.");
  parser.addHelpOption();
  parser.addOptions(
      {{"cli", "Run without the main window."},
       {"list-devices", "List capture devices (including auto loopback) and stable IDs, then exit."},
       {"device", "Capture device ID (from --list-devices).", "uid"},
       {"left", "Left input channel, 1-based.", "channel", "1"},
       {"right", "Right input channel, 1-based.", "channel", "2"},
       {"seconds", "Streaming duration, 1..3600 seconds.", "seconds", "30"},
       {"startup-timeout", "Startup/permission deadline, 1..3600 seconds.",
        "seconds", "60"},
       {"receiver", "IPv4[:port]; repeat for a stereo pair.", "endpoint"}});
  try {
    if (!parser.parse(application.arguments()))
      throw std::invalid_argument(parser.errorText().toStdString());
    if (parser.isSet("help")) {
      std::fputs(parser.helpText().toUtf8().constData(), stdout);
      return 0;
    }
    if (parser.isSet("list-devices")) {
      if (!parser.positionalArguments().isEmpty())
        throw std::invalid_argument("Unexpected positional arguments");
      for (const auto &name : parser.optionNames())
        if (name != "cli" && name != "list-devices")
          throw std::invalid_argument(
              "--list-devices cannot be combined with transmission options");
      for (const auto &device : audio::inputDevices())
        report("device", {{"uid", device.id}, {"name", device.displayName().render()}});
      return 0;
    }
    return transmit(application, options(parser));
  } catch (const std::invalid_argument &e) {
    report("error", {{"message", QString::fromUtf8(e.what())}});
    return 2;
  } catch (const std::exception &e) {
    report("error", {{"message", QString::fromUtf8(e.what())}});
    return 1;
  }
}
} // namespace app
