#include "airplay/DiscoveryApi.h"
#include "airplay/ReceiverDiscovery.h"
#include <QtTest>
#include <netinet/in.h>
#include <unistd.h>
using namespace airplay;
namespace {
struct Request {
  int pipe[2];
  DNSServiceBrowseReply browse = nullptr;
  DNSServiceResolveReply resolve = nullptr;
  DNSServiceGetAddrInfoReply address = nullptr;
  void *context = nullptr;
  Request() {
    if (::pipe(pipe))
      throw std::runtime_error("pipe failed");
  }
  ~Request() {
    ::close(pipe[0]);
    ::close(pipe[1]);
  }
};
struct FakeDiscovery {
  inline static FakeDiscovery *current;
  DiscoveryApi api;
  std::vector<Request *> requests;
  bool failResolve = false;
  FakeDiscovery() {
    current = this;
    api.browse = [](DNSServiceRef *out, DNSServiceFlags, uint32_t, const char *,
                    const char *, DNSServiceBrowseReply callback,
                    void *context) -> DNSServiceErrorType {
      auto *r = new Request;
      r->browse = callback;
      r->context = context;
      current->requests.push_back(r);
      *out = reinterpret_cast<DNSServiceRef>(r);
      return 0;
    };
    api.resolve = [](DNSServiceRef *out, DNSServiceFlags, uint32_t,
                     const char *, const char *, const char *,
                     DNSServiceResolveReply callback,
                     void *context) -> DNSServiceErrorType {
      if (current->failResolve)
        return kDNSServiceErr_Unknown;
      auto *r = new Request;
      r->resolve = callback;
      r->context = context;
      current->requests.push_back(r);
      *out = reinterpret_cast<DNSServiceRef>(r);
      return 0;
    };
    api.address = [](DNSServiceRef *out, DNSServiceFlags, uint32_t,
                     DNSServiceProtocol, const char *,
                     DNSServiceGetAddrInfoReply callback,
                     void *context) -> DNSServiceErrorType {
      auto *r = new Request;
      r->address = callback;
      r->context = context;
      current->requests.push_back(r);
      *out = reinterpret_cast<DNSServiceRef>(r);
      return 0;
    };
    api.socket = [](DNSServiceRef ref) -> int {
      return reinterpret_cast<Request *>(ref)->pipe[0];
    };
    api.process = [](DNSServiceRef) -> DNSServiceErrorType { return 0; };
    api.release = [](DNSServiceRef ref) {
      auto *r = reinterpret_cast<Request *>(ref);
      std::erase(current->requests, r);
      delete r;
    };
  }
  void browse(const char *name = "Speaker") {
    auto *r = requests.front();
    r->browse(nullptr, kDNSServiceFlagsAdd, 1, 0, name, "_airplay._tcp",
              "local.", r->context);
  }
  void resolved() {
    auto *r = requests.back();
    r->resolve(nullptr, 0, 1, 0, "Speaker", "speaker.local.", htons(7000), 0,
               nullptr, r->context);
  }
  void addressed() {
    auto *r = requests.back();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0xc0a80109);
    r->address(nullptr, kDNSServiceFlagsAdd, 1, 0, "speaker.local.",
               reinterpret_cast<sockaddr *>(&a), 120, r->context);
  }
  static void flush() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  }
};
} // namespace
class DiscoveryMacTests : public QObject {
  Q_OBJECT
private slots:
  void resolveAndDeduplicate() {
    FakeDiscovery f;
    ReceiverDiscovery d(nullptr, f.api);
    QSignalSpy found(&d, &ReceiverDiscovery::found);
    d.refresh();
    QVERIFY(d.busy());
    f.browse();
    f.browse();
    f.flush();
    QCOMPARE(f.requests.size(), size_t(2));
    f.resolved();
    f.flush();
    QCOMPARE(f.requests.size(), size_t(2));
    f.addressed();
    f.flush();
    QCOMPARE(found.count(), 1);
    QCOMPARE(f.requests.size(), size_t(1));
    QCOMPARE(found[0][1].toString(), QString("192.168.1.9:7000"));
    f.browse("Second name");
    f.flush();
    f.resolved();
    f.flush();
    f.addressed();
    f.flush();
    QCOMPARE(found.count(), 1);
    d.cancel();
    QVERIFY(!d.busy());
    QVERIFY(f.requests.empty());
  }
  void queuedResultsAfterCancelAndRefresh() {
    FakeDiscovery f;
    ReceiverDiscovery d(nullptr, f.api);
    QSignalSpy found(&d, &ReceiverDiscovery::found);
    d.refresh();
    f.browse();
    d.cancel();
    f.flush();
    QVERIFY(f.requests.empty());
    d.refresh();
    f.browse();
    f.flush();
    f.resolved();
    d.refresh();
    f.flush();
    QCOMPARE(f.requests.size(), size_t(1));
    QCOMPARE(found.count(), 0);
    d.cancel();
    QVERIFY(f.requests.empty());
  }
  void failureAndBoundedResolution() {
    FakeDiscovery f;
    ReceiverDiscovery d(nullptr, f.api);
    d.refresh();
    f.failResolve = true;
    f.browse();
    f.flush();
    QCOMPARE(f.requests.size(), size_t(1));
    f.failResolve = false;
    for (int i = 0; i < 140; ++i)
      f.browse(qPrintable(QString::number(i)));
    f.flush();
    QCOMPARE(f.requests.size(), size_t(5));
    d.cancel();
    QVERIFY(f.requests.empty());
  }
  void destructionReleasesPending() {
    FakeDiscovery f;
    {
      ReceiverDiscovery d(nullptr, f.api);
      d.refresh();
      f.browse();
      f.flush();
      f.resolved();
    }
    f.flush();
    QVERIFY(f.requests.empty());
  }
};
QTEST_GUILESS_MAIN(DiscoveryMacTests)
#include "DiscoveryMacTests.moc"
