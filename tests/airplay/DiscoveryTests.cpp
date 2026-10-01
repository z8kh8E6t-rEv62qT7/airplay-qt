#include "airplay/DiscoveryApi.h"
#include "airplay/ReceiverDiscovery.h"
#include <QtEndian>
#include <QtTest>
#include <thread>

using namespace airplay;
namespace {
struct FakeDns {
  struct Resolve {
    DNS_SERVICE_RESOLVE_REQUEST request;
    PDNS_SERVICE_CANCEL cancel;
  };
  DNS_SERVICE_BROWSE_REQUEST browse{};
  PDNS_SERVICE_CANCEL browseCancel = nullptr;
  QList<Resolve> requests;
  int browses = 0, resolutions = 0, maxActive = 0, freedRecords = 0,
      freedInstances = 0, cancels = 0;
  bool delayCancel = false;
  DNS_STATUS browseError = DNS_REQUEST_PENDING,
             resolveError = DNS_REQUEST_PENDING, cancelError = ERROR_SUCCESS;
  DiscoveryApi api() {
    DiscoveryApi api;
    api.browse = [this](auto *request, auto *cancel) {
      ++browses;
      browse = *request;
      browseCancel = cancel;
      return browseError;
    };
    api.resolve = [this](auto *request, auto *cancel) {
      ++resolutions;
      if (resolveError == DNS_REQUEST_PENDING) {
        requests.append({*request, cancel});
        maxActive = std::max(maxActive, int(requests.size()));
      }
      return resolveError;
    };
    api.cancelBrowse = [this](auto *cancel) {
      if (cancel != browseCancel)
        return DNS_STATUS(ERROR_INVALID_HANDLE);
      ++cancels;
      if (!delayCancel && cancelError == ERROR_SUCCESS)
        endBrowse();
      return cancelError;
    };
    api.cancelResolve = [this](auto *cancel) {
      ++cancels;
      if (!delayCancel && cancelError == ERROR_SUCCESS)
        for (qsizetype i = 0; i < requests.size(); ++i)
          if (requests[i].cancel == cancel) {
            finish(i, {}, {}, 0, ERROR_CANCELLED);
            break;
          }
      return cancelError;
    };
    api.freeRecords = [this](PDNS_RECORD record) {
      ++freedRecords;
      while (record) {
        auto *next = record->pNext;
        delete[] record->Data.PTR.pNameHost;
        delete record;
        record = next;
      }
    };
    api.freeInstance = [this](PDNS_SERVICE_INSTANCE instance) {
      ++freedInstances;
      delete[] instance->pszInstanceName;
      delete instance->ip4Address;
      delete instance;
    };
    return api;
  }
  static wchar_t *wide(const QString &text) {
    const auto w = text.toStdWString();
    auto *result = new wchar_t[w.size() + 1];
    std::copy(w.c_str(), w.c_str() + w.size() + 1, result);
    return result;
  }
  void advertise(const QStringList &names, DWORD status = ERROR_SUCCESS) {
    PDNS_RECORD head = nullptr, *tail = &head;
    for (const auto &name : names) {
      auto *record = new DNS_RECORD{};
      record->wType = DNS_TYPE_PTR;
      record->dwTtl = 120;
      record->Flags.S.CharSet = DnsCharSetUnicode;
      record->Data.PTR.pNameHost = wide(name + "._airplay._tcp.local");
      *tail = record;
      tail = &record->pNext;
    }
    browse.pBrowseCallback(status, browse.pQueryContext, head);
  }
  void endBrowse() {
    auto completed = browse;
    browse = {};
    browseCancel = nullptr;
    completed.pBrowseCallback(ERROR_CANCELLED, completed.pQueryContext,
                              nullptr);
  }
  void finish(qsizetype index, const QString &name, const QString &ip,
              quint16 port = 7000, DWORD status = ERROR_SUCCESS) {
    auto request = requests.takeAt(index).request;
    auto *instance = new DNS_SERVICE_INSTANCE{};
    instance->pszInstanceName = wide(name + "._airplay._tcp.local");
    if (!ip.isEmpty())
      instance->ip4Address =
          new IP4_ADDRESS(qToBigEndian(QHostAddress(ip).toIPv4Address()));
    instance->wPort = port;
    request.pResolveCompletionCallback(status, request.pQueryContext, instance);
  }
};
} // namespace
class DiscoveryTests : public QObject {
  Q_OBJECT
private slots:
  void scopedDiscovery() {
    const auto choices = NetworkBinding::available();
    QVERIFY(!choices.isEmpty());
    const auto index = NetworkRoute::resolve(choices.first()).index;
    FakeDns fake;
    ReceiverDiscovery discovery(nullptr, fake.api());
    discovery.refresh(choices.first());
    QCOMPARE(fake.browse.InterfaceIndex, index);
    fake.advertise({"Speaker"});
    QTRY_COMPARE(fake.requests.size(), 1);
    QCOMPARE(fake.requests.first().request.InterfaceIndex, index);
    discovery.cancel();
    QTRY_VERIFY(!discovery.busy());
  }
  void incrementalDedupAndInvalidResults() {
    FakeDns dns;
    ReceiverDiscovery discovery(nullptr, dns.api());
    QSignalSpy found(&discovery, &ReceiverDiscovery::found);
    discovery.refresh();
    QCOMPARE(dns.browse.InterfaceIndex, ULONG(0));
    QCOMPARE(QString::fromWCharArray(dns.browse.QueryName),
             QString("_airplay._tcp.local"));
    dns.advertise({"A", "B", "C", "D", "E", "A"});
    QTRY_COMPARE(dns.requests.size(), 4);
    dns.finish(0, "same name", "192.168.8.9", 7001);
    QTRY_COMPARE(found.size(), 1);
    QCOMPARE(dns.requests.size(), 4);
    QCOMPARE(found[0][1].toString(), QString("192.168.8.9:7001"));
    dns.finish(0, "same name", "192.168.8.10");
    dns.finish(0, "alias", "192.168.8.9", 7001);
    dns.finish(0, "IPv6 only", "");
    dns.finish(0, "bad port", "192.168.8.11", 0);
    QTRY_COMPARE(found.size(), 2);
    discovery.cancel();
    QTRY_VERIFY(!discovery.busy());
    QCOMPARE(dns.freedRecords, 1);
    QCOMPARE(dns.freedInstances, 5);
    QCOMPARE(dns.maxActive, 4);
  }
  void limits() {
    FakeDns dns;
    ReceiverDiscovery discovery(nullptr, dns.api());
    QSignalSpy status(&discovery, &ReceiverDiscovery::status);
    discovery.refresh();
    QStringList names;
    for (int i = 0; i < 140; ++i)
      names.append(QString::number(i));
    dns.advertise(names);
    QTRY_COMPARE(dns.requests.size(), 4);
    QVERIFY(status.last()[0].toString().contains("128"));
    while (dns.resolutions < 128 || !dns.requests.isEmpty()) {
      QVERIFY(!dns.requests.isEmpty());
      dns.finish(0, "same", "192.168.8.9");
      QCoreApplication::processEvents();
    }
    QCOMPARE(dns.resolutions, 128);
    QCOMPARE(dns.maxActive, 4);
    discovery.cancel();
    QTRY_VERIFY(!discovery.busy());
  }
  void refreshWaitsForLateCallbacks() {
    FakeDns dns;
    dns.delayCancel = true;
    ReceiverDiscovery discovery(nullptr, dns.api());
    QSignalSpy found(&discovery, &ReceiverDiscovery::found);
    discovery.refresh();
    dns.advertise({"old"});
    QTRY_COMPARE(dns.requests.size(), 1);
    discovery.refresh();
    discovery.refresh();
    QCOMPARE(dns.browses, 1);
    // A successful native completion already in flight after cancellation must
    // be discarded.
    dns.finish(0, "stale", "192.168.8.9");
    QCoreApplication::processEvents();
    QCOMPARE(found.size(), 0);
    QCOMPARE(dns.browses, 1);
    dns.endBrowse();
    QTRY_COMPARE(dns.browses, 2);
    dns.advertise({"new"});
    QTRY_COMPARE(dns.requests.size(), 1);
    dns.finish(0, "new", "192.168.8.10");
    QTRY_COMPARE(found.size(), 1);
    dns.delayCancel = false;
    discovery.cancel();
    QTRY_VERIFY(!discovery.busy());
  }
  void ownerDestroyedBeforeCallbacks() {
    FakeDns dns;
    dns.delayCancel = true;
    auto discovery = std::make_unique<ReceiverDiscovery>(nullptr, dns.api());
    discovery->refresh();
    dns.advertise({"late"});
    QTRY_COMPARE(dns.requests.size(), 1);
    discovery.reset();
    // Exercise native-thread delivery after the QObject and its queued events
    // are gone.
    std::thread callback([&] {
      dns.finish(0, "late", "192.168.8.9");
      dns.endBrowse();
    });
    callback.join();
    QCOMPARE(dns.freedInstances, 1);
    QCOMPARE(dns.cancels, 2);
  }
  void failures() {
    FakeDns dns;
    dns.browseError = ERROR_ACCESS_DENIED;
    ReceiverDiscovery discovery(nullptr, dns.api());
    QSignalSpy status(&discovery, &ReceiverDiscovery::status);
    discovery.refresh();
    QVERIFY(!discovery.busy());
    QVERIFY(status.last()[0].toString().contains("失败"));
    dns.browseError = DNS_REQUEST_PENDING;
    discovery.refresh();
    dns.resolveError = ERROR_INVALID_PARAMETER;
    dns.advertise({"bad"});
    QTRY_COMPARE(dns.resolutions, 1);
    QVERIFY(status.last()[0].toString().contains("失败"));
    dns.resolveError = DNS_REQUEST_PENDING;
    dns.advertise({"timeout"});
    QTRY_COMPARE(dns.requests.size(), 1);
    dns.finish(0, "timeout", "", 0, ERROR_TIMEOUT);
    QTRY_VERIFY(status.last()[0].toString().contains("解析失败"));
    dns.cancelError = ERROR_ACCESS_DENIED;
    discovery.cancel();
    QVERIFY(discovery.busy());
    QVERIFY(status.last()[0].toString().contains("取消失败"));
    dns.cancelError = ERROR_SUCCESS;
    discovery.cancel();
    QTRY_VERIFY(!discovery.busy());
  }
  void scanDeadline() {
    FakeDns dns;
    ReceiverDiscovery discovery(nullptr, dns.api());
    QSignalSpy idle(&discovery, &ReceiverDiscovery::idle);
    QElapsedTimer elapsed;
    elapsed.start();
    discovery.refresh();
    dns.advertise({"pending"});
    QTRY_COMPARE(dns.requests.size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(idle.size(), 1, 12000);
    QVERIFY(elapsed.elapsed() >= 10000);
    QVERIFY(elapsed.elapsed() < 12000);
    QVERIFY(!discovery.busy());
    QCOMPARE(dns.cancels, 2);
  }
};
QTEST_GUILESS_MAIN(DiscoveryTests)
#include "DiscoveryTests.moc"
