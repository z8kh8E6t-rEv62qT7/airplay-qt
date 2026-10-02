#pragma once
#include <functional>
#ifdef _WIN32
// windns.h requires the Windows scalar types.
// clang-format off
#include <windows.h>
#include <windns.h>
// clang-format on
#elif defined(__linux__)
#include <QDBusConnection>
#else
#include <dns_sd.h>
#endif
namespace airplay {
#ifdef _WIN32
// Direct native API injection for deterministic cancellation/resource tests.
struct DiscoveryApi {
  std::function<DNS_STATUS(PDNS_SERVICE_BROWSE_REQUEST, PDNS_SERVICE_CANCEL)>
      browse = DnsServiceBrowse;
  std::function<DNS_STATUS(PDNS_SERVICE_RESOLVE_REQUEST, PDNS_SERVICE_CANCEL)>
      resolve = DnsServiceResolve;
  std::function<DNS_STATUS(PDNS_SERVICE_CANCEL)> cancelBrowse =
      DnsServiceBrowseCancel;
  std::function<DNS_STATUS(PDNS_SERVICE_CANCEL)> cancelResolve =
      DnsServiceResolveCancel;
  std::function<void(PDNS_RECORD)> freeRecords = [](PDNS_RECORD records) {
    DnsRecordListFree(records, DnsFreeRecordList);
  };
  std::function<void(PDNS_SERVICE_INSTANCE)> freeInstance =
      DnsServiceFreeInstance;
};
#elif defined(__linux__)
struct DiscoveryApi {
  QDBusConnection bus = QDBusConnection::systemBus();
  QString service = QStringLiteral("org.freedesktop.Avahi");
};
#else
struct DiscoveryApi {
  decltype(&DNSServiceBrowse) browse = DNSServiceBrowse;
  decltype(&DNSServiceResolve) resolve = DNSServiceResolve;
  decltype(&DNSServiceGetAddrInfo) address = DNSServiceGetAddrInfo;
  decltype(&DNSServiceRefSockFD) socket = DNSServiceRefSockFD;
  decltype(&DNSServiceProcessResult) process = DNSServiceProcessResult;
  decltype(&DNSServiceRefDeallocate) release = DNSServiceRefDeallocate;
};
#endif
} // namespace airplay
