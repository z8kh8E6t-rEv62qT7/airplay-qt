#pragma once
#include <QAbstractSocket>
#include <QJsonValue>
#include <QNetworkInterface>

namespace airplay {
struct NetworkBinding {
  QString interfaceName, ipv4;
  bool automatic() const { return interfaceName.isEmpty() && ipv4.isEmpty(); }
  bool operator==(const NetworkBinding &) const = default;
  QString validate() const;
  QJsonValue json() const;
  static NetworkBinding fromJson(const QJsonValue &);
  static QList<NetworkBinding> available();
};
// A session resolves once. Validation never substitutes a new index or address.
struct NetworkRoute {
  NetworkBinding binding;
  uint index = 0;
  QHostAddress local;
  static NetworkRoute resolve(const NetworkBinding &);
  void validate() const;
  void bind(QAbstractSocket &, quint16 port = 0,
            bool receiveMulticast = false) const;
};
} // namespace airplay
