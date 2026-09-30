#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
#include <array>

namespace app {
struct Timing {
  double lead = 2, settle = 2, prebuffer = .04, backlog = .1, late = .1;
  double inputTimeout = 1, connectTimeout = 8, requestTimeout = 8,
         teardownTimeout = 1;
  double ptpSync = .125, ptpAnnounce = 1, audioSync = .5, keepAlive = 10;
  QString validate() const;
};
struct TimingField {
  const char *key;
  const char *label;
  double Timing::*member;
  double minimum, maximum;
  bool powerOfTwo;
};
extern const std::array<TimingField, 13> timingFields;
struct ReceiverSelection {
  QString name, endpoint;
  bool operator==(const ReceiverSelection &) const = default;
};
struct Settings {
  QString driverId;
  int left = 0, right = 1; // SDK indices, GUI displays index + 1.
  Timing timing;
  QList<ReceiverSelection> receiverSelection;
  QString validate() const;
  QJsonObject json() const;
  static Settings fromJson(const QJsonObject &object);
  static Settings load(const QString &path);
  void save(const QString &path) const;
  static QString path();
};
// The standalone controller owns this store. Saving input settings must never
// erase the separately remembered discovery selection (including manual mode).
class SettingsStore {
public:
  explicit SettingsStore(QString path = {}) : path_(std::move(path)) {}
  Settings load();
  void rememberReceivers(const QList<ReceiverSelection> &);
  void saveInput(Settings);
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
