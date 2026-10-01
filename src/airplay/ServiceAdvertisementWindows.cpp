#include "Crypto.h"
#include "ServiceAdvertisement.h"
#include <QTimer>
#include <atomic>
#include <windns.h>
#include <winsock2.h>

namespace airplay {
// Native completion never touches Qt. Drain native work before releasing the
// request or unloading the plugin that owns the callback.
struct ServiceAdvertisement::State {
  DNS_SERVICE_REGISTER_REQUEST request{};
  DNS_SERVICE_CANCEL cancel{};
  HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<DWORD> status{DNS_REQUEST_PENDING};
  QTimer poll;
  bool pending = false, registered = false;
  ~State() {
    if (pending) {
      DnsServiceRegisterCancel(&cancel);
      WaitForSingleObject(completion, INFINITE);
      registered = status.load() == ERROR_SUCCESS;
    }
    if (registered) {
      ResetEvent(completion);
      if (DnsServiceDeRegister(&request, nullptr) == DNS_REQUEST_PENDING)
        WaitForSingleObject(completion, INFINITE);
    }
    if (request.pServiceInstance)
      DnsServiceFreeInstance(request.pServiceInstance);
    if (completion)
      CloseHandle(completion);
  }
  static void WINAPI completed(DWORD result, void *context,
                               DNS_SERVICE_INSTANCE *instance) {
    auto &s = *static_cast<State *>(context);
    if (instance)
      DnsServiceFreeInstance(instance);
    s.status.store(result);
    SetEvent(s.completion);
  }
};
ServiceAdvertisement::ServiceAdvertisement(QObject *parent) : QObject(parent) {}
ServiceAdvertisement::~ServiceAdvertisement() = default;
void ServiceAdvertisement::stop() { state_.reset(); }
void ServiceAdvertisement::start(const QString &identity, quint16 port,
                                 const NetworkRoute &route) {
  stop();
  route.validate();
  state_ = std::make_unique<State>();
  auto &s = *state_;
  if (!s.completion)
    throw Error("DACP 原生完成事件创建失败");
  const auto name =
      ("iTunes_Ctrl_" + identity + "._dacp._tcp.local").toStdWString();
  const auto host =
      ("airplayqt-" + identity.toLower() + ".local").toStdWString();
  const auto db = identity.toStdWString();
  PCWSTR keys[] = {L"txtvers", L"Ver", L"DbId", L"OSsi"};
  PCWSTR values[] = {L"1", L"131077", db.c_str(), L"0x1F5"};
  IP4_ADDRESS address = htonl(route.local.toIPv4Address());
  s.request.pServiceInstance = DnsServiceConstructInstance(
      name.c_str(), host.c_str(), route.local.isNull() ? nullptr : &address,
      nullptr, port, 0, 0, 4, keys, values);
  if (!s.request.pServiceInstance)
    throw Error("DACP 服务实例创建失败");
  s.request.Version = DNS_QUERY_REQUEST_VERSION1;
  s.request.InterfaceIndex = route.index;
  s.request.pRegisterCompletionCallback = &State::completed;
  s.request.pQueryContext = &s;
  const auto result = DnsServiceRegister(&s.request, &s.cancel);
  if (result != DNS_REQUEST_PENDING)
    throw Error(QString("DACP 服务发布失败：%1").arg(result));
  s.pending = true;
  s.poll.setInterval(20);
  connect(&s.poll, &QTimer::timeout, this, [this] {
    auto &s = *state_;
    if (WaitForSingleObject(s.completion, 0) != WAIT_OBJECT_0)
      return;
    s.poll.stop();
    s.pending = false;
    s.registered = s.status.load() == ERROR_SUCCESS;
    if (s.registered)
      emit ready();
    else
      emit failed(QString("DACP 服务发布失败：%1").arg(s.status.load()));
  });
  s.poll.start();
}
} // namespace airplay
