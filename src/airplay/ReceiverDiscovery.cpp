#include "ReceiverDiscovery.h"
#include <QMetaObject>
#include <QtEndian>
#include <atomic>
#include <mutex>

namespace airplay {
struct ReceiverDiscovery::Bridge {
  std::mutex mutex;
  ReceiverDiscovery *owner = nullptr;
};
struct ReceiverDiscovery::Operation {
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
ReceiverDiscovery::ReceiverDiscovery(QObject *parent, DiscoveryApi api)
    : QObject(parent), api_(std::move(api)),
      bridge_(std::make_shared<Bridge>()) {
  bridge_->owner = this;
  deadline_.setSingleShot(true);
  deadline_.setInterval(10000);
  deadline_.setTimerType(Qt::PreciseTimer);
  connect(&deadline_, &QTimer::timeout, this, &ReceiverDiscovery::cancel);
}
ReceiverDiscovery::~ReceiverDiscovery() {
  // Prevent a native callback from posting against a destructing QObject.
  {
    std::lock_guard lock(bridge_->mutex);
    bridge_->owner = nullptr;
  }
  restart_ = scanning_ = false;
  cancelOperations();
}
bool ReceiverDiscovery::busy() const {
  return bool(browse_) || !resolving_.isEmpty();
}
void ReceiverDiscovery::refresh() {
  restart_ = true;
  scanning_ = false;
  deadline_.stop();
  pending_.clear();
  endpoints_.clear();
  emit cleared();
  cancelOperations();
  drained();
}
void ReceiverDiscovery::cancel() {
  restart_ = scanning_ = false;
  deadline_.stop();
  pending_.clear();
  cancelOperations();
  drained();
}
void ReceiverDiscovery::cancelOperations() {
  auto cancelOne = [this](const std::shared_ptr<Operation> &op, bool browse) {
    if (!op || op->cancelRequested || op->complete.load())
      return;
    op->cancelRequested = true;
    const auto result = browse ? api_.cancelBrowse(&op->cancel)
                               : api_.cancelResolve(&op->cancel);
    if (result != ERROR_SUCCESS && !op->complete.load()) {
      op->cancelRequested = false;
      failed_ = true;
      emit status(
          QString("发现取消失败（%1），等待原生请求结束；可使用手动模式。")
              .arg(result));
    }
  };
  cancelOne(browse_, true);
  for (const auto &op : resolving_)
    cancelOne(op, false);
}
void ReceiverDiscovery::drained() {
  if (busy())
    return;
  if (restart_) {
    begin();
    return;
  }
  if (!failed_)
    emit status(
        endpoints_.isEmpty()
            ? "扫描结束，未发现可用 IPv4 接收端；可刷新或使用手动模式。"
            : QString("扫描结束，找到 %1 台接收端。").arg(endpoints_.size()));
  emit idle();
}
void ReceiverDiscovery::begin() {
  restart_ = failed_ = false;
  scanning_ = true;
  ++generation_;
  endpoints_.clear();
  auto op = std::make_shared<Operation>();
  op->bridge = bridge_;
  op->api = api_;
  op->generation = generation_;
  op->query = L"_airplay._tcp.local";
  op->browse.Version = DNS_QUERY_REQUEST_VERSION1;
  op->browse.InterfaceIndex = 0;
  op->browse.QueryName = op->query.c_str();
  op->browse.pBrowseCallback = &ReceiverDiscovery::browsed;
  op->browse.pQueryContext = op.get();
  browse_ = op;
  op->nativeLease = op;
  emit status("扫描中（10 秒）…请选择一台或两台接收端。");
  deadline_.start();
  const auto result = api_.browse(&op->browse, &op->cancel);
  if (result != DNS_REQUEST_PENDING) {
    op->nativeLease.reset();
    browse_.reset();
    scanning_ = false;
    failed_ = true;
    deadline_.stop();
    emit status(
        QString("设备发现失败（%1）；请刷新或使用手动模式。").arg(result));
    drained();
  }
}
void WINAPI ReceiverDiscovery::browsed(DWORD status, PVOID context,
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
        owner,
        [owner, op, status, names, limit] {
          owner->browseResult(op, status, names, limit);
        },
        Qt::QueuedConnection);
}
void ReceiverDiscovery::browseResult(const std::shared_ptr<Operation> &op,
                                     DWORD status, const QStringList &names,
                                     bool limit) {
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
    emit this->status(
        QString("发现查询失败（%1）；可使用手动模式。").arg(status));
    cancel();
    return;
  }
  if (limit)
    emit this->status(
        "候选服务达到 128 个上限，忽略其余服务；最多同时解析 4 个。");
  pending_.append(names);
  pump();
}
void ReceiverDiscovery::pump() {
  while (scanning_ && resolving_.size() < 4 && !pending_.isEmpty()) {
    auto op = std::make_shared<Operation>();
    op->bridge = bridge_;
    op->api = api_;
    op->generation = generation_;
    op->query = pending_.takeFirst().toStdWString();
    op->resolve.Version = DNS_QUERY_REQUEST_VERSION1;
    op->resolve.InterfaceIndex = 0;
    op->resolve.QueryName = op->query.data();
    op->resolve.pResolveCompletionCallback = &ReceiverDiscovery::resolved;
    op->resolve.pQueryContext = op.get();
    resolving_.append(op);
    op->nativeLease = op;
    const auto result = api_.resolve(&op->resolve, &op->cancel);
    if (result != DNS_REQUEST_PENDING) {
      op->nativeLease.reset();
      resolving_.removeOne(op);
      emit status(QString("服务解析启动失败（%1），继续扫描。").arg(result));
    }
  }
}
void WINAPI ReceiverDiscovery::resolved(DWORD status, PVOID context,
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
        owner,
        [owner, op, status, name, endpoint] {
          owner->resolveResult(op, status, name, endpoint);
        },
        Qt::QueuedConnection);
}
void ReceiverDiscovery::resolveResult(const std::shared_ptr<Operation> &op,
                                      DWORD result, const QString &name,
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
          emit found(display.isEmpty() ? endpoint.text() : display, endpoint);
        }
      } catch (const std::exception &) {
        emit status("已忽略无有效 IPv4 地址或端口的服务，继续扫描。");
      }
    } else if (result != ERROR_CANCELLED)
      emit status(QString("服务解析失败（%1），继续扫描。").arg(result));
    pump();
  }
  drained();
}
} // namespace airplay
