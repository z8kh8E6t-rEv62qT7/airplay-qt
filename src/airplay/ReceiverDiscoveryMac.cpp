#include "DiscoveryApi.h"
#include "ReceiverDiscovery.h"
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <QtEndian>
#include <map>
#include <netinet/in.h>

namespace airplay {
struct ReceiverDiscovery::State {
  struct Service {
    QByteArray name, type, domain;
    uint32_t interface;
  };
  struct Operation : std::enable_shared_from_this<Operation> {
    State &owner;
    DiscoveryApi api;
    quint64 generation;
    DNSServiceRef ref = nullptr;
    std::unique_ptr<QSocketNotifier> notifier;
    QString name;
    quint16 port = 0;
    bool complete = false;
    Operation(State &s) : owner(s), api(s.api), generation(s.generation) {}
    ~Operation() { close(); }
    void close() {
      notifier.reset();
      if (ref) {
        api.release(ref);
        ref = nullptr;
      }
    }
    template <class F> void post(F action) {
      auto op = shared_from_this();
      QMetaObject::invokeMethod(
          &owner.object,
          [op, action = std::move(action)] {
            if (op->generation == op->owner.generation && op->ref)
              action(*op);
          },
          Qt::QueuedConnection);
    }
    void watch() {
      const int fd = api.socket(ref);
      if (fd < 0)
        throw std::runtime_error("Bonjour socket 无效");
      notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
      std::weak_ptr<Operation> weak = shared_from_this();
      QObject::connect(notifier.get(), &QSocketNotifier::activated,
                       &owner.object, [weak] {
                         if (auto op = weak.lock(); op && op->ref) {
                           const auto error = op->api.process(op->ref);
                           if (error != kDNSServiceErr_NoError)
                             op->post([error](Operation &value) {
                               value.owner.failed(value, error);
                             });
                         }
                       });
    }
  };
  ReceiverDiscovery &object;
  DiscoveryApi api;
  QTimer deadline;
  quint64 generation = 0;
  uint selectedIndex = 0;
  std::shared_ptr<Operation> browse;
  QList<std::shared_ptr<Operation>> resolving;
  QList<Service> pending;
  QSet<QByteArray> seen;
  QSet<QString> endpoints;
  State(ReceiverDiscovery &owner, const DiscoveryApi &value)
      : object(owner), api(value) {
    deadline.setSingleShot(true);
    deadline.setInterval(10000);
    QObject::connect(&deadline, &QTimer::timeout, &object,
                     [this] { cancel(); });
  }
  ~State() { clear(); }
  void clear() {
    ++generation;
    deadline.stop();
    if (browse)
      browse->close();
    for (const auto &op : resolving)
      op->close();
    resolving.clear();
    browse.reset();
    pending.clear();
  }
  void cancel() {
    clear();
    emit object.status(
        endpoints.isEmpty()
            ? "扫描结束，未发现可用 IPv4 接收端；可刷新或使用手动模式。"
            : QString("扫描结束，找到 %1 台接收端。").arg(endpoints.size()));
    emit object.idle();
  }
  void failed(Operation &op, DNSServiceErrorType error) {
    const bool isBrowse = browse.get() == &op;
    if (isBrowse) {
      clear();
    } else {
      finish(op);
    }
    emit object.status(QString("Bonjour %1失败（%2）；可刷新或使用手动模式。")
                           .arg(isBrowse ? "查询" : "解析")
                           .arg(error));
    if (isBrowse)
      emit object.idle();
  }
  void finish(Operation &op) {
    op.close();
    for (auto it = resolving.begin(); it != resolving.end(); ++it)
      if (it->get() == &op) {
        resolving.erase(it);
        break;
      }
    pump();
  }
  static void browsed(DNSServiceRef, DNSServiceFlags flags, uint32_t interface,
                      DNSServiceErrorType error, const char *name,
                      const char *type, const char *domain, void *context) {
    auto &op = *static_cast<Operation *>(context);
    if (error) {
      op.post([error](Operation &o) { o.owner.failed(o, error); });
      return;
    }
    if (op.owner.selectedIndex && interface != op.owner.selectedIndex)
      return;
    if (!(flags & kDNSServiceFlagsAdd) || !name || !type || !domain)
      return;
    Service service{name, type, domain, interface};
    op.post([service](Operation &o) {
      auto &s = o.owner;
      const auto key = service.name + '\0' + service.type + '\0' +
                       service.domain + '\0' +
                       QByteArray::number(service.interface);
      if (s.seen.contains(key))
        return;
      if (s.seen.size() >= 128) {
        emit s.object.status(
            "候选服务达到 128 个上限，忽略其余服务；最多同时解析 4 个。");
        return;
      }
      s.seen.insert(key);
      s.pending.append(service);
      s.pump();
    });
  }
  static void resolved(DNSServiceRef, DNSServiceFlags, uint32_t interface,
                       DNSServiceErrorType error, const char *,
                       const char *host, uint16_t port, uint16_t,
                       const unsigned char *, void *context) {
    auto &op = *static_cast<Operation *>(context);
    if (op.complete)
      return;
    op.complete = true;
    if (op.owner.selectedIndex)
      interface = op.owner.selectedIndex;
    const QByteArray hostname = host ? host : "";
    op.post([error, hostname, port, interface](Operation &o) {
      if (error || hostname.isEmpty() || !port) {
        o.owner.failed(o, error ? error : kDNSServiceErr_BadParam);
        return;
      }
      // Native references are replaced only after ProcessResult has returned.
      o.close();
      o.complete = false;
      o.port = qFromBigEndian(port);
      const auto result =
          o.api.address(&o.ref, 0, interface, kDNSServiceProtocol_IPv4,
                        hostname.constData(), addressed, &o);
      if (result) {
        o.owner.failed(o, result);
        return;
      }
      try {
        o.watch();
      } catch (const std::exception &) {
        o.owner.failed(o, kDNSServiceErr_Unknown);
      }
    });
  }
  static void addressed(DNSServiceRef, DNSServiceFlags flags, uint32_t,
                        DNSServiceErrorType error, const char *,
                        const sockaddr *value, uint32_t, void *context) {
    auto &op = *static_cast<Operation *>(context);
    if (op.complete)
      return;
    if (!error && (!(flags & kDNSServiceFlagsAdd) || !value ||
                   value->sa_family != AF_INET))
      return;
    op.complete = true;
    QHostAddress host;
    if (!error)
      host = QHostAddress(qFromBigEndian(
          reinterpret_cast<const sockaddr_in *>(value)->sin_addr.s_addr));
    op.post([error, host](Operation &o) {
      if (error) {
        o.owner.failed(o, error);
        return;
      }
      auto &s = o.owner;
      const ReceiverEndpoint endpoint{host, o.port};
      try {
        validateEndpoints({endpoint});
        if (!s.endpoints.contains(endpoint.text())) {
          s.endpoints.insert(endpoint.text());
          emit s.object.found(o.name.isEmpty() ? endpoint.text() : o.name,
                              endpoint.text());
        }
      } catch (const std::exception &) {
        emit s.object.status("已忽略无有效 IPv4 地址或端口的服务，继续扫描。");
      }
      // A signal receiver may have canceled or refreshed discovery.
      if (o.generation == s.generation)
        s.finish(o);
    });
  }
  void pump() {
    while (browse && resolving.size() < 4 && !pending.isEmpty()) {
      const auto service = pending.takeFirst();
      auto op = std::make_shared<Operation>(*this);
      op->name = QString::fromUtf8(service.name);
      resolving.append(op);
      const auto error =
          api.resolve(&op->ref, 0, service.interface, service.name.constData(),
                      service.type.constData(), service.domain.constData(),
                      resolved, op.get());
      if (error) {
        failed(*op, error);
        continue;
      }
      try {
        op->watch();
      } catch (const std::exception &) {
        failed(*op, kDNSServiceErr_Unknown);
      }
    }
  }
  void refresh(const NetworkBinding &binding) {
    clear();
    seen.clear();
    endpoints.clear();
    emit object.cleared();
    try {
      selectedIndex = NetworkRoute::resolve(binding).index;
    } catch (const std::exception &e) {
      emit object.status(QString::fromUtf8(e.what()));
      emit object.idle();
      return;
    }
    auto op = std::make_shared<Operation>(*this);
    browse = op;
    const auto error = api.browse(&op->ref, 0, selectedIndex, "_airplay._tcp",
                                  "local.", browsed, op.get());
    if (error) {
      failed(*op, error);
      return;
    }
    try {
      op->watch();
    } catch (const std::exception &) {
      failed(*op, kDNSServiceErr_Unknown);
      return;
    }
    deadline.start();
    emit object.status("扫描中（10 秒）…请选择一台或两台接收端。");
  }
};
const DiscoveryApi &defaultDiscoveryApi() {
  static const DiscoveryApi api;
  return api;
}
ReceiverDiscovery::ReceiverDiscovery(QObject *parent, const DiscoveryApi &api)
    : QObject(parent), state_(std::make_unique<State>(*this, api)) {}
ReceiverDiscovery::~ReceiverDiscovery() = default;
void ReceiverDiscovery::refresh(const NetworkBinding &binding) {
  state_->refresh(binding);
}
void ReceiverDiscovery::cancel() { state_->cancel(); }
bool ReceiverDiscovery::busy() const {
  return bool(state_->browse) || !state_->resolving.isEmpty();
}
} // namespace airplay
