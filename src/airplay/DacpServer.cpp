#include "DacpServer.h"
#include "NowPlaying.h"
#include "app/Message.h"
#include <QNetworkProxy>
#include <QUrl>
#include <QUrlQuery>
#include <cmath>

namespace airplay {
DacpServer::DacpServer(QObject *parent) : QObject(parent) {
  server_.setProxy(QNetworkProxy::NoProxy);
  server_.setMaxPendingConnections(8);
  connect(&server_, &QTcpServer::newConnection, this, &DacpServer::accept);
  connect(&advertisement_, &ServiceAdvertisement::failed, this,
          &DacpServer::failed);
  connect(&advertisement_, &ServiceAdvertisement::ready, this,
          &DacpServer::ready);
}
DacpServer::~DacpServer() { stop(); }
void DacpServer::start(const QString &identity, const QHostAddress &local,
                       const NetworkRoute &route,
                       const QList<QHostAddress> &peers, bool advertise,
                       quint32 activeRemote) {
  stop();
  route.validate();
  peers_ = peers;
  route_ = route;
  activeRemote_ = QByteArray::number(activeRemote);
  if (local.isNull() || !server_.listen(local, 0))
    throw Error(i18n::text(i18n::Id::DACPListenFailed) + server_.errorString());
  try {
    route_.bindInterface(server_.socketDescriptor());
  } catch (...) {
    server_.close();
    throw;
  }
  // Auto mode also advertises only where the selected source address lives.
  auto publicationRoute = route;
  if (publicationRoute.binding.automatic())
    for (const auto &binding : NetworkBinding::available())
      if (QHostAddress(binding.ipv4) == local) {
        publicationRoute = NetworkRoute::resolve(binding);
        break;
      }
  if (advertise) {
    if (publicationRoute.local.isNull())
      throw Error(i18n::text(i18n::Id::NoNetworkInterfaceMatchesTheDACPSource));
    advertisement_.start(identity, server_.serverPort(), publicationRoute);
  } else {
    const auto generation = generation_;
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == generation_ && server_.isListening())
        emit ready();
    });
  }
}
void DacpServer::stop() {
  ++generation_;
  enabled_ = false;
  advertisement_.stop();
  server_.close();
  for (auto *client : std::as_const(clients_)) {
    client->disconnect(this);
    client->abort();
    client->deleteLater();
  }
  clients_.clear();
  peers_.clear();
}
void DacpServer::accept() {
  while (server_.hasPendingConnections()) {
    auto *client = server_.nextPendingConnection();
    if (clients_.size() >= 8 || !peers_.contains(client->peerAddress())) {
      client->abort();
      client->deleteLater();
      continue;
    }
    clients_.insert(client);
    try {
      route_.bindInterface(client->socketDescriptor());
    } catch (const std::exception &e) {
      clients_.remove(client);
      client->abort();
      client->deleteLater();
      emit failed(i18n::fromException(e));
      return;
    }
    client->setReadBufferSize(16384);
    auto buffer = std::make_shared<QByteArray>();
    auto *deadline = new QTimer(client);
    deadline->setSingleShot(true);
    deadline->start(3000);
    connect(deadline, &QTimer::timeout, client, &QTcpSocket::abort);
    connect(client, &QTcpSocket::disconnected, this, [this, client] {
      clients_.remove(client);
      client->deleteLater();
    });
    connect(client, &QTcpSocket::readyRead, this, [this, client, buffer] {
      try {
        *buffer += client->readAll();
        if (buffer->size() > 16384)
          throw Error(i18n::text(i18n::Id::DACPRequestIsTooLarge));
        auto request = parseControlMessage(*buffer);
        if (!request)
          return;
        QByteArray body;
        const int status = handle(*client, *request, body);
        auto response =
            "HTTP/1.1 " + QByteArray::number(status) +
            " Result\r\nContent-Length: " + QByteArray::number(body.size()) +
            "\r\nConnection: close\r\n";
        if (!body.isEmpty())
          response += "Content-Type: application/x-dmap-tagged\r\n";
        response += "\r\n" + body;
        if (status != 200 && status != 204)
          emit log(i18n::text(i18n::Id::DACPRequestNotExecutedStatus)
                       .arg(QString::fromLatin1(request->line.left(256)))
                       .arg(status));
        client->write(response);
        client->disconnectFromHost();
      } catch (const std::exception &e) {
        emit log(i18n::text(i18n::Id::DACPRequestRejected) +
                 i18n::fromException(e));
        client->abort();
      }
    });
  }
}
int DacpServer::handle(QTcpSocket &socket, const ControlMessage &request,
                       QByteArray &body) {
  if (!enabled_)
    return 503;
  const auto parts = request.line.split(' ');
  if (parts.size() != 3 || parts[0] != "GET" ||
      (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0") ||
      !request.body.isEmpty())
    return 400;
  // Both the receiver address and the current session token must match.
  if (request.headers.value("active-remote") != activeRemote_)
    return 403;
  const auto url = QUrl::fromEncoded(parts[1], QUrl::StrictMode);
  if (!url.isValid() || !url.isRelative() || url.hasFragment())
    return 400;
  const auto prefix = QStringLiteral("/ctrl-int/1/");
  if (!url.path().startsWith(prefix))
    return 404;
  const auto action = url.path().mid(prefix.size());
  if (action == "getproperty") {
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (items.size() != 1 || items[0].first != "properties")
      return 400;
    const auto names = items[0].second.split(',');
    if (names.isEmpty() || names.size() > 16)
      return 400;
    for (const auto &name : names)
      if (name != "dmcp.volume" && name != "dacp.volumecontrollable")
        return 501;
    body = volumeProperties(volume_, names.contains("dmcp.volume"),
                            names.contains("dacp.volumecontrollable"));
    return 200;
  }
  QString operation;
  double value = 0;
  if (action == "volumeup" || action == "volumedown" ||
      action == "mutetoggle") {
    if (url.hasQuery())
      return 400;
    operation = action;
  } else if (action.startsWith("devicevolume=")) {
    bool valid = false;
    value = action.mid(13).toDouble(&valid);
    if (url.hasQuery() || !valid || !std::isfinite(value) || value < -144 ||
        value > 0)
      return 400;
    operation = "absolute";
  } else if (action == "setproperty") {
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    if (items.size() != 1)
      return 400;
    bool valid = false;
    value = items[0].second.toDouble(&valid);
    if (!valid || !std::isfinite(value))
      return 400;
    if (items[0].first == "dmcp.device-volume") {
      if (value < -144 || value > 0)
        return 400;
      operation = "absolute";
    } else if (items[0].first == "dmcp.volume") {
      if (value < 0 || value > 100)
        return 400;
      value = value == 0 ? -144 : -30 + value * .3;
      operation = "absolute";
    } else {
      return 501;
    }
  } else if (action == "play" || action == "pause" ||
             action == "playpause" || action == "playresume") {
    if (url.hasQuery())
      return 400;
    operation = action == "playresume" ? "play" : action;
  } else {
    return 501;
  }
  emit log(
      i18n::Message("DACP: ") + socket.peerAddress().toString() + " · " +
      operation +
      (operation == "absolute" ? QString(" %1 dB").arg(value) : QString{}));
  emit command(socket.peerAddress().toString(), operation, value);
  return 204;
}
} // namespace airplay
