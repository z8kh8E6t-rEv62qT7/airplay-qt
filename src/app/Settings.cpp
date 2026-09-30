#include "Settings.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <stdexcept>

namespace app {
const std::array<TimingField, 13> timingFields{
    {{"lead", "播放提前量", &Timing::lead, 0, 2, false},
     {"settle", "启动 PTP 等待", &Timing::settle, 0, 30, false},
     {"prebuffer", "采集预缓冲", &Timing::prebuffer, 0, .5, false},
     {"backlog", "最大积压", &Timing::backlog, .01, 1, false},
     {"late", "最大发送落后", &Timing::late, .01, 1, false},
     {"inputTimeout", "采集断流超时", &Timing::inputTimeout, .1, 30, false},
     {"connectTimeout", "TCP 连接超时", &Timing::connectTimeout, .1, 60, false},
     {"requestTimeout", "RTSP 请求超时", &Timing::requestTimeout, .1, 60,
      false},
     {"teardownTimeout", "TEARDOWN 等待上限", &Timing::teardownTimeout, .1, 10,
      false},
     {"ptpSync", "PTP Sync 周期", &Timing::ptpSync, 1. / 64, 1, true},
     {"ptpAnnounce", "PTP Announce 周期", &Timing::ptpAnnounce, 1. / 8, 8,
      true},
     {"audioSync", "音频同步包周期", &Timing::audioSync, .05, 2, false},
     {"keepAlive", "RTSP 保活周期", &Timing::keepAlive, 1, 20, false}}};
static void fail(const QString &message) {
  throw std::runtime_error(message.toStdString());
}
QString Timing::validate() const {
  for (const auto &f : timingFields) {
    const double value = this->*(f.member);
    if (!std::isfinite(value) || value < f.minimum || value > f.maximum ||
        (f.powerOfTwo && std::exp2(std::round(std::log2(value))) != value))
      return QString::fromUtf8(f.label) + "超出允许范围";
  }
  if (prebuffer >= backlog)
    return "采集预缓冲必须小于最大积压";
  return {};
}
QString Settings::validate() const {
  if (left < 0 || right < 0 || left == right)
    return "请选择两个不同的有效输入通道";
  return timing.validate();
}
QJsonObject Settings::json() const {
  QJsonObject times;
  for (const auto &f : timingFields)
    times.insert(f.key, timing.*(f.member));
  return {{"version", 1},
          {"driverId", driverId},
          {"left", left},
          {"right", right},
          {"timing", times}};
}
Settings Settings::fromJson(const QJsonObject &object) {
  if (object.value("version") != QJsonValue(1) ||
      !object.value("driverId").isString() ||
      !object.value("timing").isObject())
    fail("配置结构或版本无效");
  Settings settings;
  settings.driverId = object.value("driverId").toString();
  for (auto [key, target] : {std::pair{"left", &settings.left},
                             std::pair{"right", &settings.right}}) {
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
        number > INT_MAX || std::floor(number) != number)
      fail("配置声道索引无效");
    *target = static_cast<int>(number);
  }
  const auto times = object.value("timing").toObject();
  for (const auto &f : timingFields) {
    if (!times.value(f.key).isDouble())
      fail(QString("缺少时间参数：") + f.key);
    settings.timing.*(f.member) = times.value(f.key).toDouble();
  }
  if (const auto error = settings.validate(); !error.isEmpty())
    fail(error);
  return settings;
}
Settings Settings::load(const QString &path) {
  if (!QFileInfo::exists(path))
    return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    fail(file.errorString());
  if (file.size() > 65536)
    fail("配置文件过大");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    fail("配置 JSON 损坏：" + error.errorString());
  return fromJson(document.object());
}
void Settings::save(const QString &path) const {
  if (const auto error = validate(); !error.isEmpty())
    fail(error);
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly))
    fail(file.errorString());
  const auto data = QJsonDocument(json()).toJson();
  if (file.write(data) != data.size() || !file.commit())
    fail(file.errorString());
}
QString Settings::path() {
  return QCoreApplication::applicationDirPath() + "/AirPlayQt.json";
}
} // namespace app
