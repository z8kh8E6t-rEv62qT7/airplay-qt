#pragma once
#include <QJsonObject>
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
struct Settings {
  QString driverId;
  int left = 0, right = 1; // SDK indices, GUI displays index + 1.
  Timing timing;
  QString validate() const;
  QJsonObject json() const;
  static Settings fromJson(const QJsonObject &object);
  static Settings load(const QString &path);
  void save(const QString &path) const;
  static QString path();
};
} // namespace app
