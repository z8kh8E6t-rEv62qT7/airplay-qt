#include "Controller.h"
#include "audio/PipeWireCatalog.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <utility>

namespace {
// Keep device names and system errors on one terminal line.
QString singleLine(QString text) {
  for (auto &character : text)
    if (character.category() == QChar::Other_Control)
      character = QChar(' ');
  return text;
}
void output(FILE *stream, const QString &text) {
  const auto bytes = text.toUtf8();
  std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
  std::fflush(stream);
}
void log(const QString &text) {
  output(stderr, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) +
                     " " + singleLine(text) + '\n');
}

enum class Mode { Send, Devices, Interfaces, Help, Version };
struct Options {
  Mode mode = Mode::Send;
  app::Settings settings;
  QList<airplay::ReceiverEndpoint> receivers;
};

Options options(QCommandLineParser &parser, const QStringList &arguments) {
  parser.setApplicationDescription(
      "Send a Linux audio input to manually specified AirPlay receivers. "
      "The first startup failure exits; after streaming once, sessions retry "
      "every 2 seconds until SIGINT or SIGTERM. Only timing is read from "
      "--config.");
  parser.addOptions(
      {{"help", "Show this help and exit."},
       {"version", "Show the application version and exit."},
       {"list-devices",
        "Query input devices once, print stable IDs, and exit."},
       {"list-interfaces",
        "List available network interfaces and IPv4 addresses."},
       {"config",
        "Required version 4 application configuration; only timing is used.",
        "path"},
       {"device", "Required input device ID from --list-devices.", "id"},
       {"receiver",
        "Required IPv4[:port], default port 7000; repeat for a stereo pair.",
        "endpoint"},
       {"interface", "Sending interface name; requires --local-ip.", "name"},
       {"local-ip", "Sending interface IPv4; requires --interface.", "ipv4"},
       {"left", "Left input channel (1 or 2).", "channel", "1"},
       {"right", "Right input channel (1 or 2).", "channel", "2"}});
  if (!parser.parse(arguments))
    throw std::invalid_argument(parser.errorText().toStdString());
  if (!parser.positionalArguments().isEmpty())
    throw std::invalid_argument("Unexpected positional arguments");
  const auto names = parser.optionNames();
  for (const auto &name : names)
    if (name != "receiver" && names.count(name) > 1)
      throw std::invalid_argument(("Repeated option: --" + name).toStdString());

  Options result;
  for (const auto &[name, mode] : {
           std::pair{"help", Mode::Help}, {"version", Mode::Version},
           {"list-devices", Mode::Devices}, {"list-interfaces", Mode::Interfaces}}) {
    if (!parser.isSet(name)) continue;
    if (names.size() != 1)
      throw std::invalid_argument("Query options cannot be combined with other options");
    result.mode = mode;
    return result;
  }

  if (!parser.isSet("config") || parser.value("config").isEmpty())
    throw std::invalid_argument("--config PATH is required");
  result.settings.timing =
      app::Settings::load(parser.value("config"), true).timing;
  result.settings.driverId = parser.value("device");
  if (result.settings.driverId.trimmed().isEmpty())
    throw std::invalid_argument("--device is required; use --list-devices");
  const auto channel = [&](const char *name) {
    const auto value = parser.value(name);
    if (value != "1" && value != "2")
      throw std::invalid_argument(QString("--%1 must be 1 or 2").arg(name).toStdString());
    return value == "1" ? 0 : 1;
  };
  result.settings.left = channel("left");
  result.settings.right = channel("right");
  if (parser.isSet("interface") != parser.isSet("local-ip"))
    throw std::invalid_argument("--interface and --local-ip must be supplied together");
  if (parser.isSet("interface")) {
    result.settings.networkBinding = {parser.value("interface"), parser.value("local-ip")};
    if (result.settings.networkBinding.interfaceName.isEmpty() ||
        result.settings.networkBinding.ipv4.isEmpty())
      throw std::invalid_argument("--interface and --local-ip must not be empty");
  }
  if (!parser.isSet("receiver"))
    throw std::invalid_argument("At least one --receiver IPv4[:port] is required");
  try {
    for (const auto &value : parser.values("receiver"))
      result.receivers.append(airplay::parseReceiverEndpoint(value));
    airplay::validateEndpoints(result.receivers);
    if (const auto error = result.settings.validate(); !error.isEmpty())
      throw i18n::MessageError(error);
  } catch (const std::exception &error) {
    throw std::invalid_argument(error.what());
  }
  return result;
}

