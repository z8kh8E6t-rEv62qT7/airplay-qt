#include "BluetoothVolume.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSocketNotifier>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace audio {
QString bluetoothInputSource(const QString &name, const QString &phys, const QString &uniq,
                             const QList<BluetoothSource> &sources) {
  if (!name.endsWith(" (AVRCP)")) return {};
  QString result;
  for (const auto &source : sources) {
    if (!source.connected || !source.id.startsWith("bluez:")) continue;
    const auto adapter = source.id.mid(6).section('/', 0, 0);
    const auto address = source.id.section('/', 1, 1);
    if (phys.section('/', 0, 0).compare(adapter, Qt::CaseInsensitive) != 0) continue;
    if (!uniq.isEmpty()) {
      if (uniq.compare(address, Qt::CaseInsensitive) != 0) continue;
    } else {
      // BlueZ truncates the UTF-8 name to leave room for the AVRCP suffix.
      const auto expected = QString::fromUtf8(source.inputName.toUtf8().left(79 - 8)) + " (AVRCP)";
      if (name != expected && name.compare(address + " (AVRCP)", Qt::CaseInsensitive) != 0) continue;
    }
    if (!result.isEmpty()) return {}; // Duplicate names cannot safely identify a phone.
    result = source.id;
  }
  return result;
}

struct BluetoothVolume::Input {
  int fd = -1;
  bool dropped = false;
  QString source;
  std::unique_ptr<QSocketNotifier> notifier;
  ~Input() { notifier.reset(); if (fd >= 0) ::close(fd); }
};

