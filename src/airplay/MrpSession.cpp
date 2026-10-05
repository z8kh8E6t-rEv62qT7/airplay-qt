#include "MrpSession.h"
#include <QCoreApplication>
#include <QString>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace airplay {
namespace {
constexpr qsizetype FrameLimit = 65536;
[[noreturn]] void invalid() {
  throw std::runtime_error("Invalid or excessive MRP frame");
}
// Protobuf wire format: protobuf.dev/programming-guides/encoding/.
// MRP field/type numbers: pyatv ProtocolMessage/DeviceInfoMessage schemas.
// No protobuf runtime dependency; groups are intentionally unsupported.
std::optional<quint64> readVarint(const QByteArray &data, qsizetype &pos) {
  quint64 value = 0;
  for (int i = 0; i < 10; ++i) {
    if (pos == data.size())
      return {};
    const auto byte = quint8(data[pos++]);
    if (i == 9 && byte > 1)
      invalid();
    value |= quint64(byte & 127) << (7 * i);
    if (!(byte & 128))
      return value;
  }
  invalid();
}
quint64 requiredVarint(const QByteArray &data, qsizetype &pos) {
  auto value = readVarint(data, pos);
  if (!value)
    invalid();
  return *value;
}
struct Field {
  quint32 tag;
  int wire;
  quint64 number = 0;
  QByteArray bytes;
};
QList<Field> parse(const QByteArray &data) {
  QList<Field> fields;
  qsizetype pos = 0;
  while (pos < data.size()) {
    if (fields.size() == 1024)
      invalid();
    const auto key = requiredVarint(data, pos);
    if (!(key >> 3) || (key >> 3) > 0x1fffffff)
      invalid();
    Field field{quint32(key >> 3), int(key & 7), 0, {}};
    if (field.wire == 0)
      field.number = requiredVarint(data, pos);
    else {
      quint64 size;
      if (field.wire == 2)
        size = requiredVarint(data, pos);
      else if (field.wire == 1)
        size = 8;
      else if (field.wire == 5)
        size = 4;
      else
        invalid();
      if (size > quint64(data.size() - pos))
        invalid();
      field.bytes = data.mid(pos, qsizetype(size));
      pos += qsizetype(size);
    }
    fields.append(std::move(field));
  }
  return fields;
}
const Field *single(const QList<Field> &fields, quint32 tag, int wire,
                    bool required = false) {
  const Field *found = nullptr;
  for (const auto &field : fields) {
    if (field.tag != tag)
      continue;
    if (found || field.wire != wire)
      invalid();
    found = &field;
  }
  if (required && !found)
    invalid();
  return found;
}
QByteArray text(const Field *field, qsizetype limit) {
  if (!field)
    return {};
  if (field->bytes.size() > limit || field->bytes.contains('\0') ||
      QString::fromUtf8(field->bytes).toUtf8() != field->bytes)
    invalid();
  return field->bytes;
}
QByteArray varint(quint64 value) {
  QByteArray result;
  do {
    const auto byte = quint8(value & 127);
    value >>= 7;
    result.append(char(byte | (value ? 128 : 0)));
  } while (value);
  return result;
}
QByteArray number(quint32 tag, quint64 value) {
  return varint(quint64(tag) << 3) + varint(value);
}
QByteArray bytes(quint32 tag, const QByteArray &value) {
  return varint((quint64(tag) << 3) | 2) + varint(quint64(value.size())) +
         value;
}
QByteArray fixed32(quint32 tag, quint32 value) {
  QByteArray raw(4, '\0');
  raw[0] = char(value);
  raw[1] = char(value >> 8);
  raw[2] = char(value >> 16);
  raw[3] = char(value >> 24);
  return varint((quint64(tag) << 3) | 5) + raw;
}
QByteArray floatField(quint32 tag, float value) {
  quint32 bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return fixed32(tag, bits);
}
QByteArray response(quint32 type, const QByteArray &id, int error,
                    const QByteArray &extra = {}) {
  auto message = number(1, type);
  if (!id.isEmpty())
    message += bytes(2, id);
  message +=
      number(4, quint64(error)) + extra +
      bytes(85, QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
  return varint(quint64(message.size())) + message;
}
constexpr auto Bundle = "org.airplayqt.app"; // Same player as the audio
                                             // session's now-playing metadata.
constexpr auto Player = "MediaRemote-DefaultPlayer";
QByteArray outputUID(const ControlOutput &output) {
  return output.aliases.isEmpty() ? output.id : output.aliases.first();
}
QByteArray client() {
  return number(1, quint64(QCoreApplication::applicationPid())) +
         bytes(2, Bundle) + bytes(7, "AirPlayQt");
}
QByteArray playerPath() {
  return bytes(2, client()) +
         bytes(3, bytes(1, Player) + bytes(2, "AirPlayQt") + number(3, 1));
}
QByteArray supported(bool available) {
  QByteArray out;
  for (int command : {1, 2, 3})
    out += bytes(1, number(1, command) + number(2, available));
  return out;
}
QByteArray deviceInfo(const QByteArray &identity, const ControlState &state) {
  auto out = bytes(1, identity) + bytes(2, "AirPlayQt") + bytes(3, "Computer") +
             bytes(5, "org.airplayqt.app") + number(7, 1) + number(8, 72) +
             number(9, 0) + number(10, 0) + number(13, 0) + number(14, 0) +
             bytes(19, identity) + number(21, 9) + number(22, 1) +
             number(30, state.available);
  if (!state.group.isEmpty())
    out += bytes(26, state.group) + bytes(27, "AirPlayQt") +
           bytes(32, state.group) + bytes(42, state.group);
  for (const auto &output : state.outputs) {
    // Actual selected receivers, not a fabricated HomePod cluster descriptor.
    out += bytes(28, bytes(2, output.name) + bytes(19, outputUID(output)));
    out += bytes(33, outputUID(output));
  }
  return out;
}
QByteArray setState(const ControlState &state,
                    const QByteArray &identifier = {}) {
  const float rate =
      state.available && state.playback == PlaybackState::Playing ? 1.f : 0.f;
  const auto title = QByteArray("AirPlayQt");
  const auto artist = QStringLiteral("实时音频").toUtf8();
  const auto info = bytes(2, artist) + floatField(5, rate) + bytes(9, title) +
                    number(10, 1) + number(17, 1);
  const auto metadata = bytes(1, title) + number(4, state.available) +
                        bytes(7, artist) + number(26, 1) + number(37, 1) +
                        floatField(39, rate) + floatField(59, 1) +
                        number(64, 1) + number(65, 1);
  const auto queue =
      number(1, 0) + bytes(2, bytes(1, "airplayqt-live") + bytes(2, metadata));
  const auto value = bytes(1, info) + bytes(2, supported(state.available)) +
                     bytes(3, queue) + bytes(5, "AirPlayQt") +
                     number(6, int(state.available ? state.playback
                                                   : PlaybackState::Stopped)) +
                     bytes(9, playerPath());
  return response(4, identifier, 0, bytes(9, value));
}
float normalizedVolume(const ControlState &state) {
  return state.volumeDb <= -144
             ? 0.f
             : float(std::clamp((state.volumeDb + 30.) / 30., 0., 1.));
}
QByteArray volumeMessages(const QByteArray &identity,
                          const ControlState &state) {
  const auto capability = number(1, state.available) +
                          number(2, state.available ? 2 : 0); // Absolute only.
  auto out = response(17, {}, 0, bytes(22, capability));
  QList<QByteArray> ids{identity};
  if (!state.group.isEmpty())
    ids.append(state.group);
  for (const auto &device : state.outputs)
    ids.append(outputUID(device));
  for (const auto &id : ids) {
    out += response(
        64, {}, 0,
        bytes(68, bytes(1, capability) + bytes(3, identity) + bytes(4, id)));
    out += response(52, {}, 0,
                    bytes(56, floatField(1, normalizedVolume(state)) +
                                  bytes(2, identity) + bytes(3, id)));
  }
  return out;
}
QByteArray outputMessages(const QByteArray &identity,
                          const ControlState &state) {
  auto devices = bytes(2, identity);
  for (const auto &device : state.outputs) {
    const auto descriptor =
        bytes(1, device.name) + bytes(2, outputUID(device)) +
        bytes(3, state.group) + number(7, state.available) + number(9, 0) +
        number(10, 1) + number(14, 0) +
        floatField(24, normalizedVolume(state)) + number(25, state.available) +
        number(33, state.available) + number(37, state.available ? 2 : 0) +
        bytes(51, state.group);
    devices += bytes(1, descriptor);
  }
  return response(65, {}, 0, bytes(69, devices));
}
bool knownOutput(const QByteArray &id, const QByteArray &identity,
                 const ControlState &state) {
  if (id.isEmpty() || id == identity ||
      (!state.group.isEmpty() && id == state.group))
    return true;
  for (const auto &device : state.outputs)
    if (id == device.id || device.aliases.contains(id))
      return true;
  return false;
}
bool knownPlayer(const Field *path) {
  if (!path)
    return true; // Current player.
  const auto fields = parse(path->bytes);
  if (const auto origin = single(fields, 1, 2)) {
    const auto fields = parse(origin->bytes);
    if (const auto type = single(fields, 1, 0);
        type && type->number != 0 && type->number != 1)
      return false;
    if (const auto id = single(fields, 3, 0); id && id->number != 0)
      return false;
  }
  if (const auto clientField = single(fields, 2, 2)) {
    const auto fields = parse(clientField->bytes);
    const auto bundle = text(single(fields, 2, 2), 256);
    if (!bundle.isEmpty() && bundle != Bundle)
      return false;
    if (const auto pid = single(fields, 1, 0);
        pid && pid->number != quint64(QCoreApplication::applicationPid()))
      return false;
  }
  if (const auto player = single(fields, 3, 2)) {
    const auto id = text(single(parse(player->bytes), 1, 2), 256);
    if (!id.isEmpty() && id != Player)
      return false;
  }
  return true;
}
bool supportedOptions(const Field *options) {
  if (!options)
    return true;
  const auto fields = parse(options->bytes);
  // These fields describe the caller, not a different queue/position/rate.
  for (const auto &field : fields) {
    if (field.tag == 2 || field.tag == 3 || field.tag == 19) {
      text(single(fields, field.tag, 2), 256);
    } else if (field.tag == 4) {
      if (single(fields, 4, 0)->number > 1)
        invalid();
    } else if (field.tag == 16) {
      const auto destination = text(single(fields, 16, 2), 256);
      if (!destination.isEmpty() && destination != Bundle)
        return false;
    } else if (field.tag == 17) {
      if (single(fields, 17, 0)->number)
        return false;
    } else
      return false;
  }
  return true;
}
QByteArray commandResult(const QByteArray &id, int error) {
  const auto sendError = error ? (error == 6 ? 10 : 2) : 0;
  return response(2, id, error,
                  bytes(7, number(1, sendError) + number(2, error ? 2 : 0)));
}
} // namespace
QList<MrpSession::Exchange> MrpSession::receive(const QByteArray &data,
                                                const QByteArray &identity,
                                                const ControlState &state) {
  if (identity.isEmpty() || identity.size() > 256 ||
      data.size() > FrameLimit + 10 - pending_.size())
    invalid();
  auto input = pending_ + data;
  auto next = *this;
  qsizetype consumed = 0;
  QList<Exchange> result;
  while (consumed < input.size()) {
    auto pos = consumed;
    const auto length = readVarint(input, pos);
    if (!length)
      break;
    if (!*length || *length > quint64(FrameLimit))
      invalid();
    if (*length > quint64(input.size() - pos))
      break;
    if (result.size() == 32)
      invalid();
    const auto fields = parse(input.mid(pos, qsizetype(*length)));
    const auto typeValue = single(fields, 1, 0, true)->number;
    if (typeValue > 0x7fffffff)
      invalid();
    const auto type = quint32(typeValue);
    const auto id = text(single(fields, 2, 2), 256);
    Exchange exchange{type, {}, 0, id, {}, {}};
    if (type == 15 || (type == 37 && next.introduced_)) {
      const auto info = parse(single(fields, 20, 2, true)->bytes);
      if (text(single(info, 2, 2, true), 1024).isEmpty() ||
          (type == 15 && id.isEmpty()))
        invalid();
      exchange.deviceUID = text(single(info, 19, 2), 256);
      if (type == 15)
        exchange.response =
            response(15, id, 0, bytes(20, deviceInfo(identity, state)));
      // Updates change peer information; they are not a new handshake.
      next.introduced_ = true;
    } else if (type == 0) {
      // Never reply to generic acknowledgements.
    } else if (!next.introduced_)
      exchange.errorCode = 2;
    else if (type == 38) {
      const auto connection = parse(single(fields, 42, 2, true)->bytes);
      if (single(connection, 1, 0, true)->number > 3)
        invalid();
    } else if (type == 16) {
      const auto config = parse(single(fields, 21, 2, true)->bytes);
      for (quint32 tag = 1; tag <= 5; ++tag) {
        const auto field = single(config, tag, 0);
        if (field && field->number > 1)
          invalid();
      }
      const auto enabled = [&](quint32 tag) {
        const auto f = single(config, tag, 0);
        return f && f->number;
      };
      next.subscribed_ = true;
      next.nowPlayingUpdates_ = enabled(2);
      next.volumeUpdates_ = enabled(3);
      next.outputUpdates_ = enabled(5);
      // Initial snapshot identifies the player even when ongoing now-playing
      // updates are disabled. This matches the reference server's subscription
      // flow.
      exchange.response = next.stateUpdate(identity, state, true);
      if (!id.isEmpty())
        exchange.response += response(0, id, 0);
    } else if (type == 3 || type == 32)
      exchange.response = setState(state, id);
    else if (type == 1) {
      const auto command = parse(single(fields, 6, 2, true)->bytes);
      const auto code = single(command, 1, 0, true)->number;
      if (!supportedOptions(single(command, 2, 2))) {
        exchange.errorCode =
            2; // Do not silently ignore options with different semantics.
      } else if (!knownPlayer(single(command, 3, 2)))
        exchange.errorCode = 35;
      else if (code < 1 || code > 3)
        exchange.errorCode = 6;
      else if (!state.available)
        exchange.errorCode = 2;
      else
        exchange.request = ControlRequest{code == 1   ? ControlAction::Play
                                          : code == 2 ? ControlAction::Pause
                                                      : ControlAction::Toggle};
    } else if (type == 49 || type == 51) {
      const auto volume =
          parse(single(fields, type == 49 ? 53 : 55, 2, true)->bytes);
      const auto uid = text(single(volume, type == 49 ? 1 : 2, 2), 256);
      if (!knownOutput(uid, identity, state))
        exchange.errorCode = 39;
      else if (!state.available)
        exchange.errorCode = 2;
      else if (type == 49)
        exchange.response = response(
            50, id, 0, bytes(54, floatField(1, normalizedVolume(state))));
      else {
        const auto raw = single(volume, 1, 5, true)->bytes;
        quint32 bits = quint8(raw[0]) | (quint32(quint8(raw[1])) << 8) |
                       (quint32(quint8(raw[2])) << 16) |
                       (quint32(quint8(raw[3])) << 24);
        float normalized = 0;
        std::memcpy(&normalized, &bits, sizeof normalized);
        if (!std::isfinite(normalized) || normalized < 0 || normalized > 1)
          exchange.errorCode = 18;
        else
          exchange.request =
              ControlRequest{ControlAction::Volume,
                             normalized <= 0 ? -144. : -30. + 30. * normalized};
      }
    } else if (type == 24) {
      if (!id.isEmpty())
        exchange.response = response(23, id, 0);
    } else if (type == 42) {
      if (!id.isEmpty())
        exchange.response = response(0, id, 0); // Heartbeat.
    } else
      exchange.errorCode = 6;
    if (exchange.errorCode && !id.isEmpty())
      exchange.response = type == 1 ? commandResult(id, exchange.errorCode)
                                    : response(0, id, exchange.errorCode);
    result.append(std::move(exchange));
    consumed = pos + qsizetype(*length);
  }
  next.pending_ = input.mid(consumed);
  *this = std::move(next);
  return result;
}
QByteArray MrpSession::stateUpdate(const QByteArray &identity,
                                   const ControlState &state,
                                   bool initial) const {
  if (!introduced_ || !subscribed_ || state.outputs.isEmpty())
    return {};
  QByteArray out;
  if (initial || outputUpdates_)
    out += response(37, {}, 0, bytes(20, deviceInfo(identity, state))) +
           outputMessages(identity, state);
  if (initial || nowPlayingUpdates_) {
    out += response(46, {}, 0, bytes(50, bytes(1, client())));
    out += response(47, {}, 0, bytes(51, bytes(1, playerPath())));
    out += setState(state);
    out += response(72, {}, 0,
                    bytes(75, bytes(2, supported(state.available)) +
                                  bytes(9, playerPath())));
  }
  if (initial || volumeUpdates_)
    out += volumeMessages(identity, state);
  return out;
}
QByteArray MrpSession::complete(const Exchange &request, bool success) {
  if (request.identifier.isEmpty())
    return {};
  return request.type == 1 ? commandResult(request.identifier, success ? 0 : 1)
                           : response(0, request.identifier, success ? 0 : 1);
}
QList<QByteArray> MrpSession::outgoingFrames(const QByteArray &data) {
  if (data.size() > FrameLimit)
    invalid();
  QList<QByteArray> frames;
  qsizetype pos = 0;
  while (pos < data.size()) {
    const auto start = pos;
    const auto length = requiredVarint(data, pos);
    if (!length || length > quint64(data.size() - pos) || frames.size() == 32)
      invalid();
    pos += qsizetype(length);
    frames.append(data.mid(start, pos - start));
  }
  return frames;
}
} // namespace airplay
