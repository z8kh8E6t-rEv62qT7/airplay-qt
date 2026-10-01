#include "PtpClock.h"
#include "app/Message.h"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <time.h>
#endif

namespace airplay {
int64_t wallNs() {
#ifdef Q_OS_WIN
  FILETIME time;
  GetSystemTimePreciseAsFileTime(&time);
  const uint64_t ticks =
      (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  return int64_t((ticks - 116444736000000000ULL) * 100);
#else
  timespec stamp{};
  if (clock_gettime(CLOCK_REALTIME, &stamp) != 0)
    throw Error(i18n::text(i18n::Id::CannotReadTheSystemClock));
  return int64_t(stamp.tv_sec) * 1000000000 + stamp.tv_nsec;
#endif
}
QByteArray ptpHeader(int kind, int length, uint64_t clock, uint16_t sequence,
                     int flags, int interval) {
  QByteArray out;
  out.append(char(0x10 | kind));
  out.append(char(2));
  appendBe(out, length, 2);
  out += QByteArray(2, '\0');
  appendBe(out, flags, 2);
  out += QByteArray(12, '\0');
  appendBe(out, clock, 8);
  appendBe(out, 0x8005, 2);
  appendBe(out, sequence, 2);
  out.append(char(kind == 12 ? 5 : 0));
  out.append(char(interval));
  return out;
}
QByteArray ptpTime(uint64_t ns) {
  QByteArray out;
  appendBe(out, ns / 1000000000, 6);
  appendBe(out, ns % 1000000000, 4);
  return out;
}
QByteArray syncPacket(uint64_t clock, uint32_t rtp, int64_t audible,
                      int64_t now, bool first) {
  // Split seconds to avoid overflowing int64_t during long continuous sessions.
  const int64_t delta = now - audible;
  const int64_t whole = delta / 1000000000;
  const int64_t rest = delta % 1000000000;
  int64_t fractional = rest * 44100 / 1000000000;
  if (rest < 0 && (rest * 44100) % 1000000000)
    --fractional; // Python's // floors negatives.
  const uint32_t frame = rtp + uint32_t(whole * 44100 + fractional) + 11035;
  QByteArray out;
  out.append(char(first ? 0x90 : 0x80));
  out.append(char(0xd7));
  appendBe(out, 6, 2);
  appendBe(out, frame, 4);
  appendBe(out, uint64_t(now), 8);
  appendBe(out, uint32_t(frame + 77175), 4);
  appendBe(out, clock, 8);
  return out;
}
PtpClock::PtpClock(QObject *parent) : QObject(parent) {
  timer_.setTimerType(Qt::PreciseTimer);
  timer_.setInterval(1);
  connect(&timer_, &QTimer::timeout, this, &PtpClock::tick);
  connect(&event_, &QUdpSocket::readyRead, this, [this] { receive(event_); });
  connect(&general_, &QUdpSocket::readyRead, this,
          [this] { receive(general_); });
}
void PtpClock::start(const QHostAddress &local,
                     const QList<QHostAddress> &hosts, uint64_t id,
                     const app::Timing &timing, const NetworkRoute &route) {
  stop();
  route_ = route;
  route_.validate();
  hosts_ = hosts;
  identity_ = id;
  timing_ = timing;
  sequence_ = 0;
  QNetworkInterface selected;
  for (const auto &iface : QNetworkInterface::allInterfaces())
    for (const auto &address : iface.addressEntries())
      if (address.ip() == local)
        selected = iface;
  if (!route.binding.automatic())
    selected = QNetworkInterface::interfaceFromIndex(int(route.index));
  if (!selected.isValid())
    throw Error(i18n::text(i18n::Id::CannotDetermineTheLocalPTPInterface));
  try {
    for (auto [socket, port] :
         {std::pair{&event_, 319}, std::pair{&general_, 320}}) {
      if (!route.binding.automatic())
        route.bind(*socket, quint16(port), true);
      else if (!socket->bind(QHostAddress::AnyIPv4, quint16(port),
                             QUdpSocket::DontShareAddress))
        throw Error(i18n::text(i18n::Id::PTPUDPBindingFailed)
                        .arg(port)
                        .arg(socket->errorString()));
      socket->setMulticastInterface(selected);
      if (!socket->joinMulticastGroup(QHostAddress("224.0.1.129"), selected))
        throw Error(i18n::text(i18n::Id::PTPMulticastJoinFailed) +
                    socket->errorString());
    }
    active_ = true;
    elapsed_.start();
    nextSync_ = nextAnnounce_ = 0;
    timer_.start();
    tick();
  } catch (...) {
    stop();
    throw;
  }
}
void PtpClock::stop() {
  active_ = false;
  timer_.stop();
  event_.close();
  general_.close();
}
void PtpClock::send(QUdpSocket &socket, quint16 port, const QByteArray &packet,
                    const QHostAddress &host) {
  for (const auto &receiver : hosts_)
    if (host.isNull() || receiver == host) {
      QNetworkDatagram datagram(packet, receiver, port);
      if (!route_.binding.automatic())
        datagram.setSender(route_.local, socket.localPort());
      if (socket.writeDatagram(datagram) != packet.size())
        throw Error(i18n::text(i18n::Id::PTPSendFailed) + socket.errorString());
    }
}
void PtpClock::tick() {
  if (!active_)
    return;
  try {
    const auto now = elapsed_.nsecsElapsed();
    if (now >= nextAnnounce_) {
      QByteArray body(42, '\0');
      const auto quality = QByteArray::fromHex("800621436a80");
      body.replace(13, 6, quality);
      QByteArray id;
      appendBe(id, identity_, 8);
      body.replace(19, 8, id);
      body[29] = char(0x20);
      QByteArray path;
      appendBe(path, 8, 2);
      appendBe(path, 8, 2);
      path += id;
      body.replace(30, 12, path);
      send(general_, 320,
           ptpHeader(11, 76, identity_, sequence_, 0x408,
                     int(std::log2(timing_.ptpAnnounce))) +
               body);
      nextAnnounce_ = now + qint64(timing_.ptpAnnounce * 1e9);
    }
    if (now >= nextSync_) {
      const int interval = int(std::log2(timing_.ptpSync));
      send(event_, 319,
           ptpHeader(0, 44, identity_, sequence_, 0x608, interval) +
               QByteArray(10, '\0'));
      const auto stamp = ptpTime(wallNs());
      QByteArray ieee =
          QByteArray::fromHex("0003001c0080c2000001") + QByteArray(22, '\0');
      QByteArray apple = QByteArray::fromHex("00030010000d93000004");
      appendBe(apple, identity_, 8);
      apple += QByteArray(2, '\0');
      send(general_, 320,
           ptpHeader(8, 96, identity_, sequence_, 0x408, interval) + stamp +
               ieee + apple);
      ++sequence_;
      nextSync_ = now + qint64(timing_.ptpSync * 1e9);
    }
  } catch (const std::exception &error) {
    stop();
    emit failed(i18n::fromException(error));
  }
}
void PtpClock::receive(QUdpSocket &socket) {
  if (!active_)
    return;
  try {
    for (int batch = 0; batch < 64 && socket.hasPendingDatagrams(); ++batch) {
      const auto datagram = socket.receiveDatagram(2048);
      const auto host = datagram.senderAddress();
      auto packet = datagram.data();
      if (!hosts_.contains(host) || packet.size() < 34 ||
          (uint8_t(packet[1]) & 15) != 2)
        continue;
      const int length = int(readBe(packet, 2, 2));
      if (length < 34 || length > packet.size())
        continue;
      packet.truncate(length);
      const int kind = uint8_t(packet[0]) & 15;
      const auto sequence = uint16_t(readBe(packet, 30, 2));
      if ((kind == 1 || kind == 2) && length >= 44) {
        send(kind == 1 ? general_ : event_, kind == 1 ? 320 : 319,
             ptpHeader(kind == 1 ? 9 : 3, 54, identity_, sequence, 0x608) +
                 ptpTime(wallNs()) + packet.mid(20, 10),
             host);
        if (kind == 2)
          send(general_, 320,
               ptpHeader(10, 54, identity_, sequence) + ptpTime(wallNs()) +
                   packet.mid(20, 10),
               host);
      } else if (kind == 12 && length >= 44) {
        QByteArray grants;
        int cursor = 44;
        while (cursor + 4 <= length && grants.size() < 96) {
          int tag = int(readBe(packet, cursor, 2)),
              size = int(readBe(packet, cursor + 2, 2));
          cursor += 4;
          if (cursor + size > length)
            break;
          if (tag == 4 && size >= 6)
            grants += QByteArray::fromHex("00050008") + packet.mid(cursor, 6) +
                      QByteArray::fromHex("0001");
          cursor += size;
        }
        if (!grants.isEmpty())
          send(general_, 320,
               ptpHeader(12, int(44 + grants.size()), identity_, sequence,
                         0x400, 127) +
                   packet.mid(20, 10) + grants,
               host);
      }
    }
    if (socket.hasPendingDatagrams())
      QTimer::singleShot(0, this, [this, &socket] { receive(socket); });
  } catch (const std::exception &error) {
    stop();
    emit failed(i18n::fromException(error));
  }
}
} // namespace airplay
