#include "ReceiverEndpoint.h"
#include "Crypto.h"

namespace airplay {
QString ReceiverEndpoint::text() const {
  return host.toString() + ':' + QString::number(port);
}
void validateEndpoints(const QList<ReceiverEndpoint> &endpoints) {
  if (endpoints.size() < 1 || endpoints.size() > 2)
    throw Error("请选择一台或两台接收端");
  for (const auto &endpoint : endpoints) {
    if (endpoint.host.protocol() != QAbstractSocket::IPv4Protocol ||
        endpoint.host.toIPv4Address() == 0 ||
        endpoint.host.toIPv4Address() == 0xffffffffU ||
        endpoint.host.isMulticast() || endpoint.port == 0)
      throw Error("接收端必须为有效 IPv4 单播地址及 1～65535 端口");
  }
  if (endpoints.size() == 2 && endpoints[0] == endpoints[1])
    throw Error("接收端地址和端口不能重复");
}
ReceiverEndpoint parseReceiverEndpoint(const QString &text) {
  const auto parts = text.trimmed().split(':');
  if (parts.size() < 1 || parts.size() > 2)
    throw Error("地址格式应为 IPv4[:端口]");
  const auto octets = parts[0].split('.');
  if (octets.size() != 4)
    throw Error("请输入完整 IPv4 地址");
  auto decimal = [](const QString &value, uint maximum) {
    if (value.isEmpty() || value.size() > 5)
      throw Error("地址或端口无效");
    for (QChar c : value)
      if (c < u'0' || c > u'9')
        throw Error("地址和端口仅接受十进制数字");
    bool ok = false;
    const auto number = value.toUInt(&ok, 10);
    if (!ok || number > maximum)
      throw Error("地址或端口超出范围");
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