void listDevices(const QList<audio::DriverInfo> &devices) {
  output(stdout, "ID\tNAME\tCONNECTION\n");
  for (const auto &device : devices) {
    const auto state = device.connected
        ? (*device.connected ? "connected" : "disconnected") : "n/a";
    output(stdout, singleLine(device.id) + '\t' + singleLine(device.name) +
                       '\t' + state + '\n');
  }
}
void listInterfaces() {
  output(stdout, "INTERFACE\tIPV4\n");
  for (const auto &binding : airplay::NetworkBinding::available())
    output(stdout, singleLine(binding.interfaceName) + '\t' + binding.ipv4 + '\n');
}

static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> interrupted{0};
// A process signal can be delivered on the network or PipeWire thread.
// Only a lock-free store runs in the handler; Qt performs the cleanup.
void interrupt(int signal) { interrupted.store(signal, std::memory_order_relaxed); }

class Interrupts {
public:
  Interrupts() {
    struct sigaction action{};
    action.sa_handler = interrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &action, &previousInt_) != 0)
      throw std::runtime_error(std::strerror(errno));
    if (sigaction(SIGTERM, &action, &previousTerm_) != 0) {
      const auto error = errno;
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

class CliRunner : public QObject {
public:
  CliRunner(QCoreApplication &application, const Options &configuration)
      : application_(application), configuration_(configuration) {
    queryTimeout_.setSingleShot(true);
    queryTimeout_.setTimerType(Qt::PreciseTimer);
    retry_.setSingleShot(true);
    retry_.setTimerType(Qt::PreciseTimer);
    connect(&queryTimeout_, &QTimer::timeout, this, [this] {
      log("Error: Initial input device query timed out after 5 seconds");
      stop(1);
    });
    connect(&retry_, &QTimer::timeout, this, &CliRunner::attempt);
    connect(&interruptPoll_, &QTimer::timeout, this, [this] { checkInterrupt(); });
    interruptPoll_.start(50);
    QTimer::singleShot(0, this, &CliRunner::begin);
  }
  ~CliRunner() override {
    // Controller destruction can emit capture diagnostics. The runner's
    // members must no longer receive callbacks during member destruction.
    if (controller_) controller_->disconnect(this);
  }

private:
  enum class Phase { Querying, Starting, Running, Waiting, Stopping, Finished };

  bool checkInterrupt() {
    const auto signal = interrupted.load(std::memory_order_relaxed);
    if (signal) {
      if (phase_ != Phase::Stopping && phase_ != Phase::Finished) {
        log(QString("Signal %1 received; stopping").arg(signal));
        stop(128 + signal);
      }
      return true;
    }
    return phase_ == Phase::Stopping || phase_ == Phase::Finished;
  }

  void begin() {
    if (checkInterrupt()) return;
    queryTimeout_.start(5000);
    try {
      auto &catalog = audio::PipeWireCatalog::instance();
      connect(&catalog, &audio::PipeWireCatalog::initialQueryFinished,
              this, &CliRunner::queried);
      if (catalog.initialQueryResult()) queried(*catalog.initialQueryResult());
    } catch (const std::exception &error) {
      log("Error: " + QString::fromUtf8(error.what()));
      stop(1);
    }
  }

  void queried(const QString &error) {
    if (checkInterrupt() || phase_ != Phase::Querying) return;
    queryTimeout_.stop();
    if (!error.isEmpty()) {
      log("Error: " + error);
      stop(1);
      return;
    }
    try {
      const auto devices = audio::inputDevices();
      if (configuration_.mode == Mode::Devices) {
        listDevices(devices);
        stop(0);
        return;
      }
      const auto &id = configuration_.settings.driverId;
      if (std::none_of(devices.begin(), devices.end(),
                       [&](const auto &device) { return device.id == id; })) {
        log("Error: Input device not found: " + id);
        stop(1);
        return;
      }
      controller_ = std::make_unique<app::Controller>();
      controller_->session().setTelemetryEnabled(false);
      const auto report = [](const QJsonArray &message) {
        const auto text = i18n::Message(message).render();
        if (!text.isEmpty()) log(text);
      };
      connect(controller_.get(), &app::Controller::status, this, report);
      connect(controller_.get(), &app::Controller::log, this, report);
      connect(controller_.get(), &app::Controller::group, this, report);
      connect(controller_.get(), &app::Controller::error, this,
              [this](const QJsonArray &message) {
        lastError_ = i18n::Message(message).render();
        log("Error: " + lastError_);
        // Asynchronous session errors are followed by stopped(). Synchronous
        // select/start errors are inspected after those calls return.
      });
      connect(controller_.get(), &app::Controller::streamingChanged, this,
              [this](bool active) {
        if (active && phase_ == Phase::Starting) {
          everStreamed_ = true;
          phase_ = Phase::Running;
          log("Streaming; session recovery is now enabled");
        }
      });
      connect(controller_.get(), &app::Controller::stopped,
              this, &CliRunner::attemptEnded);
      // Deliberately do not call initialize(): it loads GUI settings.
      controller_->selectDriver(id, nullptr);
      if (!lastError_.isEmpty()) { stop(1); return; }
      attempt();
    } catch (const std::exception &exception) {
      log("Error: " + QString::fromUtf8(exception.what()));
      stop(1);
    }
  }

  void attempt() {
    if (checkInterrupt() || controller_->busy() ||
        (phase_ != Phase::Querying && phase_ != Phase::Waiting)) return;
    phase_ = Phase::Starting;
    lastError_.clear();
    log(everStreamed_ ? "Reconnecting AirPlay session" : "Starting AirPlay session");
    controller_->start(configuration_.settings, configuration_.receivers, false);
    // Preparation/route failures do not create a session or emit stopped().
    if (!controller_->busy()) attemptEnded();
  }

  void attemptEnded() {
    if (phase_ == Phase::Stopping) { finish(); return; }
    if (checkInterrupt() || phase_ == Phase::Waiting || phase_ == Phase::Finished)
      return;
    if (!everStreamed_) {
      log("Initial AirPlay startup failed; exiting without retry");
      stop(1);
      return;
    }
    // SessionController emits stopped only after capture and protocol cleanup.
    // Setting the phase before arming the timer makes duplicate notifications
    // harmless, including errors followed by stopped in the same event turn.
    phase_ = Phase::Waiting;
    log("AirPlay session ended; retrying in 2 seconds");
    retry_.start(2000);
  }

  void stop(int result) {
    if (phase_ == Phase::Stopping || phase_ == Phase::Finished) return;
    phase_ = Phase::Stopping;
    result_ = result;
    queryTimeout_.stop();
    retry_.stop();
    if (controller_) controller_->stop();
    if (!controller_ || !controller_->busy()) finish();
  }

  void finish() {
    if (phase_ == Phase::Finished) return;
    // Join the network thread before the event loop and capture owner go away.
    if (controller_) controller_->session().shutdown();
    phase_ = Phase::Finished;
    interruptPoll_.stop();
    application_.exit(result_);
  }

  QCoreApplication &application_;
  const Options &configuration_;
  QTimer queryTimeout_, retry_, interruptPoll_;
  Phase phase_ = Phase::Querying;
  bool everStreamed_ = false;
  int result_ = 1;
  QString lastError_;
  std::unique_ptr<app::Controller> controller_;
};

int runInput(QCoreApplication &application, const Options &configuration) {
  Interrupts interrupts;
  CliRunner runner(application, configuration);
  return application.exec();
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName("AirPlayQtCli");
  QCoreApplication::setApplicationVersion(AIRPLAY_VERSION);
  try {
    QCommandLineParser parser;
    const auto configuration = options(parser, application.arguments());
    switch (configuration.mode) {
    case Mode::Help:
      output(stdout, parser.helpText());
      return 0;
    case Mode::Version:
      output(stdout, "AirPlayQtCli " + application.applicationVersion() + '\n');
      return 0;
    case Mode::Interfaces:
      listInterfaces();
      return 0;
    case Mode::Devices:
    case Mode::Send:
      return runInput(application, configuration);
    }
  } catch (const std::invalid_argument &error) {
    log("Error: " + QString::fromUtf8(error.what()));
    return 2;
  } catch (const std::exception &error) {
    log("Error: " + QString::fromUtf8(error.what()));
    return 1;
  }
  return 1;
}
