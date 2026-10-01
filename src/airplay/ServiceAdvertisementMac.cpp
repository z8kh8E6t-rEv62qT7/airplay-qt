#include "Crypto.h"
#include "ServiceAdvertisement.h"
#include <QSocketNotifier>
#include <QtEndian>
#include <dns_sd.h>

namespace airplay {
struct ServiceAdvertisement::State {
  ServiceAdvertisement &owner;
  quint64 generation;
  DNSServiceRef ref = nullptr;
  DNSServiceRef addressRef = nullptr;
  DNSRecordRef addressRecord = nullptr;
  std::unique_ptr<QSocketNotifier> notifier;
  std::unique_ptr<QSocketNotifier> addressNotifier;
  bool addressReady = false, serviceReady = false;
  explicit State(ServiceAdvertisement &o)
      : owner(o), generation(o.generation_) {}
  ~State() {
    notifier.reset();
    addressNotifier.reset();
    if (ref)
      DNSServiceRefDeallocate(ref);
    if (addressRef)
      DNSServiceRefDeallocate(addressRef);
  }
  void completed(DNSServiceErrorType code, bool address) {
    // Do not destroy the DNSServiceRef inside DNSServiceProcessResult.
    QMetaObject::invokeMethod(
        &owner,
        [&o = owner, generation = generation, code, address] {
          if (!o.state_ || o.generation_ != generation)
            return;
          if (code)
            emit o.failed(QString("DACP 服务发布失败：%1").arg(code));
          else {
            (address ? o.state_->addressReady : o.state_->serviceReady) = true;
            if (o.state_->addressReady && o.state_->serviceReady)
              emit o.ready();
          }
        },
        Qt::QueuedConnection);
  }
  static void registered(DNSServiceRef, DNSServiceFlags,
                         DNSServiceErrorType code, const char *, const char *,
                         const char *, void *context) {
    static_cast<State *>(context)->completed(code, false);
  }
  static void addressRegistered(DNSServiceRef, DNSRecordRef, DNSServiceFlags,
                                DNSServiceErrorType code, void *context) {
    static_cast<State *>(context)->completed(code, true);
  }
  std::unique_ptr<QSocketNotifier> watch(DNSServiceRef value) {
    const int fd = DNSServiceRefSockFD(value);
    if (fd < 0)
      throw Error("DACP Bonjour socket 无效");
    auto result = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    QObject::connect(result.get(), &QSocketNotifier::activated, &owner,
                     [this, value] {
                       const auto error = DNSServiceProcessResult(value);
                       if (error)
                         completed(error, false);
                     });
    return result;
  }
};
ServiceAdvertisement::ServiceAdvertisement(QObject *parent) : QObject(parent) {}
ServiceAdvertisement::~ServiceAdvertisement() = default;
void ServiceAdvertisement::stop() {
  ++generation_;
  state_.reset();
}
void ServiceAdvertisement::start(const QString &identity, quint16 port,
                                 const NetworkRoute &route) {
  stop();
  route.validate();
  state_ = std::make_unique<State>(*this);
  const auto name = "iTunes_Ctrl_" + identity.toLatin1();
  const auto host = "airplayqt-" + identity.toLower().toLatin1() + ".local.";
  QByteArray txt;
  for (const auto &item :
       {QByteArray("txtvers=1"), QByteArray("Ver=131077"),
        QByteArray("DbId=") + identity.toLatin1(), QByteArray("OSsi=0x1F5")}) {
    txt.append(char(item.size()));
    txt += item;
  }
  // A unique host with only the selected IPv4 avoids advertising a system
  // AAAA address for a listener intentionally restricted to IPv4.
  auto result = DNSServiceCreateConnection(&state_->addressRef);
  if (!result) {
    const quint32 ipv4 = qToBigEndian(route.local.toIPv4Address());
    result = DNSServiceRegisterRecord(
        state_->addressRef, &state_->addressRecord, kDNSServiceFlagsUnique,
        route.index, host.constData(), kDNSServiceType_A, kDNSServiceClass_IN,
        sizeof(ipv4), &ipv4, 0, &State::addressRegistered, state_.get());
  }
  if (!result)
    result = DNSServiceRegister(&state_->ref, kDNSServiceFlagsNoAutoRename,
                                route.index, name.constData(), "_dacp._tcp",
                                "local.", host.constData(), qToBigEndian(port),
                                uint16_t(txt.size()), txt.constData(),
                                &State::registered, state_.get());
  if (result) {
    stop();
    throw Error(QString("DACP 服务发布失败：%1").arg(result));
  }
  state_->notifier = state_->watch(state_->ref);
  state_->addressNotifier = state_->watch(state_->addressRef);
}
} // namespace airplay
