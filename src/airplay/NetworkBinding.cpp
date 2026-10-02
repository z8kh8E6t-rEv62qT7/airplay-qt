#include "NetworkBinding.h"
#include "Crypto.h"
#include "app/Message.h"
#include <QJsonObject>
#include <QNetworkProxy>
#include <algorithm>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace airplay {
i18n::Message NetworkBinding::validate() const {
  if (automatic())
    return {};
  const QHostAddress address(ipv4);
  if (interfaceName.isEmpty() || interfaceName.size() > 1024 ||
      interfaceName.contains(QChar(0)) ||
      address.protocol() != QAbstractSocket::IPv4Protocol ||
      address.toString() != ipv4 || address == QHostAddress::AnyIPv4 ||
      address.isMulticast() || address == QHostAddress::Broadcast)
    return i18n::text(i18n::Id::InvalidNetworkBindingStructure);
  return {};
}
QJsonValue NetworkBinding::json() const {
  if (const auto error = validate(); !error.isEmpty())
    throw Error(error);
  if (automatic())
    return QJsonValue(QJsonValue::Null);
  return QJsonObject{{"interfaceName", interfaceName}, {"ipv4", ipv4}};
}
NetworkBinding NetworkBinding::fromJson(const QJsonValue &value) {
  if (value.isNull())
    return {};
  const auto object = value.toObject();
  if (!value.isObject() || !object.value("interfaceName").isString() ||
      !object.value("ipv4").isString())
    throw Error(
        i18n::text(i18n::Id::MissingOrInvalidNetworkBindingConfiguration));
  NetworkBinding result{object["interfaceName"].toString(),
                        object["ipv4"].toString()};
  if (result.automatic())
    throw Error(i18n::text(i18n::Id::AutomaticNetworkBindingMustBeNull));
  if (const auto error = result.validate(); !error.isEmpty())
    throw Error(error);
  return result;
}
QList<NetworkBinding> NetworkBinding::available() {
  QList<NetworkBinding> result;
  for (const auto &iface : QNetworkInterface::allInterfaces()) {
    if (!iface.isValid() || iface.index() <= 0 ||
        !iface.flags().testFlag(QNetworkInterface::IsUp) ||
        !iface.flags().testFlag(QNetworkInterface::IsRunning))
      continue;
    for (const auto &entry : iface.addressEntries()) {
      if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
        continue;
      NetworkBinding value{iface.name(), entry.ip().toString()};
      if (value.validate().isEmpty() && !result.contains(value))
        result.append(value);
    }
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    if (a.interfaceName != b.interfaceName)
      return a.interfaceName < b.interfaceName;
    return QHostAddress(a.ipv4).toIPv4Address() <
           QHostAddress(b.ipv4).toIPv4Address();
  });
  return result;
}
NetworkRoute NetworkRoute::resolve(const NetworkBinding &binding) {
  if (const auto error = binding.validate(); !error.isEmpty())
    throw Error(error);
  if (binding.automatic())
    return {};
  const auto iface =
      QNetworkInterface::interfaceFromName(binding.interfaceName);
  NetworkRoute result{binding, uint(iface.index()), QHostAddress(binding.ipv4)};
  result.validate();
  return result;
}
void NetworkRoute::validate() const {
  if (binding.automatic())
    return;
  const auto iface =
      QNetworkInterface::interfaceFromName(binding.interfaceName);
  bool found = false;
  for (const auto &entry : iface.addressEntries())
    found |= entry.ip() == local;
  if (!binding.validate().isEmpty() || local != QHostAddress(binding.ipv4) ||
      !index || !iface.isValid() || uint(iface.index()) != index ||
      !iface.flags().testFlag(QNetworkInterface::IsUp) ||
      !iface.flags().testFlag(QNetworkInterface::IsRunning) || !found)
    throw Error(
        i18n::text(i18n::Id::SelectedNetworkInterfaceOrIPvIsUnavailable) +
        binding.interfaceName + " · " + binding.ipv4 +
        i18n::text(i18n::Id::StopAndSelectItAgainNoOther));
}
void NetworkRoute::bind(QAbstractSocket &socket, quint16 port,
                        bool multicast) const {
  validate();
  if (!binding.automatic())
    socket.setProxy(QNetworkProxy::NoProxy);
  const auto address =
      multicast || local.isNull() ? QHostAddress(QHostAddress::AnyIPv4) : local;
  if (!socket.bind(address, port, QAbstractSocket::DontShareAddress)) {
    const auto error = socket.errorString();
    socket.abort();
    throw Error(i18n::text(i18n::Id::LocalAddressBindingFailed) + error);
  }
  try {
    bindInterface(socket.socketDescriptor());
  } catch (...) {
    socket.abort();
    throw;
  }
}
void NetworkRoute::bindInterface(qintptr descriptor) const {
  validate();
  if (binding.automatic())
    return;
#ifdef Q_OS_WIN
  const DWORD option = htonl(index);
  const int result =
      setsockopt(SOCKET(descriptor), IPPROTO_IP, IP_UNICAST_IF,
                 reinterpret_cast<const char *>(&option), sizeof(option));
  const auto error = QString::number(WSAGetLastError());
#elif defined(Q_OS_LINUX)
  // Each socket is bound once; changing the interface requires a new socket.
  const auto name = binding.interfaceName.toLocal8Bit();
  char current[256]{};
  socklen_t length = sizeof(current);
  if (getsockopt(int(descriptor), SOL_SOCKET, SO_BINDTODEVICE, current, &length) == 0 &&
      name == current)
    return; // Accepted TCP sockets inherit the listener's interface.
  const int result = setsockopt(int(descriptor), SOL_SOCKET, SO_BINDTODEVICE,
                               name.constData(), socklen_t(name.size() + 1));
  const auto error = QString::fromLocal8Bit(std::strerror(errno));
#else
  const int option = int(index);
  const int result = setsockopt(int(descriptor), IPPROTO_IP, IP_BOUND_IF,
                                &option, sizeof(option));
  const auto error = QString::fromLocal8Bit(std::strerror(errno));
#endif
  if (result != 0) {
    throw Error(i18n::text(i18n::Id::SendingInterfaceBindingFailed) + error);
  }
}
} // namespace airplay
