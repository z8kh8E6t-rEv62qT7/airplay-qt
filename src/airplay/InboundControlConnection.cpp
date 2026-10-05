#include "InboundControlConnection.h"
#include "InboundControlServer.h"
#include <QNetworkProxy>
#include <algorithm>
#include <openssl/crypto.h>
#include <utility>

namespace airplay {
namespace {
constexpr qsizetype BufferLimit = 1048576 + 32768;
class PublicationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
} // namespace
InboundControlConnection::InboundControlConnection(QTcpSocket *socket,
                                                   InboundControlServer &owner)
    : QObject(&owner), owner_(owner) {
  attach(control_, socket);
  deadline_.setSingleShot(true);
  deadline_.start(10000);
  connect(&deadline_, &QTimer::timeout, this,
          [this] { finish("connection deadline"); });
  eventDeadline_.setSingleShot(true);
  connect(&eventDeadline_, &QTimer::timeout, this,
          [this] { finish("MRP event acknowledgement timeout", true); });
  eventServer_.setProxy(QNetworkProxy::NoProxy);
  eventServer_.setMaxPendingConnections(1);
  connect(&eventServer_, &QTcpServer::newConnection, this,
          &InboundControlConnection::acceptEvent);
}

InboundControlConnection::~InboundControlConnection() {
  shutdown();
  owner_.connections_.remove(this);
}

void InboundControlConnection::controlUpdated(bool publish) {
  if (closing_)
    return;
  try {
    if (publish) {
      const auto updates = streams_.updates(owner_.identity_, owner_.state_);
      for (auto it = updates.cbegin(); it != updates.cend(); ++it)
        enqueue(it.key(), it.value(), true);
    }
    if (owner_.state_.settled || !owner_.state_.available) {
      const auto pending = std::exchange(pendingControls_, {});
      for (const auto &item : pending)
        if (streams_.contains(item.stream))
          enqueue(item.stream, MrpSession::complete(item.request,
                                                    owner_.state_.available &&
                                                        owner_.state_.settled));
    }
    sendNextEvent();
  } catch (const PublicationError &e) {
    finish(QString::fromUtf8(e.what()), true);
  } catch (const std::exception &e) {
    finish(QString::fromUtf8(e.what()));
  }
}

void InboundControlConnection::attach(Channel &channel, QTcpSocket *socket) {
  channel.socket = socket;
  socket->setParent(this);
  socket->setReadBufferSize(BufferLimit);
  connect(socket, &QTcpSocket::readyRead, this,
          [this, &channel] { read(channel); });
  connect(socket, &QTcpSocket::disconnected, this,
          [this] { finish({}, false); });
}

void InboundControlConnection::clear(Channel &channel) {
  if (channel.socket) {
    channel.socket->disconnect(this);
    channel.socket->abort();
  }
  channel.records.reset();
  channel.wire.clear();
  channel.plain.clear();
}

void InboundControlConnection::shutdown() {
  deadline_.stop();
  eventDeadline_.stop();
  outgoing_.clear();
  pendingControls_.clear();
  newControls_.clear();
  inFlight_.reset();
  eventServer_.close();
  while (auto *socket = eventServer_.nextPendingConnection())
    delete socket;
  clear(event_);
  clear(control_);
  pairing_.reset();
  streams_.clear();
}

void InboundControlConnection::finish(const QString &reason,
                                      bool publicationFailure) {
  if (closing_)
    return;
  closing_ = true;
  shutdown();
  if (publicationFailure)
    owner_.publicationFailed();
  else if (!reason.isEmpty())
    emit owner_.error(i18n::Message(reason));
  deleteLater();
}

void InboundControlConnection::drainAndClose() {
  closing_ = true;
  deadline_.stop();
  eventDeadline_.stop();
  outgoing_.clear();
  pendingControls_.clear();
  newControls_.clear();
  inFlight_.reset();
  eventServer_.close();
  clear(event_);
  streams_.clear();

  // Preserve the queued control reply, but keep teardown bounded.
  connect(control_.socket, &QTcpSocket::disconnected, this,
          &QObject::deleteLater);
  control_.socket->disconnectFromHost();
  QTimer::singleShot(1000, this, &QObject::deleteLater);
}

QVariantMap InboundControlConnection::info() const {
  auto result = owner_.info_;
  result.insert("senderAddress",
                control_.socket->peerAddress().toString() + ":" +
                    QString::number(control_.socket->peerPort()));
  return result;
}

void InboundControlConnection::reply(Channel &channel,
                                     const ControlMessage &request, int status,
                                     const QByteArray &body,
                                     const QByteArray &type) {
  auto out = "RTSP/1.0 " + QByteArray::number(status) +
             (status == 200 ? " OK\r\n" : " Error\r\n") +
             "CSeq: " + request.headers.value("cseq") +
             "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
  if (!body.isEmpty())
    out += "Content-Type: " + type + "\r\n";
  out += "\r\n" + body;
  write(channel, out);
}

void InboundControlConnection::write(Channel &channel, QByteArray out) {
  if (channel.records)
    out = channel.records->encode(out);
  if (channel.socket->bytesToWrite() + out.size() > BufferLimit ||
      channel.socket->write(out) != out.size())
    throw std::runtime_error("Response write failed or output limit exceeded");
}

void InboundControlConnection::enqueue(quint64 stream, const QByteArray &data,
                                       bool snapshot) {
  if (data.isEmpty())
    return;
  QList<QByteArray> frames;
  try {
    frames = MrpSession::outgoingFrames(data);
  } catch (const std::exception &) {
    throw PublicationError("MRP event payload framing or limit error");
  }
  // Replace only unsent state snapshots, never command results or in-flight
  // data.
  if (snapshot)
    for (auto &item : outgoing_)
      if (item.stream == stream && item.snapshot) {
        item.frames = std::move(frames);
        return;
      }
  if (outgoing_.size() + int(inFlight_.has_value()) >= 32)
    throw PublicationError("MRP event queue limit exceeded");
  outgoing_.append({stream, std::move(frames), snapshot});
}

void InboundControlConnection::executeControls() {
  if (closing_)
    return;
  const auto work = std::exchange(newControls_, {});
  for (const auto &item : work) {
    if (!streams_.contains(item.stream))
      continue;
    // A synchronous no-op can settle immediately. The owner refreshes state
    // on the next event-loop turn, after the complete request batch is queued.
    const bool accepted = !owner_.stopping_ && owner_.handler_ &&
                          owner_.handler_(*item.request.request);
    if (!accepted)
      enqueue(item.stream, MrpSession::complete(item.request, false));
    else {
      owner_.state_.settled = false;
      pendingControls_.append(item);
    }
  }
  sendNextEvent();
}

void InboundControlConnection::sendNextEvent() {
  if (closing_ || inFlight_ || !event_.socket)
    return;
  while (!outgoing_.isEmpty() && !streams_.contains(outgoing_.first().stream))
    outgoing_.removeFirst();
  if (outgoing_.isEmpty())
    return;
  if (nextEventSequence_ == 9999999999ULL)
    throw PublicationError("Event CSeq exhausted");
  auto &batch = outgoing_.first();
  const auto stream = batch.stream;
  const auto data = batch.frames.takeFirst();
  if (batch.frames.isEmpty())
    outgoing_.removeFirst();
  const auto sequence = ++nextEventSequence_;
  const auto body =
      plistEncode(QVariantMap{{"params", QVariantMap{{"data", data}}}});
  const auto request =
      "POST /command RTSP/1.0\r\nCSeq: " + QByteArray::number(sequence) +
      "\r\nX-Apple-StreamID: " + QByteArray::number(stream) +
      "\r\nContent-Type: application/x-apple-binary-plist\r\nContent-Length: " +
      QByteArray::number(body.size()) + "\r\n\r\n" + body;
  try {
    write(event_, request);
  } catch (const std::exception &) {
    throw PublicationError("MRP event write failed");
  }
  inFlight_ = InFlight{sequence, stream};
  eventDeadline_.start(5000);
}

void InboundControlConnection::eventResponse(Channel &channel,
                                             const ControlMessage &message) {
  const auto parts = message.line.split(' ');
  const bool hasSequence = message.headers.contains("cseq");
  const auto sequenceText = message.headers.value("cseq");

  if (&channel != &event_ || !channel.records)
    throw std::runtime_error("RTSP response outside authenticated event channel");
  if (!inFlight_ || parts.size() < 3 || parts[1].size() != 3)
    throw PublicationError("Unexpected MRP event acknowledgement");
  // Only one request is in flight on this authenticated connection. A peer
  // may omit CSeq; an explicitly supplied value must still match exactly.
  if (hasSequence) {
    if (sequenceText.isEmpty() || sequenceText.size() > 10)
      throw PublicationError("Invalid event CSeq");
    for (const auto c : sequenceText)
      if (c < '0' || c > '9')
        throw PublicationError("Invalid event CSeq");
    bool valid = false;
    const auto sequence = sequenceText.toULongLong(&valid);
    if (!valid || sequence != inFlight_->sequence)
      throw PublicationError("Unexpected MRP event acknowledgement");
  }
  if (parts[1] != "200")
    throw PublicationError("MRP event response failed");

  eventDeadline_.stop();
  inFlight_.reset();
  sendNextEvent();
}

void InboundControlConnection::read(Channel &channel) {
  if (closing_)
    return;
  try {
    channel.wire += channel.socket->read(16384);
    if (channel.wire.size() > BufferLimit)
      throw std::runtime_error("Wire buffer limit exceeded");
    if (channel.records)
      channel.plain += channel.records->decodeAvailable(channel.wire);
    else {
      channel.plain += channel.wire;
      channel.wire.clear();
    }
    if (channel.plain.size() > BufferLimit)
      throw std::runtime_error("Plaintext buffer limit exceeded");
    int count = 0;
    while (!closing_ && count < 16) {
      auto message = parseControlMessage(channel.plain);
      if (!message)
        break;
      ++count;
      handle(channel, *message);
    }
    if (!closing_ && (channel.socket->bytesAvailable() || count == 16))
      QTimer::singleShot(0, this, [this, &channel] { read(channel); });
  } catch (const PublicationError &e) {
    finish(QString::fromUtf8(e.what()), true);
  } catch (const std::exception &e) {
    finish(QString::fromUtf8(e.what()));
  }
}

void InboundControlConnection::handle(Channel &channel,
                                      const ControlMessage &message) {
  if (message.line.startsWith("RTSP/1.0 ")) {
    eventResponse(channel, message);
    return;
  }
  const auto parts = message.line.split(' ');
  if (parts.size() != 3 || parts[2] != "RTSP/1.0" || parts[0].isEmpty() ||
      parts[1].isEmpty())
    throw std::runtime_error("Invalid RTSP request line");
  for (char c : message.line)
    if (uint8_t(c) < 32 || uint8_t(c) > 126)
      throw std::runtime_error("Invalid RTSP request character");
  const auto sequence = message.headers.value("cseq");
  if (sequence.isEmpty() || sequence.size() > 10)
    throw std::runtime_error("Missing or invalid CSeq");
  for (char c : sequence)
    if (c < '0' || c > '9')
      throw std::runtime_error("Invalid CSeq");
  const auto &method = parts[0];
  const auto &path = parts[1];
  std::optional<QVariant> plist;
  // Apple's pair-setup can label TLV8 as binary-plist: identify by magic.
  if (message.body.startsWith("bplist00")) {
    try {
      plist = plistDecode(message.body);
    } catch (const std::exception &) {
      reply(channel, message, 400);
      return;
    }
  }
  if (channel.records && method == "POST" && path == "/command" &&
      message.headers.contains("x-apple-streamid")) {
    const auto result =
        streams_.message(message.headers.value("x-apple-streamid"), plist);
    QList<MrpSession::Exchange> exchanges;
    const auto id = result.stream;
    if (result.status == 200) {
      try {
        exchanges =
            streams_.receive(id, result.data, owner_.identity_, owner_.state_);
      } catch (const std::exception &) {

        reply(channel, message, 400);
        return;
      }
      const auto commands = std::ranges::count_if(
          exchanges, [](const auto &item) { return item.request.has_value(); });
      if (commands + newControls_.size() + pendingControls_.size() > 32)
        throw std::runtime_error("MRP pending control limit exceeded");
      for (const auto &exchange : exchanges) {

        if (!exchange.deviceUID.isEmpty())
          owner_.observeDevice(control_.socket->peerAddress(),
                               exchange.deviceUID);
        enqueue(id, exchange.response);
        if (exchange.request.has_value()) {
          newControls_.append({id, exchange});
        }
        if (exchange.type == 15)
          deadline_.stop();
      }
    }
    // RTSP acknowledges receipt. MRP replies travel independently
    // over the authenticated event connection, never in this response body.
    reply(channel, message, result.status);
    sendNextEvent();
    if (!newControls_.isEmpty())
      QTimer::singleShot(0, this, [this] {
        try {
          executeControls();
        } catch (const PublicationError &e) {
          finish(QString::fromUtf8(e.what()), true);
        } catch (const std::exception &e) {
          finish(QString::fromUtf8(e.what()));
        }
      });
    return;
  }
  if (&channel == &event_) {
    // Acknowledge receipt only: no playback action or MRP reply is synthesized.
    const int status =
        method == "POST" && path == "/command"
            ? (plist && plist->typeId() == QMetaType::QVariantMap ? 200 : 400)
            : 501;
    reply(channel, message, status);
    return;
  }
  if (method == "GET" && (path == "/info" || path.startsWith("/info?"))) {
    reply(channel, message, 200, plistEncode(info()),
          "application/x-apple-binary-plist");
    return;
  }
  if (!control_.records && method == "POST" && path == "/pair-setup") {
    pair(message);
    return;
  }
  if (!control_.records) {
    reply(channel, message, 470);
    return;
  }
  if (method == "POST" && path == "/fp-setup") {
    if (eventPort_) {
      reply(channel, message, 455);
      return;
    }
    if (message.headers.value("x-apple-et") != "32") {
      reply(channel, message, 501);
      return;
    }
    const auto response = fairPlay_.respond(message.body);
    if (!response) {
      reply(channel, message, 400);
      return;
    }
    reply(channel, message, 200, *response);

    return;
  }
  if (method == "SETUP") {
    setup(message, plist);
    return;
  }
  if (method == "RECORD" || method == "TEARDOWN" ||
      (method == "POST" && path == "/feedback")) {
    if (!eventPort_) {
      reply(channel, message, 455);
      return;
    }
    if (method == "TEARDOWN" && plist &&
        plist->typeId() == QMetaType::QVariantMap &&
        plist->toMap().contains("streams")) {
      const auto status = streams_.teardown(plist->toMap().value("streams"));
      if (status == 200) {
        for (auto i = outgoing_.size(); i > 0; --i)
          if (!streams_.contains(outgoing_[i - 1].stream))
            outgoing_.removeAt(i - 1);
        for (auto i = pendingControls_.size(); i > 0; --i)
          if (!streams_.contains(pendingControls_[i - 1].stream))
            pendingControls_.removeAt(i - 1);
        for (auto i = newControls_.size(); i > 0; --i)
          if (!streams_.contains(newControls_[i - 1].stream))
            newControls_.removeAt(i - 1);
      }
      // Already-written events still need their RTSP ack, but never restore a
      // deleted stream.
      reply(channel, message, status);
      return;
    }
    const bool emptyDictionary = plist &&
                                 plist->typeId() == QMetaType::QVariantMap &&
                                 plist->toMap().isEmpty();
    if (!message.body.isEmpty() && !emptyDictionary) {
      reply(channel, message, 501);
      return;
    }
    reply(channel, message, 200);
    if (method == "TEARDOWN")
      drainAndClose();
    return;
  }
  reply(channel, message, 501);
}

void InboundControlConnection::pair(const ControlMessage &message) {
  const auto tlv = tlvDecode(message.body);
  if (message.headers.value("x-apple-hkp") != "4") {
    reply(control_, message, 501);
    return;
  }
  if (tlv.value(6) == QByteArray(1, char(1)) && !pairing_) {
    if (tlv.value(0) != QByteArray(1, char(0)) ||
        tlv.value(19) != QByteArray(1, char(0x10))) {
      reply(control_, message, 400);
      return;
    }
    pairing_ = std::make_unique<TransientPairing>();
    reply(control_, message, 200, pairing_->challenge());
  } else if (tlv.value(6) == QByteArray(1, char(3)) && pairing_) {
    TransientPairing::Result result;
    try {
      result = pairing_->verify(tlv.value(3), tlv.value(4));
    } catch (const std::exception &) {
      reply(control_, message, 200,
            tlvEncode(
                {{6, QByteArray(1, char(4))}, {7, QByteArray(1, char(2))}}));

      emit owner_.error(
          i18n::Message("Inbound control authentication rejected"));
      drainAndClose();
      return;
    }
    // Queue plaintext M4 before enabling encryption. Derive both channels
    // from the pairing secret, then cleanse it without retaining a root key.
    try {
      reply(control_, message, 200,
            tlvEncode({{6, QByteArray(1, char(4))}, {4, result.proof}}));
      control_.records = std::make_unique<HapRecords>(
          hkdf(result.key, "Control-Salt", "Control-Read-Encryption-Key"),
          hkdf(result.key, "Control-Salt", "Control-Write-Encryption-Key"));
      event_.records = std::make_unique<HapRecords>(
          hkdf(result.key, "Events-Salt", "Events-Write-Encryption-Key"),
          hkdf(result.key, "Events-Salt", "Events-Read-Encryption-Key"));
    } catch (...) {
      OPENSSL_cleanse(result.key.data(), size_t(result.key.size()));
      throw;
    }
    OPENSSL_cleanse(result.key.data(), size_t(result.key.size()));
    pairing_.reset();
    deadline_.start(15000);

    control_.wire = std::move(control_.plain);
    control_.plain = control_.records->decodeAvailable(control_.wire);
  } else
    reply(control_, message, 400);
}

void InboundControlConnection::setup(const ControlMessage &message,
                                     const std::optional<QVariant> &value) {
  if (!fairPlay_.complete()) {
    reply(control_, message, 455);
    return;
  }
  if (!value || value->typeId() != QMetaType::QVariantMap) {
    reply(control_, message, 400);
    return;
  }
  const auto fields = value->toMap();
  for (const auto &key :
       {"isRemoteControlOnly", "combinedGetInfoWithControlSetup",
        "updateSessionRequest"}) {
    if (fields.contains(key) && fields.value(key).typeId() != QMetaType::Bool) {
      reply(control_, message, 400);
      return;
    }
  }
  if (fields.contains("timingProtocol") &&
      fields.value("timingProtocol").typeId() != QMetaType::QString) {
    reply(control_, message, 400);
    return;
  }
  if (fields.contains("streams")) {
    if (!eventPort_) {
      reply(control_, message, 455);
      return;
    }
    const auto result =
        streams_.setup(fields.value("streams"), owner_.nextStream_);
    reply(control_, message, result.status,
          result.status == 200 ? plistEncode(result.body) : QByteArray{},
          "application/x-apple-binary-plist");

    return;
  }
  if (fields.value("isRemoteControlOnly") != QVariant(true) ||
      fields.value("timingProtocol") != QVariant(QString("None")) ||
      fields.value("updateSessionRequest").toBool()) {
    reply(control_, message, 501);
    return;
  }
  if (fields.contains("qualifier")) {
    const auto qualifier = fields.value("qualifier");
    if (qualifier.typeId() != QMetaType::QVariantList) {
      reply(control_, message, 400);
      return;
    }
    for (const auto &item : qualifier.toList())
      if (item.typeId() != QMetaType::QString) {
        reply(control_, message, 400);
        return;
      }
  }
  if (!eventPort_) {
    if (!eventServer_.listen(owner_.address_, 0))
      throw std::runtime_error("Event listener failed");
    owner_.route_.bindInterface(eventServer_.socketDescriptor());
    eventPort_ = eventServer_.serverPort();
  }
  QVariantMap response{{"eventPort", int(eventPort_)}};
  if (fields.value("combinedGetInfoWithControlSetup").toBool())
    response.insert("info", info());
  reply(control_, message, 200, plistEncode(response),
        "application/x-apple-binary-plist");
}

void InboundControlConnection::acceptEvent() {
  while (auto *socket = eventServer_.nextPendingConnection()) {
    if (closing_ || event_.socket ||
        socket->peerAddress() != control_.socket->peerAddress()) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    attach(event_, socket);

    // Exactly one event connection per control session; no counter reset or
    // socket descriptor sharing. Already-pending extras are rejected above.
    eventServer_.close();
    QTimer::singleShot(0, this, [this] {
      try {
        sendNextEvent();
        read(event_);
      } catch (const PublicationError &e) {
        finish(QString::fromUtf8(e.what()), true);
      } catch (const std::exception &e) {
        finish(QString::fromUtf8(e.what()));
      }
    });
  }
}
} // namespace airplay
