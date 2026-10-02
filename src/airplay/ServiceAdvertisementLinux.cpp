#include "ServiceAdvertisement.h"
#include "AvahiObject.h"
#include <QDBusMetaType>
#include <QTimer>

namespace airplay {
struct ServiceAdvertisement::State {
  explicit State(const DiscoveryApi &api) : group(api, "org.freedesktop.Avahi.EntryGroup") {}
  AvahiObject group;
};
ServiceAdvertisement::ServiceAdvertisement(QObject *parent, const DiscoveryApi &api)
    : QObject(parent), api_(api) {}
ServiceAdvertisement::~ServiceAdvertisement() = default;
void ServiceAdvertisement::stop() { ++generation_; state_.reset(); }
void ServiceAdvertisement::start(const QString &identity, quint16 port, const NetworkRoute &route) {
  stop();
  route.validate();
  qDBusRegisterMetaType<QList<QByteArray>>();
  state_ = std::make_unique<State>(api_);
  auto *group = &state_->group;
  const auto generation = generation_;
  const auto report = [this, generation](const QString &error) {
    if (generation == generation_)
      emit failed(i18n::text(i18n::Id::LinuxPublicationFailed).arg(error));
  };
  connect(group, &AvahiObject::failed, this, report, Qt::QueuedConnection);
  connect(group, &AvahiObject::event, this,
      [this, generation, report](const QString &member, const QVariantList &args) {
    if (generation != generation_ || member != "StateChanged" || args.size() != 2) return;
    if (args[0].toInt() == 2) emit ready();
    else if (args[0].toInt() == 3 || args[0].toInt() == 4) report(args[1].toString());
  }, Qt::QueuedConnection);
  connect(group, &AvahiObject::created, group, [group, identity, port, route] {
    const auto host = "airplayqt-" + identity.toLower() + ".local";
    const int interface = route.index ? int(route.index) : -1;
    // AVAHI_PUBLISH_NO_REVERSE keeps this private IPv4 host out of reverse DNS.
    group->call("AddAddress", {interface, 0, uint(16), host, route.local.toString()},
      [group, identity, port, host, interface] {
        const QList<QByteArray> txt{"txtvers=1", "Ver=131077", "DbId=" + identity.toLatin1(), "OSsi=0x1F5"};
        group->call("AddService", {interface, 0, uint(0), "iTunes_Ctrl_" + identity,
            "_dacp._tcp", "local", host, QVariant::fromValue(port), QVariant::fromValue(txt)},
            [group] { group->call("Commit"); });
      });
  });
  group->create("EntryGroupNew", {});
}
} // namespace airplay
