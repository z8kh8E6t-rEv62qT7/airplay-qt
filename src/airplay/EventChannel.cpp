#include "EventChannel.h"
#include <cmath>

namespace airplay {
EventChannel::EventChannel(QObject *parent) : QObject(parent) {
  socket_.setReadBufferSize(65536);
  timeout_.setSingleShot(true);
  connect(&timeout_, &QTimer::timeout, this,
          [this] { fail("事件连接或消息接收超时"); });
  connect(&socket_, &QTcpSocket::connected, this, [this] {
    timeout_.stop();
    socket_.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    emit connected();
  });
  connect(&socket_, &QTcpSocket::readyRead, this, &EventChannel::receive);
  connect(&socket_, &QTcpSocket::errorOccurred, this,
          [this] { fail("事件连接失败：" + socket_.errorString()); });
  connect(&socket_, &QTcpSocket::disconnected, this,
          [this] { fail("事件连接被接收端关闭"); });
}
EventChannel::~EventChannel() { close(); }
void EventChannel::open(const QByteArray &shared, const QHostAddress &host,
                        quint16 port, const QHostAddress &local,
                        const NetworkRoute &route, double timeout) {
  close();
  records_ = std::make_unique<HapRecords>(
      hkdf(shared, "Events-Salt", "Events-Read-Encryption-Key"),
      hkdf(shared, "Events-Salt", "Events-Write-Encryption-Key"));
  if (!route.binding.automatic())
    route.bind(socket_);
  else if (!socket_.bind(local, 0))
    throw Error("事件连接本地地址绑定失败");
  closed_ = false;
  timeout_.start(int(std::ceil(timeout * 1000)));
  socket_.connectToHost(host, port);
}
void EventChannel::close() {
  closed_ = true;
  timeout_.stop();
  socket_.abort();
  records_.reset();
  wire_.clear();
  plain_.clear();
}
void EventChannel::fail(const QString &error) {
  if (closed_)
    return;
  close();
  emit failed(error);
}
void EventChannel::receive() {
  if (closed_)
    return;
  try {
    // Bound work per event-loop turn so remote traffic cannot starve audio.
    wire_ += socket_.read(16384);
    plain_ += records_->decodeAvailable(wire_);
    if (plain_.size() > 1048576 + 16388)
      throw Error("事件消息缓冲超限");
    int count = 0;
    while (++count <= 16) {
      auto message = parseControlMessage(plain_);
      if (!message)
        break;
      const auto line = message->line.split(' ');
      if (line.size() != 3 || (line[2] != "RTSP/1.0" && line[2] != "HTTP/1.1" &&
                               line[2] != "HTTP/1.0"))
        throw Error("事件请求行无效");
      for (char c : message->line)
        if (uint8_t(c) < 32 || uint8_t(c) >= 127)
          throw Error("事件请求行包含无效字符");
      const bool isCommand = line[0] == "POST" && line[1] == "/command";
      QString type;
      if (isCommand) {
        const auto value = plistDecode(message->body);
        if (value.typeId() != QMetaType::QVariantMap)
          throw Error("事件命令不是 plist 字典");
        type = value.toMap().value("type").toString().left(128);
        if (type.isEmpty())
          type = "无 type；字段=" + value.toMap().keys().join(',').left(128);
        type.replace('\n', ' ').replace('\r', ' ');
      }
      const auto cseq = message->headers.value("cseq");
      for (char c : cseq)
        if (c < '0' || c > '9')
          throw Error("事件 CSeq 无效");
      const int status = isCommand ? 200 : 501;
      auto response = line[2] + ' ' + QByteArray::number(status) +
                      (isCommand ? " OK\r\n" : " Not Implemented\r\n");
      response += "Content-Length: 0\r\nAudio-Latency: 0\r\n";
      if (!cseq.isEmpty())
        response += "CSeq: " + cseq + "\r\n";
      response += "\r\n";
      const auto encrypted = records_->encode(response);
      if (socket_.bytesToWrite() + encrypted.size() > 65536 ||
          socket_.write(encrypted) != encrypted.size())
        throw Error("事件响应写入失败或积压超限");
      emit log(QString("事件：%1 %2；type=%3；应答 %4")
                   .arg(QString::fromLatin1(line[0].left(16)),
                        QString::fromLatin1(line[1].left(128)), type)
                   .arg(status));
      if (isCommand)
        emit command(message->body);
      if (closed_)
        return;
    }
    if (wire_.isEmpty() && plain_.isEmpty())
      timeout_.stop();
    else if (!timeout_.isActive())
      timeout_.start(5000);
    if (socket_.bytesAvailable() || count > 16)
      QTimer::singleShot(0, this, &EventChannel::receive);
  } catch (const std::exception &e) {
    fail(QString::fromUtf8(e.what()));
  }
}
} // namespace airplay
