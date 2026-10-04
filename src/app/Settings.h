#pragma once
#include "airplay/NetworkBinding.h"
#include "app/Message.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <array>
#include <optional>

namespace app {
struct WindowLayout {
  int width = 0, height = 0, controlWidth = 0, logWidth = 0, expandedWidth = 0;
  bool logVisible = true;
  bool operator==(const WindowLayout &) const = default;
  bool valid() const;
};
struct WindowLayoutField {
  const char *key;
  int WindowLayout::*member;
};
extern const std::array<WindowLayoutField, 5> windowLayoutFields;
struct Timing {
  double leadMs = 2000, settleMs = 2000, lateMs = 100;
  double inputTimeoutMs = 1000, connectTimeoutMs = 8000,
         requestTimeoutMs = 8000, teardownTimeoutMs = 1000;
  double ptpSyncMs = 125, ptpAnnounceMs = 1000, audioSyncMs = 500,
         keepAliveMs = 10000;
  int packetSamples = 64, prebufferSamples = 2048, backlogSamples = 8192;
  bool operator==(const Timing &) const = default;
  i18n::Message validate() const;
};
struct TimingMsField {
  const char *key;
  i18n::Message label;
  double Timing::*member;
  double minimum, maximum;
  bool powerOfTwo;
};
struct TimingSamplesField {
  const char *key;
  i18n::Message label;
  int Timing::*member;
  int minimum, maximum;
};
extern const std::array<TimingMsField, 11> timingMsFields;
extern const std::array<TimingSamplesField, 3> timingSamplesFields;
struct ReceiverSelection {
  QString name, endpoint;
  bool operator==(const ReceiverSelection &) const = default;
};
struct Settings {
  WindowLayout windowLayout;
  i18n::Language language = i18n::Language::English;
  QString driverId;
  int left = 0, right = 1; // SDK indices, GUI displays index + 1.
  Timing timing;
  airplay::NetworkBinding networkBinding;
  QList<ReceiverSelection> receiverSelection;
  i18n::Message validate() const;
  QJsonObject json() const;
  static Settings fromJson(const QJsonObject &object);
  static Settings load(const QString &path, bool required = false);
  void save(const QString &path) const;
  static QString path();
};
// The standalone controller owns this store. Saving input settings must never
// erase the separately remembered discovery selection (including manual mode).
class SettingsStore {
public:
  explicit SettingsStore(QString path = {}) : path_(std::move(path)) {}
  Settings load();
  void saveLanguage(i18n::Language);
  void saveWindowLayout(const WindowLayout &);
  void saveStart(
      Settings,
      const std::optional<QList<ReceiverSelection>> &selection = std::nullopt);
  const QList<ReceiverSelection> &receivers() const {
    return value_.receiverSelection;
  }
  bool writable() const { return writable_; }

private:
  QString path() const { return path_.isEmpty() ? Settings::path() : path_; }
  void commit(const Settings &);
  QString path_;
  Settings value_;
  bool writable_ = false; // Never overwrite a configuration that was not read.
};
} // namespace app
