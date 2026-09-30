#include "RtspClient.h"
#include <QHostAddress>
#include <cmath>
#include <memory>
#include <plist/plist.h>

namespace airplay {
namespace {
struct PlistDelete {
  void operator()(void *p) const {
    if (p)
      plist_free(p);
  }
};
using Plist = std::unique_ptr<void, PlistDelete>;
using Memory = std::unique_ptr<void, decltype(&plist_mem_free)>;
plist_t toPlist(const QVariant &value, int depth) {
  if (depth > 32)
    throw Error("plist 嵌套过深");
  plist_t raw = nullptr;
  switch (value.typeId()) {
  case QMetaType::QVariantMap: {
    Plist node(plist_new_dict());
    if (!node)
      throw Error("plist 分配失败");
    const auto map = value.toMap();
    for (auto it = map.begin(); it != map.end(); ++it)
      plist_dict_set_item(node.get(), it.key().toUtf8().constData(),
                          toPlist(it.value(), depth + 1));
    return node.release();
  }
  case QMetaType::QVariantList: {
    Plist node(plist_new_array());
    if (!node)
      throw Error("plist 分配失败");
    for (const auto &item : value.toList())
      plist_array_append_item(node.get(), toPlist(item, depth + 1));
    return node.release();
  }
  case QMetaType::QString:
    raw = plist_new_string(value.toString().toUtf8().constData());
    break;
  case QMetaType::QByteArray: {
    auto b = value.toByteArray();
    raw = plist_new_data(b.constData(), uint64_t(b.size()));
    break;
  }
  case QMetaType::Bool:
    raw = plist_new_bool(value.toBool());
    break;
  case QMetaType::Int:
  case QMetaType::LongLong:
    raw = plist_new_int(value.toLongLong());
    break;
  case QMetaType::UInt:
  case QMetaType::ULongLong:
    raw = plist_new_uint(value.toULongLong());
    break;
  case QMetaType::Double:
    if (!std::isfinite(value.toDouble()))
      throw Error("plist 浮点值无效");
    raw = plist_new_real(value.toDouble());
    break;
  default:
    throw Error("不支持的 plist 数据类型");
  }
  if (!raw)
    throw Error("plist 分配失败");
  return raw;
}
QVariant fromPlist(plist_t node, int depth, int &count) {
  if (!node || depth > 32 || ++count > 10000)
    throw Error("plist 结构超限");
  switch (plist_get_node_type(node)) {
  case PLIST_DICT: {
    QVariantMap map;
    plist_dict_iter raw = nullptr;
    plist_dict_new_iter(node, &raw);
    Memory iterator(raw, plist_mem_free);
    for (;;) {
      char *key = nullptr;
      plist_t child = nullptr;
      plist_dict_next_item(node, iterator.get(), &key, &child);
      Memory name(key, plist_mem_free);
      if (!child)
        break;
      if (!key)
        throw Error("plist 字典缺少键");
      map.insert(QString::fromUtf8(key), fromPlist(child, depth + 1, count));
    }
    return map;
  }
  case PLIST_ARRAY: {
    QVariantList list;
    auto size = plist_array_get_size(node);
    if (size > 10000)
      throw Error("plist 数组过大");
    for (uint32_t i = 0; i < size; ++i)
      list.append(fromPlist(plist_array_get_item(node, i), depth + 1, count));
    return list;
  }
  case PLIST_STRING: {
    uint64_t size = 0;
    const char *s = plist_get_string_ptr(node, &size);
    if (size > 1048576)
      throw Error("plist 字符串过大");
    return QString::fromUtf8(s, qsizetype(size));
  }
  case PLIST_DATA: {
    uint64_t size = 0;
    const char *s = plist_get_data_ptr(node, &size);
    if (size > 1048576)
      throw Error("plist 数据过大");
    return QByteArray(s, qsizetype(size));
  }
  case PLIST_INT: {
    int64_t v = 0;
    plist_get_int_val(node, &v);
    return QVariant::fromValue<qlonglong>(v);
  }
  case PLIST_REAL: {
    double v = 0;
    plist_get_real_val(node, &v);
    return v;
  }
  case PLIST_BOOLEAN: {
    uint8_t v = 0;
    plist_get_bool_val(node, &v);
    return bool(v);
  }
  // Unknown optional /info fields are retained as invalid variants, never
  // coerced.
  default:
    return {};
  }
}
} // namespace
QByteArray plistEncode(const QVariant &value) {
  Plist root(toPlist(value, 0));
  char *raw = nullptr;
  uint32_t size = 0;
  const auto result = plist_to_bin(root.get(), &raw, &size);
  Memory buffer(raw, plist_mem_free);
  if (result != PLIST_ERR_SUCCESS || size > 1048576)
    throw Error("plist 序列化失败");
  return QByteArray(raw, size);
}
QVariant plistDecode(const QByteArray &data) {
  if (data.isEmpty() || data.size() > 1048576)
    throw Error("plist 长度无效");
  plist_t raw = nullptr;
  const auto result =
      plist_from_memory(data.constData(), uint32_t(data.size()), &raw, nullptr);
  Plist root(raw);
  if (result != PLIST_ERR_SUCCESS || !root)
    throw Error("接收端 plist 无效");
  int count = 0;
  return fromPlist(root.get(), 0, count);
}
std::optional<RtspResponse> parseResponse(QByteArray &data) {
  const auto end = data.indexOf("\r\n\r\n");
  if (end < 0) {
    if (data.size() > 16384)
      throw Error("RTSP 头部过大");
    return {};
  }
  if (end > 16384)
    throw Error("RTSP 头部过大");
  auto lines = data.left(end).split('\n');
  for (auto &line : lines)
    if (line.endsWith('\r'))
      line.chop(1);
  auto status = lines.takeFirst().split(' ');
  if (status.size() < 2 || (status[0] != "RTSP/1.0" &&
                            status[0] != "HTTP/1.1" && status[0] != "HTTP/1.0"))
    throw Error("RTSP 状态行无效");
  bool valid = false;
  int code = status[1].toInt(&valid);
  if (!valid || code < 100 || code > 999)
    throw Error("RTSP 状态码无效");
  QMap<QByteArray, QByteArray> headers;
  for (const auto &line : lines) {
    const auto separator = line.indexOf(':');
    if (separator <= 0)
      throw Error("RTSP 头部无效");
    const auto name = line.left(separator).toLower();
    for (char c : name)
      if (uint8_t(c) <= 32 || uint8_t(c) >= 127)
        throw Error("RTSP 头部名称无效");
    if (headers.contains(name))
      throw Error("RTSP 重复头部");
    headers.insert(name, line.mid(separator + 1).trimmed());
  }
  const auto lengthText = headers.value("content-length", "0");
  if (lengthText.isEmpty())
    throw Error("RTSP 长度无效");
  for (char c : lengthText)
    if (c < '0' || c > '9')
      throw Error("RTSP 长度无效");
  const auto length = lengthText.toLongLong(&valid);
  if (!valid || length > 1048576 || headers.contains("transfer-encoding"))
    throw Error("RTSP 消息长度或分帧无效");
  const auto total = end + 4 + length;
  if (data.size() < total)
    return {};
  RtspResponse response{code, headers, data.mid(end + 4, length)};
  data.remove(0, total);
  return response;
}
RtspClient::RtspClient(QObject *parent) : QObject(parent) {
  socket_.setReadBufferSize(1048576 + 32768);
  timer_.setSingleShot(true);
  connect(&timer_, &QTimer::timeout, this,
          [this] { fail("TCP/RTSP 请求超时"); });
  connect(&socket_, &QTcpSocket::connected, this, [this] {
    timer_.stop();
    socket_.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    emit opened();
  });
  connect(&socket_, &QTcpSocket::readyRead, this, &RtspClient::receive);
  connect(&socket_, &QTcpSocket::errorOccurred, this, [this] {
    if (!closed_)
      fail(socket_.errorString());
  });
  connect(&socket_, &QTcpSocket::disconnected, this, [this] {
    if (!closed_)
      fail("接收端控制连接已断开");
  });
}
void RtspClient::open(const QHostAddress &host, quint16 port,
                      const QString &identity, double timeout,
                      const QHostAddress &local) {
  abort();
  identity_ = identity.toLatin1();
  cseq_ = 0;
  session_.clear();
  closed_ = false;
  if (!local.isNull() && !socket_.bind(local, 0)) {
    fail(socket_.errorString());
    return;
  }
  timer_.start(int(std::ceil(timeout * 1000)));
  socket_.connectToHost(host, port);
}
void RtspClient::request(const QByteArray &method, const QByteArray &path,
                         const QByteArray &body, const QByteArray &type,
                         double timeout) {
  if (closed_ || pending_ || !connected() || cseq_ == UINT32_MAX ||
      body.size() > 1048576)
    throw Error("RTSP 请求状态无效");
  ++cseq_;
  QByteArray packet =
      method + " " + path + " RTSP/1.0\r\nCSeq: " + QByteArray::number(cseq_) +
      "\r\nUser-Agent: AirPlay/550.10\r\nDACP-ID: " + identity_ +
      "\r\nActive-Remote: 1\r\nContent-Length: " +
      QByteArray::number(body.size()) + "\r\n";
  if (!session_.isEmpty())
    packet += "Session: " + session_ + "\r\n";
  if (!type.isEmpty())
    packet += "Content-Type: " + type + "\r\n";
  if (path == "/pair-setup")
    packet += "X-Apple-HKP: 4\r\n";
  packet += "\r\n";
  packet += body;
  if (records_)
    packet = records_->encode(packet);
  pending_ = true;
  timer_.start(int(std::ceil(timeout * 1000)));
  if (socket_.write(packet) != packet.size())
    fail("RTSP 写入失败");
}
void RtspClient::encrypt(const QByteArray &shared) {
  if (!wire_.isEmpty() || !plain_.isEmpty() || records_)
    throw Error("配对后存在意外明文");
  records_ = std::make_unique<HapRecords>(
      hkdf(shared, "Control-Salt", "Control-Write-Encryption-Key"),
      hkdf(shared, "Control-Salt", "Control-Read-Encryption-Key"));
}
void RtspClient::abort() {
  closed_ = true;
  pending_ = false;
  timer_.stop();
  socket_.abort();
  records_.reset();
  wire_.clear();
  plain_.clear();
}
void RtspClient::fail(const QString &error) {
  if (closed_)
    return;
  abort();
  emit failed(error);
}
void RtspClient::receive() {
  try {
    wire_ += socket_.readAll();
    if (wire_.size() > 1048576 + 32768)
      throw Error("RTSP 接收缓冲超限");
    if (records_) {
      while (wire_.size() >= 2) {
        const int length = uint8_t(wire_[0]) + (uint8_t(wire_[1]) << 8);
        if (length < 1 || length > 1024)
          throw Error("HAP 记录长度无效");
        if (wire_.size() < length + 18)
          break;
        plain_ += records_->decode(wire_.left(2), wire_.mid(2, length + 16));
        wire_.remove(0, length + 18);
      }
    } else {
      plain_ += wire_;
      wire_.clear();
    }
    if (plain_.size() > 1048576 + 16388)
      throw Error("RTSP 明文缓冲超限");
    if (auto reply = parseResponse(plain_)) {
      if (!pending_)
        throw Error("收到未请求的 RTSP 响应");
      if (reply->headers.value("cseq", QByteArray::number(cseq_)) !=
          QByteArray::number(cseq_))
        throw Error("RTSP CSeq 不匹配");
      if (reply->status != 200)
        throw Error(QString("接收端返回状态 %1").arg(reply->status));
      if (reply->headers.contains("session"))
        session_ = reply->headers["session"].split(';')[0];
      pending_ = false;
      timer_.stop();
      emit response(reply->body);
    }
  } catch (const std::exception &error) {
    fail(QString::fromUtf8(error.what()));
  }
}
} // namespace airplay
