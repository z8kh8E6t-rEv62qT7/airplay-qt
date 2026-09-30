#pragma once
#include "ReceiverEndpoint.h"
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>
// windns.h depends on Windows scalar types; preserve this include order.
// clang-format off
#include <windows.h>
#include <windns.h>
// clang-format on

namespace airplay {
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
class ReceiverDiscovery : public QObject {
  Q_OBJECT
public:
  explicit ReceiverDiscovery(QObject *parent = nullptr, DiscoveryApi api = {});
  ~ReceiverDiscovery() override;
  void refresh();
  void cancel();
  bool busy() const;
signals:
  void cleared();
  void found(QString name, airplay::ReceiverEndpoint endpoint);
  void status(QString text);
  void idle();

private:
  struct Bridge;
  struct Operation;
  static void WINAPI browsed(DWORD, PVOID, PDNS_RECORD);
  static void WINAPI resolved(DWORD, PVOID, PDNS_SERVICE_INSTANCE);
  void begin();
  void browseResult(const std::shared_ptr<Operation> &, DWORD,
                    const QStringList &, bool limit);
  void resolveResult(const std::shared_ptr<Operation> &, DWORD, const QString &,
                     const ReceiverEndpoint &);
  void pump();
  void cancelOperations();
  void drained();
  DiscoveryApi api_;
  std::shared_ptr<Bridge> bridge_;
  std::shared_ptr<Operation> browse_;
  QList<std::shared_ptr<Operation>> resolving_;
  QStringList pending_;
  QSet<QString> endpoints_;
  QTimer deadline_;
  quint64 generation_ = 0;
  bool scanning_ = false, restart_ = false, failed_ = false;
};
} // namespace airplay
Q_DECLARE_METATYPE(airplay::ReceiverEndpoint)
