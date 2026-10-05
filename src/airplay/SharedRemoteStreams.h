#pragma once
#include "MrpSession.h"
#include <QMap>
#include <QSet>
#include <QVariantMap>
#include <limits>
#include <optional>
#include <stdexcept>

namespace airplay {
// Validates shared MediaRemote stream envelopes and owns isolated MRP state.
// Wire encoding lives in MrpSession.
class SharedRemoteStreams {
public:
  struct Result {
    int status;
    QVariantMap body;
  };
  Result setup(const QVariant &value, quint64 &nextId) {
    if (value.typeId() != QMetaType::QVariantList || value.toList().isEmpty())
      return {400, {}};
    const auto list = value.toList();
    if (list.size() > Limit || streams_.size() + list.size() > Limit ||
        nextId >
            quint64(std::numeric_limits<qint64>::max()) - quint64(list.size()))
      return {453, {}};
    for (const auto &item : list) {
      if (item.typeId() != QMetaType::QVariantMap)
        return {400, {}};
      const auto fields = item.toMap();
      if (fields.value("type").typeId() != QMetaType::LongLong ||
          fields.value("controlType").typeId() != QMetaType::LongLong)
        return {400, {}};
      for (const auto &key : {"clientTypeUUID", "clientUUID", "channelID"}) {
        const auto text = fields.value(key);
        if (text.typeId() != QMetaType::QString || text.toString().isEmpty() ||
            text.toString().size() > 256)
          return {400, {}};
      }
      for (const auto &key : {"wantsDedicatedSocket", "sendMessageAsIs"}) {
        if (fields.contains(key) &&
            fields.value(key).typeId() != QMetaType::Bool)
          return {400, {}};
        if (fields.value(key).toBool())
          return {501, {}};
      }
      if (fields.value("type").toLongLong() != 130 ||
          fields.value("controlType").toLongLong() != 2 ||
          fields.value("clientTypeUUID")
                  .toString()
                  .compare("1910A70F-DBC0-4242-AF95-115DB30604E1",
                           Qt::CaseInsensitive) != 0 ||
          fields.contains("seed"))
        return {501, {}};
    }
    // Validate the whole batch before allocating anything. IDs are monotonic
    // across server stop/start so an old ID cannot address a replacement
    // stream.
    QVariantList response;
    for (qsizetype i = 0; i < list.size(); ++i) {
      const auto id = ++nextId;
      streams_.insert(id, {});
      response.append(QVariantMap{{"type", 130}, {"streamID", qlonglong(id)}});
    }
    return {200, {{"streams", response}}};
  }
  int teardown(const QVariant &value) {
    if (value.typeId() != QMetaType::QVariantList || value.toList().isEmpty())
      return 400;
    const auto list = value.toList();
    if (list.size() > Limit)
      return 400;
    QSet<quint64> ids;
    for (const auto &item : list) {
      if (item.typeId() != QMetaType::QVariantMap)
        return 400;
      const auto fields = item.toMap();
      if (fields.value("type").typeId() != QMetaType::LongLong ||
          fields.value("streamID").typeId() != QMetaType::LongLong ||
          fields.value("streamID").toLongLong() <= 0)
        return 400;
      if (fields.value("type").toLongLong() != 130)
        return 501;
      const auto id = fields.value("streamID").toULongLong();
      if (!streams_.contains(id))
        return 454;
      if (ids.contains(id))
        return 400;
      ids.insert(id);
    }
    for (const auto id : ids)
      streams_.remove(id);
    return 200;
  }
  struct Message {
    int status;
    quint64 stream = 0;
    QByteArray data;
  };
  Message message(const QByteArray &header,
                  const std::optional<QVariant> &value) const {
    if (header.isEmpty() || header.size() > 20)
      return {400, 0, {}};
    for (const auto c : header)
      if (c < '0' || c > '9')
        return {400, 0, {}};
    bool valid = false;
    const auto id = header.toULongLong(&valid, 10);
    if (!valid || !id)
      return {400, 0, {}};
    if (!streams_.contains(id))
      return {454, 0, {}};
    if (!value || value->typeId() != QMetaType::QVariantMap)
      return {400, 0, {}};
    const auto params = value->toMap().value("params");
    if (params.typeId() != QMetaType::QVariantMap)
      return {400, 0, {}};
    const auto data = params.toMap().value("data");
    if (data.typeId() != QMetaType::QByteArray || data.toByteArray().isEmpty())
      return {400, 0, {}};
    return {200, id, data.toByteArray()};
  }
  void clear() { streams_.clear(); }
  bool contains(quint64 id) const { return streams_.contains(id); }
  QList<MrpSession::Exchange> receive(quint64 id, const QByteArray &data,
                                      const QByteArray &identity,
                                      const ControlState &state) {
    const auto it = streams_.find(id);
    if (it == streams_.end())
      throw std::runtime_error("Unknown MRP stream");
    return it->receive(data, identity, state);
  }
  QMap<quint64, QByteArray> updates(const QByteArray &identity,
                                    const ControlState &state) const {
    QMap<quint64, QByteArray> result;
    for (auto it = streams_.cbegin(); it != streams_.cend(); ++it) {
      const auto data = it->stateUpdate(identity, state, true);
      if (!data.isEmpty())
        result.insert(it.key(), data);
    }
    return result;
  }

private:
  static constexpr qsizetype Limit = 8;
  QMap<quint64, MrpSession> streams_;
};
} // namespace airplay
