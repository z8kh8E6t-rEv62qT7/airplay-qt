#pragma once
#include <QHostAddress>
#include <QList>
#include <QString>

namespace airplay {
struct ReceiverEndpoint {
  QHostAddress host;
  quint16 port = 7000;
  QString text() const;
  bool operator==(const ReceiverEndpoint &) const = default;
};
// Strict dotted-decimal IPv4, optional decimal port. Throws Error on invalid
// input.
ReceiverEndpoint parseReceiverEndpoint(const QString &text);
void validateEndpoints(const QList<ReceiverEndpoint> &endpoints);
} // namespace airplay
