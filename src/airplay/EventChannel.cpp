#include "EventChannel.h"
#include "app/Message.h"
#include <cmath>

namespace airplay {
EventChannel::EventChannel(QObject *parent) : QObject(parent) {
  socket_.setReadBufferSize(65536);
  timeout_.setSingleShot(true);
  connect(&timeout_, &QTimer::timeout, this, [this] {
    fail(i18n::text(i18n::Id::EventConnectionOrMessageReceptionTimedOut));
  });
  connect(&socket_, &QTcpSocket::connected, this, [this] {
    timeout_.stop();
    socket_.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    emit connected();
  });
  connect(&socket_, &QTcpSocket::readyRead, this, &EventChannel::receive);
  connect(&socket_, &QTcpSocket::errorOccurred, this, [this] {
    fail(i18n::text(i18n::Id::EventConnectionFailed) + socket_.errorString());
  });
  connect(&socket_, &QTcpSocket::disconnected, this, [this] {
    fail(i18n::text(i18n::Id::ReceiverClosedTheEventConnection));
  });
}
EventChannel::~EventChannel() { close(); }
void EventChannel::open(const QByteArray &shared, const QHostAddress &host,
                        quint16 port, const QHostAddress &local,
                        const NetworkRoute &route, double timeoutMs) {
  close();
  records_ = std::make_unique<HapRecords>(
      hkdf(shared, "Events-Salt", "Events-Read-Encryption-Key"),
      hkdf(shared, "Events-Salt", "Events-Write-Encryption-Key"));
  if (!route.binding.automatic())
    route.bind(socket_);
  else if (!socket_.bind(local, 0))
    throw Error(i18n::text(i18n::Id::EventConnectionLocalAddressBindingFailed));
  closed_ = false;
  timeout_.start(int(std::ceil(timeoutMs)));
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
void EventChannel::fail(const i18n::Message &error) {
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
      throw Error(i18n::text(i18n::Id::EventMessageBufferLimitExceeded));
    int count = 0;
    while (++count <= 16) {
      auto message = parseControlMessage(plain_);
      if (!message)
        break;
      const auto line = message->line.split(' ');
      if (line.size() != 3 || (line[2] != "RTSP/1.0" && line[2] != "HTTP/1.1" &&
                               line[2] != "HTTP/1.0"))
        throw Error(i18n::text(i18n::Id::InvalidEventRequestLine));
      for (char c : message->line)
        if (uint8_t(c) < 32 || uint8_t(c) >= 127)
          throw Error(i18n::text(i18n::Id::InvalidCharacterInEventRequestLine));
      const bool isCommand = line[0] == "POST" && line[1] == "/command";
      i18n::Message type;
      QVariantMap commandData;
      if (isCommand) {
        const auto value = plistDecode(message->body);
        if (value.typeId() != QMetaType::QVariantMap)
          throw Error(i18n::text(i18n::Id::EventCommandIsNotAPlistDictionary));
        commandData = value.toMap();
        type = commandData
                   .value("type")
                   .toString()
                   .left(128)
                   .replace('\n', ' ')
                   .replace('\r', ' ');
        if (type.isEmpty())
          type = i18n::text(i18n::Id::NoTypeFields) + commandData
                                                          .keys()
                                                          .join(',')
                                                          .left(128)
                                                          .replace('\n', ' ')
                                                          .replace('\r', ' ');
      }
      const auto cseq = message->headers.value("cseq");
      for (char c : cseq)
        if (c < '0' || c > '9')
          throw Error(i18n::text(i18n::Id::InvalidEventCSeq));
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
        throw Error(
            i18n::text(i18n::Id::EventResponseWriteFailedOrBacklogLimit));
      emit log(i18n::text(i18n::Id::EventTypeResponse)
                   .arg(QString::fromLatin1(line[0].left(16)),
                        QString::fromLatin1(line[1].left(128)), type)
                   .arg(status));
      if (isCommand)
        emit command(commandData);
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
    fail(i18n::fromException(e));
  }
}
} // namespace airplay
