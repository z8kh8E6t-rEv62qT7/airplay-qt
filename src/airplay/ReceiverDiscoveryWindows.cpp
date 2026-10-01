#include "DiscoveryApi.h"
#include "ReceiverDiscovery.h"
#include "app/Message.h"
#include <QMetaObject>
#include <QSet>
#include <QTimer>
#include <QtEndian>
#include <atomic>
#include <mutex>

namespace airplay {
struct ReceiverDiscovery::State {
  ReceiverDiscovery &owner;
  explicit State(ReceiverDiscovery &, DiscoveryApi);
  ~State();
  void refresh(const NetworkBinding &);
  void cancel();
  bool busy() const;
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
  NetworkBinding binding_;
  uint selectedIndex_ = 0;
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

struct ReceiverDiscovery::State::Bridge {
  std::mutex mutex;
  State *owner = nullptr;
};
struct ReceiverDiscovery::State::Operation {
  std::shared_ptr<Bridge> bridge;
  // Native lease lasts until the terminal callback, including after owner
  // teardown.
  std::shared_ptr<Operation> nativeLease;
  DiscoveryApi api;
  DNS_SERVICE_CANCEL cancel{};
  DNS_SERVICE_BROWSE_REQUEST browse{};
  DNS_SERVICE_RESOLVE_REQUEST resolve{};
  std::wstring query;
  QSet<QString> seen;
  quint64 generation = 0;
  std::atomic_bool complete{false};
  bool cancelRequested = false, limitReported = false;
};
ReceiverDiscovery::State::State(ReceiverDiscovery &object, DiscoveryApi api)
    : owner(object), api_(std::move(api)), bridge_(std::make_shared<Bridge>()) {
  bridge_->owner = this;
  deadline_.setSingleShot(true);
  deadline_.setInterval(10000);
  deadline_.setTimerType(Qt::PreciseTimer);
  QObject::connect(&deadline_, &QTimer::timeout, &owner, [this] { cancel(); });
}
ReceiverDiscovery::State::~State() {
  // Prevent a native callback from posting against a destructing QObject.
  {
    std::lock_guard lock(bridge_->mutex);
    bridge_->owner = nullptr;
  }
  restart_ = scanning_ = false;
  cancelOperations();
}
bool ReceiverDiscovery::State::busy() const {
  return bool(browse_) || !resolving_.isEmpty();
}
void ReceiverDiscovery::State::refresh(const NetworkBinding &binding) {
  binding_ = binding;
  restart_ = true;
  scanning_ = false;
  deadline_.stop();
  pending_.clear();
  endpoints_.clear();
  emit owner.cleared();
  cancelOperations();
  drained();
}
void ReceiverDiscovery::State::cancel() {
  restart_ = scanning_ = false;
  deadline_.stop();
  pending_.clear();
  cancelOperations();
  drained();
}
void ReceiverDiscovery::State::cancelOperations() {
  auto cancelOne = [this](const std::shared_ptr<Operation> &op, bool browse) {
    if (!op || op->cancelRequested || op->complete.load())
      return;
    op->cancelRequested = true;
    const auto result = browse ? api_.cancelBrowse(&op->cancel)
                               : api_.cancelResolve(&op->cancel);
    if (result != ERROR_SUCCESS && !op->complete.load()) {
      op->cancelRequested = false;
      failed_ = true;
      emit owner.status(
          i18n::text(i18n::Id::DiscoveryCancellationFailedWaitingForTheNative)
              .arg(result));
    }
  };
  cancelOne(browse_, true);
  for (const auto &op : resolving_)
    cancelOne(op, false);
}
void ReceiverDiscovery::State::drained() {
  if (busy())
    return;
  if (restart_) {
    begin();
    return;
  }
  if (!failed_)
    emit owner.status(
        endpoints_.isEmpty()
            ? i18n::text(i18n::Id::ScanCompleteNoUsableIPvReceiversFound)
            : i18n::text(i18n::Id::ScanCompleteFoundReceiverS)
                  .arg(endpoints_.size()));
  emit owner.idle();
}
void ReceiverDiscovery::State::begin() {
  restart_ = failed_ = false;
  try {
    selectedIndex_ = NetworkRoute::resolve(binding_).index;
  } catch (const std::exception &e) {
    scanning_ = false;
    failed_ = true;
    emit owner.status(i18n::fromException(e));
    emit owner.idle();
    return;
  }
  scanning_ = true;
  ++generation_;
  endpoints_.clear();
  auto op = std::make_shared<Operation>();
  op->bridge = bridge_;
  op->api = api_;
  op->generation = generation_;
  op->query = L"_airplay._tcp.local";
  op->browse.Version = DNS_QUERY_REQUEST_VERSION1;
  op->browse.InterfaceIndex = selectedIndex_;
  op->browse.QueryName = op->query.c_str();
  op->browse.pBrowseCallback = &ReceiverDiscovery::State::browsed;
  op->browse.pQueryContext = op.get();
  browse_ = op;
  op->nativeLease = op;
  emit owner.status(
      i18n::text(i18n::Id::ScanningSecondsSelectOneOrTwoReceivers));
  deadline_.start();
  const auto result = api_.browse(&op->browse, &op->cancel);
  if (result != DNS_REQUEST_PENDING) {
    op->nativeLease.reset();
    browse_.reset();
    scanning_ = false;
    failed_ = true;
    deadline_.stop();
    emit owner.status(
        i18n::text(i18n::Id::DeviceDiscoveryFailedRefreshOrUseManual)
            .arg(result));
    drained();
  }
}
void WINAPI ReceiverDiscovery::State::browsed(DWORD status, PVOID context,
                                              PDNS_RECORD records) {
  auto *raw = static_cast<Operation *>(context);
  auto bridge = raw->bridge;
  std::lock_guard lock(bridge->mutex);
  auto op = raw->nativeLease;
  QStringList names;
  bool limit = false;
  if (status == ERROR_SUCCESS) {
    for (auto *record = records; record; record = record->pNext) {
      if (record->wType != DNS_TYPE_PTR || record->dwTtl == 0)
        continue;
      const auto *name = record->Data.PTR.pNameHost;
      if (!name)
        continue;
      const auto value =
          record->Flags.S.CharSet == DnsCharSetUnicode
              ? QString::fromWCharArray(reinterpret_cast<const wchar_t *>(name))
              : QString::fromUtf8(reinterpret_cast<const char *>(name));
      if (!value.endsWith("._airplay._tcp.local", Qt::CaseInsensitive) &&
          !value.endsWith("._airplay._tcp.local.", Qt::CaseInsensitive))
        continue;
      const auto key = value.toCaseFolded();
      if (op->seen.contains(key))
        continue;
      if (op->seen.size() >= 128) {
        if (!op->limitReported)
          limit = op->limitReported = true;
        continue;
      }
      op->seen.insert(key);
      names.append(value);
    }
  }
  if (records)
    op->api.freeRecords(records);
  if (status == ERROR_CANCELLED) {
    op->complete = true;
    op->nativeLease.reset();
  }
  if (auto *owner = op->bridge->owner;
      owner && (status != ERROR_SUCCESS || !names.isEmpty() || limit))
    QMetaObject::invokeMethod(
        &owner->owner,
        [owner, op, status, names, limit] {
          owner->browseResult(op, status, names, limit);
        },
        Qt::QueuedConnection);
}
void ReceiverDiscovery::State::browseResult(
    const std::shared_ptr<Operation> &op, DWORD status,
    const QStringList &names, bool limit) {
  if (browse_ != op || op->generation != generation_)
    return;
  if (status == ERROR_CANCELLED) {
    browse_.reset();
    drained();
    return;
  }
  if (!scanning_)
    return;
  if (status != ERROR_SUCCESS) {
    failed_ = true;
    emit owner.status(
        i18n::text(i18n::Id::DiscoveryQueryFailedManualModeIsAvailable)
            .arg(status));
    cancel();
    return;
  }
  if (limit)
    emit owner.status(
        i18n::text(i18n::Id::TheLimitOfCandidateServicesWasReached));
  pending_.append(names);
  pump();
}
void ReceiverDiscovery::State::pump() {
  while (scanning_ && resolving_.size() < 4 && !pending_.isEmpty()) {
    auto op = std::make_shared<Operation>();
    op->bridge = bridge_;
    op->api = api_;
    op->generation = generation_;
    op->query = pending_.takeFirst().toStdWString();
    op->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
    op->resolve.InterfaceIndex = selectedIndex_;
    op->resolve.QueryName = op->query.data();
    op->resolve.pResolveCompletionCallback =
        &ReceiverDiscovery::State::resolved;
    op->resolve.pQueryContext = op.get();
    resolving_.append(op);
    op->nativeLease = op;
    const auto result = api_.resolve(&op->resolve, &op->cancel);
    if (result != DNS_REQUEST_PENDING) {
      op->nativeLease.reset();
      resolving_.removeOne(op);
      emit owner.status(
          i18n::text(i18n::Id::ServiceResolutionCouldNotStartContinuingThe)
              .arg(result));
    }
  }
}
void WINAPI ReceiverDiscovery::State::resolved(DWORD status, PVOID context,
                                               PDNS_SERVICE_INSTANCE instance) {
  auto *raw = static_cast<Operation *>(context);
  auto bridge = raw->bridge;
  std::lock_guard lock(bridge->mutex);
  auto op = raw->nativeLease;
  QString name;
  ReceiverEndpoint endpoint;
  if (status == ERROR_SUCCESS && instance) {
    if (instance->pszInstanceName)
      name = QString::fromWCharArray(instance->pszInstanceName);
    if (instance->ip4Address)
      endpoint.host =
          QHostAddress(qFromBigEndian<quint32>(*instance->ip4Address));
    endpoint.port = instance->wPort;
  }
  if (instance)
    op->api.freeInstance(instance);
  op->complete = true;
  op->nativeLease.reset();
  if (auto *owner = op->bridge->owner)
    QMetaObject::invokeMethod(
        &owner->owner,
        [owner, op, status, name, endpoint] {
          owner->resolveResult(op, status, name, endpoint);
        },
        Qt::QueuedConnection);
}
void ReceiverDiscovery::State::resolveResult(
    const std::shared_ptr<Operation> &op, DWORD result, const QString &name,
    const ReceiverEndpoint &endpoint) {
  if (op->generation != generation_ || !resolving_.removeOne(op))
    return;
  if (scanning_) {
    if (result == ERROR_SUCCESS) {
      try {
        validateEndpoints({endpoint});
        if (!endpoints_.contains(endpoint.text())) {
          endpoints_.insert(endpoint.text());
          auto display = name;
          const auto suffix = display.lastIndexOf("._airplay._tcp.local", -1,
                                                  Qt::CaseInsensitive);
          if (suffix >= 0)
            display.truncate(suffix);
          emit owner.found(display.isEmpty() ? endpoint.text() : display,
                           endpoint.text());
        }
      } catch (const std::exception &) {
        emit owner.status(i18n::text(i18n::Id::IgnoredAServiceWithNoValidIPv));
      }
    } else if (result != ERROR_CANCELLED)
      emit owner.status(
          i18n::text(i18n::Id::ServiceResolutionFailedContinuingTheScan)
              .arg(result));
    pump();
  }
  drained();
}

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
bool ReceiverDiscovery::busy() const { return state_->busy(); }
} // namespace airplay