BluetoothVolume::BluetoothVolume(BluezCatalog &catalog, QObject *parent, QString directory)
    : QObject(parent), catalog_(catalog), inputDirectory_(std::move(directory)) {
  scan_.setInterval(1000);
  steps_.setSingleShot(true);
  steps_.setTimerType(Qt::PreciseTimer);
  steps_.setInterval(100);
  connect(&scan_, &QTimer::timeout, this, &BluetoothVolume::scan);
  connect(&steps_, &QTimer::timeout, this, [this] {
    const auto pending = std::exchange(pending_, {});
    for (const int step : pending) {
      if (active_.isEmpty()) break;
      emit stepRequested(step);
    }
  });
  connect(&catalog_, &BluezCatalog::changed, this, &BluetoothVolume::reconcile);
  connect(&catalog_, &BluezCatalog::volumeReset, this, [this](const QString &id) {
    if (!id.isEmpty() && id == active_) reset();
  });
  connect(&catalog_, &BluezCatalog::volumeRequested, this, [this](const QString &id, double db) {
    if (id != active_ || active_.isEmpty()) return;
    steps_.stop();
    pending_.clear();
    absolute_.start();
    emit absoluteRequested(db);
  });
}
BluetoothVolume::~BluetoothVolume() = default;
void BluetoothVolume::reset() {
  steps_.stop();
  pending_.clear();
  absolute_.invalidate();
  inputs_.clear();
}
void BluetoothVolume::setSource(const QString &id) {
  if (selected_ == id) return;
  reset();
  selected_ = id;
  active_.clear();
  reported_.clear();
  reconcile();
}
void BluetoothVolume::reconcile() {
  const auto &sources = catalog_.sources();
  const bool connected = std::any_of(sources.begin(), sources.end(), [this](const auto &source) {
    return source.connected && source.id == selected_;
  });
  const QString next = connected ? selected_ : QString{};
  // Revalidate descriptors whenever BlueZ identity/name/topology changes.
  reset();
  active_ = next;
  if (active_.isEmpty()) scan_.stop();
  else { scan_.start(); scan(); }
}
void BluetoothVolume::keyEvent(const QString &sourceId, int code, int value) {
  if (active_.isEmpty() || sourceId != active_ || (value != 1 && value != 2) ||
      (code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN)) return;
  // Also suppress the key when BlueZ delivered the absolute event first.
  if (absolute_.isValid() && absolute_.elapsed() <= 100) return;
  pending_.append(code == KEY_VOLUMEUP ? 1 : -1);
  if (!steps_.isActive()) steps_.start();
}
void BluetoothVolume::report(const QString &text) {
  if (reported_.contains(text)) return;
  reported_.insert(text);
  emit log(text);
}
void BluetoothVolume::scan() {
  if (active_.isEmpty() || inputDirectory_.isEmpty()) return;
  const QDir directory(inputDirectory_);
  const auto files = directory.entryList({"event*"}, QDir::System | QDir::Files);
  for (auto it = inputs_.begin(); it != inputs_.end();) {
    if (!files.contains(it->first)) {
      steps_.stop(); pending_.clear();
      it = inputs_.erase(it);
    } else ++it;
  }
  for (const auto &file : files) {
    if (inputs_.contains(file)) continue;
    // sysfs is readable even when evdev is not; never open unrelated keyboards.
    const auto sysfs = "/sys/class/input/" + file + "/device/";
    const auto property = [&](const QString &key) {
      QFile value(sysfs + key);
      return value.open(QIODevice::ReadOnly) ? QString::fromUtf8(value.readAll()).trimmed() : QString{};
    };
    if (property("id/bustype").toUInt(nullptr, 16) != BUS_BLUETOOTH) continue;
    const auto name = property("name"), phys = property("phys"), uniq = property("uniq");
    if (!name.endsWith(" (AVRCP)")) continue;
    const auto source = bluetoothInputSource(name, phys, uniq, catalog_.inputSources());
    if (source != active_) {
      if (source.isEmpty() && phys.compare(active_.mid(6).section('/', 0, 0), Qt::CaseInsensitive) == 0)
        report("Cannot uniquely identify AVRCP input: " + name);
      continue;
    }
    const auto path = directory.filePath(file);
    auto input = std::make_unique<Input>();
    input->fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (input->fd < 0) {
      report("Cannot read AVRCP volume keys: " + path + ": " + QString::fromLocal8Bit(std::strerror(errno)) +
             "; see the Linux guide for the AVRCP uaccess rule");
      continue;
    }
    // Verify the opened device too, in case an event number was reused.
    input_id identity{};
    char actualName[256]{}, actualPhys[256]{}, actualUniq[256]{};
    if (ioctl(input->fd, EVIOCGID, &identity) < 0 || identity.bustype != BUS_BLUETOOTH ||
        ioctl(input->fd, EVIOCGNAME(sizeof(actualName)), actualName) < 0 ||
        ioctl(input->fd, EVIOCGPHYS(sizeof(actualPhys)), actualPhys) < 0) continue;
    ioctl(input->fd, EVIOCGUNIQ(sizeof(actualUniq)), actualUniq);
    if (bluetoothInputSource(QString::fromUtf8(actualName), QString::fromUtf8(actualPhys),
                             QString::fromUtf8(actualUniq), catalog_.inputSources()) != active_) continue;
    input->source = source;
    input->notifier = std::make_unique<QSocketNotifier>(input->fd, QSocketNotifier::Read, this);
    connect(input->notifier.get(), &QSocketNotifier::activated, this,
            [this, ptr = input.get()] { read(*ptr); });
    inputs_[file] = std::move(input);
  }
}
void BluetoothVolume::read(Input &input) {
  input_event events[64];
  const auto bytes = ::read(input.fd, events, sizeof(events));
  if (bytes < 0 && (errno == EAGAIN || errno == EINTR)) return;
  if (bytes <= 0 || bytes % sizeof(input_event)) {
    steps_.stop(); pending_.clear();
    // Delete after the notifier's signal finishes; a later scan can reopen it.
    input.notifier->setEnabled(false);
    const QPointer<QSocketNotifier> notifier(input.notifier.get());
    QTimer::singleShot(0, this, [this, notifier] {
      if (notifier)
        std::erase_if(inputs_, [notifier](const auto &entry) { return entry.second->notifier.get() == notifier; });
    });
    return;
  }
  for (size_t i = 0; i < size_t(bytes) / sizeof(input_event); ++i) {
    const auto &event = events[i];
    if (event.type == EV_SYN && event.code == SYN_DROPPED) {
      input.dropped = true;
      steps_.stop(); pending_.clear();
    } else if (input.dropped) {
      if (event.type == EV_SYN && event.code == SYN_REPORT) input.dropped = false;
    } else if (event.type == EV_KEY) keyEvent(input.source, event.code, event.value);
  }
}
} // namespace audio
