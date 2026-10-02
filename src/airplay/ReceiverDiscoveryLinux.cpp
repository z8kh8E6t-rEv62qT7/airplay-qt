#include "ReceiverDiscovery.h"
#include "AvahiObject.h"
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace airplay {
struct ReceiverDiscovery::State {
  ReceiverDiscovery &owner;
  DiscoveryApi api;
  QTimer deadline;
  std::unique_ptr<QObject> operation;
  QSet<QString> candidates, endpoints;
  QList<QVariantList> pending;
  int resolving = 0, interface = -1;
  State(ReceiverDiscovery &o, const DiscoveryApi &a) : owner(o), api(a) {
    deadline.setSingleShot(true);
    deadline.setInterval(10000);
    QObject::connect(&deadline, &QTimer::timeout, &owner, [this] { cancel(); });
  }
  void clear() {
    deadline.stop();
    operation.reset();
    pending.clear();
    resolving = 0;
  }
  void cancel() {
    clear();
    emit owner.status(endpoints.isEmpty()
      ? i18n::text(i18n::Id::ScanCompleteNoUsableIPvReceiversFound)
      : i18n::text(i18n::Id::ScanCompleteFoundReceiverS).arg(endpoints.size()));
    emit owner.idle();
  }
  void fail(const QString &error) {
    clear();
    emit owner.status(i18n::text(i18n::Id::LinuxDiscoveryFailed).arg(error));
    emit owner.idle();
  }
  void pump() {
    while (operation && resolving < 4 && !pending.isEmpty()) {
      const auto item = pending.takeFirst();
      auto *resolver = new AvahiObject(api, "org.freedesktop.Avahi.ServiceResolver", operation.get());
      ++resolving;
      auto done = [this, resolver] {
        QObject::disconnect(resolver, &AvahiObject::created, nullptr, nullptr);
        QObject::disconnect(resolver, &AvahiObject::failed, nullptr, nullptr);
        QObject::disconnect(resolver, &AvahiObject::event, nullptr, nullptr);
        resolver->deleteLater();
        --resolving;
        pump();
      };
      QObject::connect(resolver, &AvahiObject::created, resolver,
                       [resolver] { resolver->call("Start"); });
      QObject::connect(resolver, &AvahiObject::failed, resolver,
                       [this, done](const QString &error) {
        QTimer::singleShot(0, operation.get(), [this, error] {
          emit owner.status(i18n::text(i18n::Id::LinuxDiscoveryFailed).arg(error));
        });
        done();
      });
      QObject::connect(resolver, &AvahiObject::event, resolver,
          [this, done](const QString &member, const QVariantList &args) {
        if (member == "Found" && args.size() == 11) {
          const ReceiverEndpoint endpoint{QHostAddress(args[7].toString()), args[8].value<quint16>()};
          if ((interface < 0 || args[0].toInt() == interface) && args[6].toInt() == 0) {
            try {
              validateEndpoints({endpoint});
              if (!endpoints.contains(endpoint.text())) {
                endpoints.insert(endpoint.text());
                // Queue public signals so cancellation cannot destroy this callback.
                const auto name = args[2].toString();
                QTimer::singleShot(0, operation.get(), [this, name, endpoint] {
                  emit owner.found(name, endpoint.text());
                });
              }
            } catch (const std::exception &) {}
          }
          done();
        } else if (member == "Failure") done();
      });
      resolver->create("ServiceResolverPrepare",
          {item[0], 0, item[2], item[3], item[4], 0, uint(0)});
    }
  }
  void refresh(const NetworkBinding &binding) {
    clear();
    candidates.clear();
    endpoints.clear();
    emit owner.cleared();
    try {
      const auto route = NetworkRoute::resolve(binding);
      interface = route.index ? int(route.index) : -1;
    } catch (const std::exception &e) {
      fail(QString::fromUtf8(e.what()));
      return;
    }
    operation = std::make_unique<QObject>();
    auto *browser = new AvahiObject(api, "org.freedesktop.Avahi.ServiceBrowser", operation.get());
    QObject::connect(browser, &AvahiObject::created, browser,
                     [browser] { browser->call("Start"); });
    QObject::connect(browser, &AvahiObject::failed, &owner,
                     [this, token = QPointer(operation.get())](const QString &error) {
                       if (token) fail(error);
                     }, Qt::QueuedConnection);
    QObject::connect(browser, &AvahiObject::event, browser,
        [this](const QString &member, const QVariantList &args) {
      if (member == "Failure") {
        QTimer::singleShot(0, operation.get(), [this, args] { fail(args.value(0).toString()); });
      } else if (member == "ItemNew" && args.size() == 6 &&
                 (interface < 0 || args[0].toInt() == interface)) {
        const auto key = QString::number(args[0].toInt()) + '\n' + args[2].toString() + '\n' + args[4].toString();
        if (!candidates.contains(key) && candidates.size() < 128) {
          candidates.insert(key);
          pending.append(args);
          pump();
        }
      }
    });
    browser->create("ServiceBrowserPrepare", {interface, 0, "_airplay._tcp", "local", uint(0)});
    deadline.start();
    emit owner.status(i18n::text(i18n::Id::ScanningSecondsSelectOneOrTwoReceivers));
  }
};
const DiscoveryApi &defaultDiscoveryApi() { static const DiscoveryApi api; return api; }
ReceiverDiscovery::ReceiverDiscovery(QObject *parent, const DiscoveryApi &api)
    : QObject(parent), state_(std::make_unique<State>(*this, api)) {}
ReceiverDiscovery::~ReceiverDiscovery() = default;
void ReceiverDiscovery::refresh(const NetworkBinding &binding) { state_->refresh(binding); }
void ReceiverDiscovery::cancel() { state_->cancel(); }
bool ReceiverDiscovery::busy() const { return bool(state_->operation); }
} // namespace airplay
