#include "Settings.h"
#include "airplay/ReceiverEndpoint.h"
#include "app/Message.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <cmath>
#include <stdexcept>

namespace app {
const std::array<WindowLayoutField, 5> windowLayoutFields{{
    {"width", &WindowLayout::width}, {"height", &WindowLayout::height},
    {"controlWidth", &WindowLayout::controlWidth},
    {"logWidth", &WindowLayout::logWidth},
    {"expandedWidth", &WindowLayout::expandedWidth}}};
bool WindowLayout::valid() const {
  for (const auto &field : windowLayoutFields)
    if (this->*(field.member) < 0 || this->*(field.member) > 32768)
      return false;
  return (width == 0) == (height == 0) &&
         (logVisible || (width > 0 && expandedWidth >= width));
}
const std::array<TimingMsField, 11> timingMsFields{
    {{"leadMs", i18n::text(i18n::Id::PlaybackLead), &Timing::leadMs, 0, 2000,
      false},
     {"settleMs", i18n::text(i18n::Id::StartupPTPWait), &Timing::settleMs, 0,
      30000, false},
     {"lateMs", i18n::text(i18n::Id::MaximumSendLateness), &Timing::lateMs, 10,
      1000, false},
     {"inputTimeoutMs", i18n::text(i18n::Id::InputDropoutTimeout),
      &Timing::inputTimeoutMs, 100, 30000, false},
     {"connectTimeoutMs", i18n::text(i18n::Id::TCPConnectionTimeout),
      &Timing::connectTimeoutMs, 100, 60000, false},
     {"requestTimeoutMs", i18n::text(i18n::Id::RTSPRequestTimeout),
      &Timing::requestTimeoutMs, 100, 60000, false},
     {"teardownTimeoutMs", i18n::text(i18n::Id::TEARDOWNTimeout),
      &Timing::teardownTimeoutMs, 100, 10000, false},
     {"ptpSyncMs", i18n::text(i18n::Id::PTPSyncInterval), &Timing::ptpSyncMs,
      15.625, 1000, true},
     {"ptpAnnounceMs", i18n::text(i18n::Id::PTPAnnounceInterval),
      &Timing::ptpAnnounceMs, 125, 8000, true},
     {"audioSyncMs", i18n::text(i18n::Id::AudioSyncInterval),
      &Timing::audioSyncMs, 50, 2000, false},
     {"keepAliveMs", i18n::text(i18n::Id::RTSPKeepAliveInterval),
      &Timing::keepAliveMs, 1000, 20000, false}}};
const std::array<TimingSamplesField, 3> timingSamplesFields{
    {{"packetSamples", i18n::text(i18n::Id::PacketSamples),
      &Timing::packetSamples, 1, 352},
     {"prebufferSamples", i18n::text(i18n::Id::CapturePrebuffer),
      &Timing::prebufferSamples, 1, 1 << 30},
     {"backlogSamples", i18n::text(i18n::Id::MaximumBacklog),
      &Timing::backlogSamples, 1, 1 << 30}}};
[[noreturn]] static void fail(const i18n::Message &message) {
  throw i18n::MessageError(message);
}
i18n::Message Timing::validate() const {
  for (const auto &f : timingMsFields) {
    const double value = this->*(f.member);
    if (!std::isfinite(value) || value < f.minimum || value > f.maximum ||
        (f.powerOfTwo &&
         std::exp2(std::round(std::log2(value / 1000))) * 1000 != value))
      return f.label + i18n::text(i18n::Id::IsOutOfRange);
  }
  for (const auto &f : timingSamplesFields) {
    const int value = this->*(f.member);
    if (value < f.minimum || value > f.maximum)
      return f.label + i18n::text(i18n::Id::IsOutOfRange);
  }
  if (prebufferSamples >= backlogSamples)
    return i18n::text(i18n::Id::CapturePrebufferMustBeLessThanMaximum);
  return {};
}
i18n::Message Settings::validate() const {
  if (!windowLayout.valid())
    return i18n::text(i18n::Id::InvalidWindowLayout);
  if (!i18n::valid(language))
    return i18n::text(i18n::Id::InvalidLanguage);
  if (const auto error = networkBinding.validate(); !error.isEmpty())
    return error;
  if (receiverSelection.size() > 2)
    return i18n::text(i18n::Id::AtMostTwoReceiversCanBeSaved);
  QList<airplay::ReceiverEndpoint> endpoints;
  try {
    for (const auto &receiver : receiverSelection) {
      if (receiver.name.trimmed().isEmpty())
        return i18n::text(i18n::Id::InvalidSavedReceiverName);
      const auto endpoint = airplay::parseReceiverEndpoint(receiver.endpoint);
      if (endpoint.text() != receiver.endpoint)
        return i18n::text(i18n::Id::InvalidSavedReceiverAddressFormat);
      endpoints.append(endpoint);
    }
    if (!endpoints.isEmpty())
      airplay::validateEndpoints(endpoints);
  } catch (const std::exception &e) {
    return i18n::fromException(e);
  }
  if (left < 0 || right < 0)
    return i18n::text(i18n::Id::SelectTwoValidInputChannels);
  return timing.validate();
}
QJsonObject Settings::json() const {
  QJsonObject layout{{"logVisible", windowLayout.logVisible}};
  for (const auto &field : windowLayoutFields)
    layout.insert(field.key, windowLayout.*(field.member));
  QJsonArray selection;
  for (const auto &receiver : receiverSelection)
    selection.append(
        QJsonObject{{"name", receiver.name}, {"endpoint", receiver.endpoint}});
  QJsonObject times;
  for (const auto &f : timingMsFields)
    times.insert(f.key, timing.*(f.member));
  for (const auto &f : timingSamplesFields)
    times.insert(f.key, timing.*(f.member));
  return {{"version", 4},
          {"windowLayout", layout},
          {"language", i18n::languageCode(language)},
          {"networkBinding", networkBinding.json()},
          {"driverId", driverId},
          {"left", left},
          {"right", right},
          {"timing", times},
          {"receiverSelection", selection}};
}
Settings Settings::fromJson(const QJsonObject &object) {
  if (object.value("version") != QJsonValue(4) ||
      !object.value("driverId").isString() ||
      !object.value("timing").isObject())
    fail(i18n::text(i18n::Id::InvalidConfigurationStructureOrVersion));
  Settings settings;
  if (object.contains("windowLayout")) {
    if (!object.value("windowLayout").isObject())
      fail(i18n::text(i18n::Id::InvalidWindowLayout));
    const auto layout = object.value("windowLayout").toObject();
    for (const auto &field : windowLayoutFields) {
      if (!layout.contains(field.key))
        continue;
      const auto value = layout.value(field.key);
      const double number = value.toDouble(-1);
      if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
          number > 32768 || std::floor(number) != number)
        fail(i18n::text(i18n::Id::InvalidWindowLayout));
      settings.windowLayout.*(field.member) = int(number);
    }
    if (layout.contains("logVisible")) {
      if (!layout.value("logVisible").isBool())
        fail(i18n::text(i18n::Id::InvalidWindowLayout));
      settings.windowLayout.logVisible = layout.value("logVisible").toBool();
    }
  }
  const auto language =
      i18n::parseLanguage(object.value("language").toString());
  if (!language)
    fail(i18n::text(i18n::Id::InvalidLanguage));
  settings.language = *language;
  settings.networkBinding =
      airplay::NetworkBinding::fromJson(object.value("networkBinding"));
  if (object.contains("receiverSelection")) {
    if (!object.value("receiverSelection").isArray())
      fail(i18n::text(i18n::Id::InvalidSavedReceiverList));
    for (const auto &entry : object.value("receiverSelection").toArray()) {
      if (!entry.isObject())
        fail(i18n::text(i18n::Id::InvalidSavedReceiverStructure));
      const auto receiver = entry.toObject();
      if (!receiver.value("name").isString() ||
          !receiver.value("endpoint").isString())
        fail(i18n::text(i18n::Id::InvalidSavedReceiverNameOrAddress));
      settings.receiverSelection.append(
          {receiver.value("name").toString(),
           receiver.value("endpoint").toString()});
    }
  }
  settings.driverId = object.value("driverId").toString();
  for (auto [key, target] : {std::pair{"left", &settings.left},
                             std::pair{"right", &settings.right}}) {
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 ||
        number > INT_MAX || std::floor(number) != number)
      fail(i18n::text(i18n::Id::InvalidConfigurationChannelIndex));
    *target = static_cast<int>(number);
  }
  const auto times = object.value("timing").toObject();
  for (const auto &f : timingMsFields) {
    if (!times.value(f.key).isDouble())
      fail(i18n::text(i18n::Id::MissingTimingParameter) + f.key);
    settings.timing.*(f.member) = times.value(f.key).toDouble();
  }
  for (const auto &f : timingSamplesFields) {
    const auto value = times.value(f.key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < f.minimum ||
        number > f.maximum || std::floor(number) != number)
      fail(f.label + i18n::text(i18n::Id::IsOutOfRange));
    settings.timing.*(f.member) = static_cast<int>(number);
  }
  if (const auto error = settings.validate(); !error.isEmpty())
    fail(error);
  return settings;
}
Settings Settings::load(const QString &path, bool required) {
  if (!required && !QFileInfo::exists(path))
    return {};
  try {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      fail(file.errorString());
    if (file.size() > 65536)
      fail(i18n::text(i18n::Id::ConfigurationFileIsTooLarge));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
      fail(i18n::text(i18n::Id::CorruptConfigurationJSON) +
           error.errorString());
    return fromJson(document.object());
  } catch (const std::exception &error) {
    fail(i18n::fromException(error) +
         i18n::text(i18n::Id::ConfigPreservedPrefix) + path +
         i18n::text(i18n::Id::AndRestart));
  }
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
#ifdef Q_OS_LINUX
  const auto directory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  if (directory.isEmpty() || !QDir().mkpath(directory))
    fail(i18n::text(i18n::Id::CannotCreateTheUserConfigurationDirectory));
  return directory + "/AirPlayQt.json";
#elif defined(Q_OS_MACOS)
  const auto directory = QDir::home().filePath(".config");
  if (!QDir().mkpath(directory))
    fail(i18n::text(i18n::Id::CannotCreateTheUserConfigurationDirectory));
  return directory + "/AirPlayQt.json";
#else
  return QCoreApplication::applicationDirPath() + "/AirPlayQt.json";
#endif
}
Settings SettingsStore::load() {
  writable_ = false;
  value_ = Settings::load(path());
  writable_ = true;
  return value_;
}
void SettingsStore::commit(const Settings &value) {
  if (!writable_)
    fail(i18n::text(i18n::Id::OriginalConfigurationIsInvalidAndCannotBe) +
         path() + i18n::text(i18n::Id::AndRestartTheApplication));
  value.save(path());
  value_ = value;
}
void SettingsStore::saveLanguage(i18n::Language language) {
  auto updated = value_;
  updated.language = language;
  commit(updated);
}
void SettingsStore::saveWindowLayout(const WindowLayout &layout) {
  auto updated = value_;
  updated.windowLayout = layout;
  commit(updated);
}
void SettingsStore::saveStart(
    Settings value, const std::optional<QList<ReceiverSelection>> &selection) {
  value.windowLayout = value_.windowLayout;
  value.receiverSelection = selection ? *selection : value_.receiverSelection;
  commit(value);
}
} // namespace app
