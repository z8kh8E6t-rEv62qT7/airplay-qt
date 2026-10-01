#include "ReceiverEndpoint.h"
#include "Crypto.h"
#include "app/Message.h"

namespace airplay {
QString ReceiverEndpoint::text() const {
  return host.toString() + ':' + QString::number(port);
}
void validateEndpoints(const QList<ReceiverEndpoint> &endpoints) {
  if (endpoints.size() < 1 || endpoints.size() > 2)
    throw Error(i18n::text(i18n::Id::SelectOneOrTwoReceivers));
  for (const auto &endpoint : endpoints) {
    if (endpoint.host.protocol() != QAbstractSocket::IPv4Protocol ||
        endpoint.host.toIPv4Address() == 0 ||
        endpoint.host.toIPv4Address() == 0xffffffffU ||
        endpoint.host.isMulticast() || endpoint.port == 0)
      throw Error(
          i18n::text(i18n::Id::ReceiverRequiresAValidUnicastIPvAddress));
  }
  if (endpoints.size() == 2 && endpoints[0] == endpoints[1])
    throw Error(i18n::text(i18n::Id::ReceiverAddressAndPortMustNotBe));
}
ReceiverEndpoint parseReceiverEndpoint(const QString &text) {
  const auto parts = text.trimmed().split(':');
  if (parts.size() < 1 || parts.size() > 2)
    throw Error(i18n::text(i18n::Id::UseTheAddressFormatIPvPort));
  const auto octets = parts[0].split('.');
  if (octets.size() != 4)
    throw Error(i18n::text(i18n::Id::EnterACompleteIPvAddress));
  auto decimal = [](const QString &value, uint maximum) {
    if (value.isEmpty() || value.size() > 5)
      throw Error(i18n::text(i18n::Id::InvalidAddressOrPort));
    for (QChar c : value)
      if (c < u'0' || c > u'9')
        throw Error(
            i18n::text(i18n::Id::AddressAndPortAcceptDecimalDigitsOnly));
    bool ok = false;
    const auto number = value.toUInt(&ok, 10);
    if (!ok || number > maximum)
      throw Error(i18n::text(i18n::Id::AddressOrPortIsOutOfRange));
    return number;
  };
  quint32 address = 0;
  for (const auto &octet : octets)
    address = (address << 8) | decimal(octet, 255);
  ReceiverEndpoint endpoint{
      QHostAddress(address),
      quint16(parts.size() == 2 ? decimal(parts[1], 65535) : 7000)};
  validateEndpoints({endpoint});
  return endpoint;
}
} // namespace airplay
