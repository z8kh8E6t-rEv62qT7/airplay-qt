#include "PipeWireCatalog.h"
#include <QCoreApplication>
#include <QPointer>
#include <spa/param/audio/format-utils.h>
#include <algorithm>
#include <cstring>

namespace audio {
namespace {
QVariantMap properties(const spa_dict *dict) {
  QVariantMap result;
  if (dict) {
    const spa_dict_item *item;
    spa_dict_for_each(item, dict)
      result.insert(QString::fromUtf8(item->key), QString::fromUtf8(item->value));
  }
  return result;
}
}
struct PipeWireCatalog::Node {
  pw_node *proxy = nullptr;
  spa_hook listener{};
  QVariantMap props;
  uint32_t rate = 0, channels = 0;
  ~Node() {
    if (proxy) { spa_hook_remove(&listener); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy)); }
  }
  static void info(void *data, const pw_node_info *info) {
    auto &self = *static_cast<Node *>(data);
    if (info->props) self.props.insert(properties(info->props));
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS)
      for (uint32_t i = 0; i < info->n_params; ++i)
        if (info->params[i].id == SPA_PARAM_Format && (info->params[i].flags & SPA_PARAM_INFO_READ))
          pw_node_enum_params(self.proxy, 0, SPA_PARAM_Format, 0, 1, nullptr);
  }
  static void param(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
    auto &self = *static_cast<Node *>(data);
    if (id != SPA_PARAM_Format) return;
    spa_audio_info_raw format{};
    self.rate = self.channels = 0;
    if (param && spa_format_audio_raw_parse(param, &format) >= 0) {
      self.rate = format.rate; self.channels = format.channels;
    }
  }
};
struct PipeWireCatalog::Device {
  pw_device *proxy = nullptr;
  spa_hook listener{};
  QVariantMap props;
  ~Device() {
    if (proxy) { spa_hook_remove(&listener); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy)); }
  }
  static void info(void *data, const pw_device_info *info) {
    if (info->props) static_cast<Device *>(data)->props.insert(properties(info->props));
  }
};
std::optional<PipeWireSource> pipeWireSource(const QVariantMap &props, uint32_t id,
    uint32_t rate, uint32_t channels, const QList<BluetoothSource> &devices) {
  const auto name = props.value(PW_KEY_NODE_NAME).toString();
  const auto serial = props.value(PW_KEY_OBJECT_SERIAL).toString();
  if (name.isEmpty() || serial.isEmpty()) return {};
  PipeWireSource source{"pipewire:" + name,
      props.value(PW_KEY_NODE_DESCRIPTION, name).toString(), serial,
      props.value("api.bluez5.codec").toString(), id, rate, channels};
  const auto bluezPath = props.value("api.bluez5.path").toString();
  if (!bluezPath.isEmpty() || props.value(PW_KEY_DEVICE_API).toString() == "bluez5" || name.startsWith("bluez_input.")) {
    // A phone's HFP microphone must not replace its A2DP music source.
    if (!props.value("api.bluez5.profile").toString().startsWith("a2dp-")) return {};
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto &device) {
      return device.path == bluezPath;
    });
    if (found == devices.end() || !found->connected) return {};
    source.id = found->id; source.name = found->name;
  }
  return source;
}
PipeWireCatalog &PipeWireCatalog::instance() {
  static QPointer<PipeWireCatalog> instance;
  if (!instance) instance = new PipeWireCatalog(QCoreApplication::instance());
  return *instance;
}
PipeWireCatalog::PipeWireCatalog(QObject *parent) : QObject(parent) {
  pw_init(nullptr, nullptr);
  connect(&timer_, &QTimer::timeout, this, &PipeWireCatalog::tick);
  connect(&bluez_, &BluezCatalog::changed, this, [this] { tick(); emit changed(); });
  connect(&bluez_, &BluezCatalog::initialQueryFinished, this, [this](const QString &error) {
    if (!error.isEmpty()) { finishInitialQuery(error); return; }
    bluezReady_ = true;
    tick();
  });
  timer_.start(100);
  connectServer();
}
PipeWireCatalog::~PipeWireCatalog() { timer_.stop(); disconnectServer(); }
void PipeWireCatalog::connectServer() {
  loop_ = pw_thread_loop_new("airplayqt-pipewire", nullptr);
  if (!loop_) {
    finishInitialQuery("Cannot create the PipeWire thread loop");
    return;
  }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (context_) core_ = pw_context_connect(context_, nullptr, 0);
  if (core_) {
    static const pw_core_events events = [] {
      pw_core_events e{}; e.version = PW_VERSION_CORE_EVENTS;
      e.error = coreError; e.done = coreDone; return e;
    }();
    pw_core_add_listener(core_, &coreListener_, &events, this);
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (registry_) {
      static const pw_registry_events events = [] {
        pw_registry_events e{}; e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = global; e.global_remove = removed; return e;
      }();
      pw_registry_add_listener(registry_, &registryListener_, &events, this);
      if (!initialResult_) {
        initialSequence_ = pw_core_sync(core_, PW_ID_CORE, 0);
        if (initialSequence_ < 0)
          snapshotError_ = "Cannot synchronize the initial PipeWire device query";
      }
    }
  }
  if (!core_ || !registry_ || pw_thread_loop_start(loop_) < 0) {
    disconnectServer();
    finishInitialQuery("Cannot connect to the PipeWire server");
  }
}
void PipeWireCatalog::finishInitialQuery(const QString &error) {
  if (initialResult_) return;
  initialResult_ = error;
  emit initialQueryFinished(error);
}
void PipeWireCatalog::disconnectServer() {
  emit disconnecting();
  if (loop_) pw_thread_loop_stop(loop_);
  nodes_.clear(); devices_.clear();
  if (registry_) { spa_hook_remove(&registryListener_); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_)); }
  if (core_) { spa_hook_remove(&coreListener_); pw_core_disconnect(core_); }
  if (context_) pw_context_destroy(context_);
  if (loop_) pw_thread_loop_destroy(loop_);
  loop_ = nullptr; context_ = nullptr; core_ = nullptr; registry_ = nullptr;
  sources_.clear();
  lost_.store(false);
  retryTicks_ = 10;
}
void PipeWireCatalog::coreError(void *data, uint32_t id, int, int result, const char *message) {
  auto &self = *static_cast<PipeWireCatalog *>(data);
  if (!self.snapshotReady_ && self.snapshotError_.isEmpty())
    self.snapshotError_ = QString("PipeWire device query failed: %1")
        .arg(QString::fromUtf8(message ? message : "unknown error"));
  if (id == PW_ID_CORE || result == -EPIPE)
    self.lost_.store(true, std::memory_order_release);
}
void PipeWireCatalog::coreDone(void *data, uint32_t id, int sequence) {
  auto &self = *static_cast<PipeWireCatalog *>(data);
  if (id != PW_ID_CORE || sequence != self.initialSequence_ || self.snapshotReady_)
    return;
  if (!self.registrySynced_) {
    // Registry callbacks bind nodes/devices. A second barrier includes the
    // properties returned by those binds, not just the registry globals.
    self.registrySynced_ = true;
    self.initialSequence_ = pw_core_sync(self.core_, PW_ID_CORE, sequence);
    if (self.initialSequence_ < 0)
      self.snapshotError_ = "Cannot synchronize initial PipeWire device properties";
  } else {
    self.snapshotReady_ = true;
  }
}
void PipeWireCatalog::global(void *data, uint32_t id, uint32_t, const char *type,
                              uint32_t version, const spa_dict *props) {
  auto &self = *static_cast<PipeWireCatalog *>(data);
  if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0) {
    auto device = std::make_unique<Device>();
    device->props = properties(props);
    device->proxy = static_cast<pw_device *>(pw_registry_bind(self.registry_, id, type,
        std::min(version, uint32_t(PW_VERSION_DEVICE)), 0));
    if (!device->proxy) {
      if (!self.snapshotReady_ && self.snapshotError_.isEmpty())
        self.snapshotError_ = "Cannot bind a device in the initial PipeWire query";
      return;
    }
    static const pw_device_events events = [] {
      pw_device_events e{}; e.version = PW_VERSION_DEVICE_EVENTS; e.info = Device::info; return e;
    }();
    pw_device_add_listener(device->proxy, &device->listener, &events, device.get());
    self.devices_[id] = std::move(device);
    return;
  }
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
  auto values = properties(props);
  if (!values.value(PW_KEY_MEDIA_CLASS).toString().startsWith("Audio/Source")) return;
  auto node = std::make_unique<Node>();
  node->props = std::move(values);
  node->proxy = static_cast<pw_node *>(pw_registry_bind(self.registry_, id, type,
      std::min(version, uint32_t(PW_VERSION_NODE)), 0));
  if (!node->proxy) {
    if (!self.snapshotReady_ && self.snapshotError_.isEmpty())
      self.snapshotError_ = "Cannot bind an input node in the initial PipeWire query";
    return;
  }
  static const pw_node_events events = [] {
    pw_node_events e{}; e.version = PW_VERSION_NODE_EVENTS;
    e.info = Node::info; e.param = Node::param; return e;
  }();
  pw_node_add_listener(node->proxy, &node->listener, &events, node.get());
  uint32_t params[] = {SPA_PARAM_Format};
  pw_node_subscribe_params(node->proxy, params, 1);
  self.nodes_[id] = std::move(node);
}
void PipeWireCatalog::removed(void *data, uint32_t id) {
  auto &self = *static_cast<PipeWireCatalog *>(data);
  self.nodes_.erase(id); self.devices_.erase(id);
}
void PipeWireCatalog::tick() {
  if (lost_.load(std::memory_order_acquire)) {
    disconnectServer();
    finishInitialQuery("PipeWire disconnected during the initial device query");
    emit changed();
  }
  if (!core_) {
    if (--retryTicks_ <= 0) { retryTicks_ = 10; connectServer(); }
    return;
  }
  QList<PipeWireSource> next;
  bool snapshotReady;
  QString snapshotError;
  {
    PipeWireLock lock(loop_);
    snapshotReady = snapshotReady_;
    snapshotError = snapshotError_;
    for (const auto &[id, node] : nodes_) {
      QVariantMap props;
      const auto device = devices_.find(node->props.value(PW_KEY_DEVICE_ID).toUInt());
      if (device != devices_.end()) props = device->second->props;
      props.insert(node->props);
      if (const auto source = pipeWireSource(props, id, node->rate, node->channels, bluez_.sources()))
        next.append(*source);
    }
  }
  if (next != sources_) { sources_ = next; emit changed(); }
  if (!snapshotError.isEmpty()) finishInitialQuery(snapshotError);
  else if (snapshotReady && bluezReady_) finishInitialQuery({});
}
QList<DriverInfo> PipeWireCatalog::devices() const {
  QList<DriverInfo> result;
  for (const auto &device : bluez_.sources())
    result.append({device.id, device.name, CaptureKind::Input, device.connected});
  for (const auto &source : sources_)
    if (!source.id.startsWith("bluez:")) result.append({source.id, source.name});
  return result;
}
} // namespace audio
