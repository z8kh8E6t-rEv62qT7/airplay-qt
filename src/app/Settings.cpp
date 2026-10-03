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
const std::array<TimingField, 13> timingFields{
    {{"lead", i18n::text(i18n::Id::PlaybackLead), &Timing::lead, 0, 2, false},
     {"settle", i18n::text(i18n::Id::StartupPTPWait), &Timing::settle, 0, 30,
      false},
     {"prebuffer", i18n::text(i18n::Id::CapturePrebuffer), &Timing::prebuffer,
      0, .5, false},
     {"backlog", i18n::text(i18n::Id::MaximumBacklog), &Timing::backlog, .01, 1,
      false},
     {"late", i18n::text(i18n::Id::MaximumSendLateness), &Timing::late, .01, 1,
      false},
     {"inputTimeout", i18n::text(i18n::Id::InputDropoutTimeout),
      &Timing::inputTimeout, .1, 30, false},
     {"connectTimeout", i18n::text(i18n::Id::TCPConnectionTimeout),
      &Timing::connectTimeout, .1, 60, false},
     {"requestTimeout", i18n::text(i18n::Id::RTSPRequestTimeout),
      &Timing::requestTimeout, .1, 60, false},
     {"teardownTimeout", i18n::text(i18n::Id::TEARDOWNTimeout),
      &Timing::teardownTimeout, .1, 10, false},
     {"ptpSync", i18n::text(i18n::Id::PTPSyncInterval), &Timing::ptpSync,
      1. / 64, 1, true},
     {"ptpAnnounce", i18n::text(i18n::Id::PTPAnnounceInterval),
      &Timing::ptpAnnounce, 1. / 8, 8, true},
     {"audioSync", i18n::text(i18n::Id::AudioSyncInterval), &Timing::audioSync,
      .05, 2, false},
     {"keepAlive", i18n::text(i18n::Id::RTSPKeepAliveInterval),
      &Timing::keepAlive, 1, 20, false}}};
static void fail(const i18n::Message &message) {
  throw i18n::MessageError(message);
}
i18n::Message Timing::validate() const {
  for (const auto &f : timingFields) {
    const double value = this->*(f.member);
    if (!std::isfinite(value) || value < f.minimum || value > f.maximum ||
        (f.powerOfTwo && std::exp2(std::round(std::log2(value))) != value))
      return f.label + i18n::text(i18n::Id::IsOutOfRange);
  }
  if (prebuffer >= backlog)
    return i18n::text(i18n::Id::CapturePrebufferMustBeLessThanMaximum);
  return {};
}
i18n::Message Settings::validate() const {
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
  QJsonArray selection;
  for (const auto &receiver : receiverSelection)
    selection.append(
        QJsonObject{{"name", receiver.name}, {"endpoint", receiver.endpoint}});
  QJsonObject times;
  for (const auto &f : timingFields)
    times.insert(f.key, timing.*(f.member));
  return {{"version", 3},
          {"language", i18n::languageCode(language)},
          {"networkBinding", networkBinding.json()},
          {"driverId", driverId},
          {"left", left},
          {"right", right},
          {"timing", times},
          {"receiverSelection", selection}};
}
Settings Settings::fromJson(const QJsonObject &object) {
  if (object.value("version") != QJsonValue(3) ||
      !object.value("driverId").isString() ||
      !object.value("timing").isObject())
    fail(i18n::text(i18n::Id::InvalidConfigurationStructureOrVersion));
  Settings settings;
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
  for (const auto &f : timingFields) {
    if (!times.value(f.key).isDouble())
      fail(i18n::text(i18n::Id::MissingTimingParameter) + f.key);
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
    fail(i18n::text(i18n::Id::ConfigurationFileIsTooLarge));
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    fail(i18n::text(i18n::Id::CorruptConfigurationJSON) + error.errorString());
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
void SettingsStore::saveStart(
    Settings value, const std::optional<QList<ReceiverSelection>> &selection) {
  value.receiverSelection = selection ? *selection : value_.receiverSelection;
  commit(value);
}
} // namespace app
