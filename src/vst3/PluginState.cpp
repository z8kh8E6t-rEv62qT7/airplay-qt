#include "PluginState.h"
#include <bit>
#include <map>

namespace vst3 {
namespace {
std::mutex registryMutex;
std::map<uint64_t, std::weak_ptr<PluginState>> registry;
uint64_t nextId = 0;
constexpr uint32_t magic = 0x51504156; // VAPQ, fixed little endian wire format.
constexpr uint32_t version = 2;
bool transfer(Steinberg::IBStream *stream, void *bytes, int count, bool write) {
  if (!stream)
    return false;
  int done = 0;
  while (done < count) {
    Steinberg::int32 actual = 0;
    auto *part = static_cast<char *>(bytes) + done;
    const auto result = write ? stream->write(part, count - done, &actual)
                              : stream->read(part, count - done, &actual);
    if (result != Steinberg::kResultOk || actual <= 0 || actual > count - done)
      return false;
    done += actual;
  }
  return true;
}
template <class T>
bool number(Steinberg::IBStream *stream, T &value, bool write) {
  std::array<unsigned char, sizeof(T)> bytes{};
  if (write)
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<unsigned char>(value >> (8 * i));
  if (!transfer(stream, bytes.data(), int(bytes.size()), write))
    return false;
  if (!write) {
    value = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
      value |= T(bytes[i]) << (8 * i);
  }
  return true;
}
bool text(Steinberg::IBStream *stream, QString &value, bool write) {
  auto bytes = value.toUtf8();
  uint32_t size = uint32_t(bytes.size());
  if (!number(stream, size, write) || size > 4096)
    return false;
  if (!write)
    bytes.resize(int(size));
  if (size && !transfer(stream, bytes.data(), int(size), write))
    return false;
  if (!write) {
    value = QString::fromUtf8(bytes);
    if (value.toUtf8() != bytes)
      return false;
  }
  return true;
}
bool stateIo(Steinberg::IBStream *stream, SavedState &state, bool write) {
  uint32_t header = magic, format = version,
           count = uint32_t(app::timingFields.size());
  if (!number(stream, header, write) || !number(stream, format, write) ||
      !number(stream, count, write) || header != magic || format != version ||
      count != app::timingFields.size())
    return false;
  for (const auto &field : app::timingFields) {
    auto bits = std::bit_cast<uint64_t>(state.timing.*(field.member));
    if (!number(stream, bits, write))
      return false;
    if (!write)
      state.timing.*(field.member) = std::bit_cast<double>(bits);
  }
  uint32_t bypass = state.bypass ? 1 : 0;
  if (!number(stream, bypass, write) || bypass > 1)
    return false;
  state.bypass = bypass != 0;
  return text(stream, state.networkBinding.interfaceName, write) &&
         text(stream, state.networkBinding.ipv4, write) &&
         state.networkBinding.validate().isEmpty() &&
         state.timing.validate().isEmpty();
}
} // namespace
bool readState(Steinberg::IBStream *stream, SavedState &result) {
  SavedState candidate;
  if (!stateIo(stream, candidate, false))
    return false;
  result = candidate;
  return true;
}
bool writeState(Steinberg::IBStream *stream, const SavedState &value) {
  if (!value.timing.validate().isEmpty() ||
      !value.networkBinding.validate().isEmpty())
    return false;
  auto copy = value;
  return stateIo(stream, copy, true);
}
std::shared_ptr<PluginState> PluginState::create() {
  std::lock_guard lock(registryMutex);
  auto state = std::shared_ptr<PluginState>(new PluginState(++nextId));
  registry.emplace(state->id, state);
  return state;
}
std::shared_ptr<PluginState> PluginState::find(uint64_t id) {
  std::lock_guard lock(registryMutex);
  const auto it = registry.find(id);
  return it == registry.end() ? nullptr : it->second.lock();
}
PluginState::~PluginState() {
  std::lock_guard lock(registryMutex);
  registry.erase(id);
}
QString PluginState::configurationError() const {
  return invalidConfiguration.load()
             ? QString("插件配置版本无效或数据损坏；请移除该实例，重新添加后保"
                       "存工程。")
             : QString{};
}
airplay::NetworkBinding PluginState::networkBinding() const {
  std::lock_guard lock(mutex_);
  return networkBinding_;
}
void PluginState::setNetworkBinding(const airplay::NetworkBinding &value) {
  std::lock_guard lock(mutex_);
  networkBinding_ = value;
  ++timingRevision;
}
app::Timing PluginState::timing() const {
  std::lock_guard lock(mutex_);
  return timing_;
}
void PluginState::setTiming(const app::Timing &value) {
  std::lock_guard lock(mutex_);
  timing_ = value;
  ++timingRevision;
}
} // namespace vst3
