#include "PluginState.h"
#include "app/Message.h"
#include <bit>
#include <map>

namespace vst3 {
namespace {
std::mutex registryMutex;
std::map<uint64_t, std::weak_ptr<PluginState>> registry;
uint64_t nextId = 0;
constexpr uint32_t magic = 0x51504156; // VAPQ, fixed little endian wire format.
constexpr uint32_t version = 4;
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
           count = uint32_t(
               (app::timingMsFields.size() + app::timingSamplesFields.size()));
  if (!number(stream, header, write) || !number(stream, format, write) ||
      !number(stream, count, write) || header != magic || format != version ||
      count != (app::timingMsFields.size() + app::timingSamplesFields.size()))
    return false;
  for (const auto &field : app::timingMsFields) {
    auto bits = std::bit_cast<uint64_t>(state.timing.*(field.member));
    if (!number(stream, bits, write))
      return false;
    if (!write)
      state.timing.*(field.member) = std::bit_cast<double>(bits);
  }
  for (const auto &field : app::timingSamplesFields) {
    uint32_t value = uint32_t(state.timing.*(field.member));
    if (!number(stream, value, write) || value > uint32_t(field.maximum) ||
        value < uint32_t(field.minimum))
      return false;
    if (!write)
      state.timing.*(field.member) = int(value);
  }
  uint32_t bypass = state.bypass ? 1 : 0;
  if (!number(stream, bypass, write) || bypass > 1)
    return false;
  state.bypass = bypass != 0;
  uint32_t language = uint32_t(state.language);
  if (!text(stream, state.networkBinding.interfaceName, write) ||
      !text(stream, state.networkBinding.ipv4, write) ||
      !number(stream, language, write) || language > 1)
    return false;
  state.language = i18n::Language(language);
  // Version 4 originally ended after language. EOF here is the only valid
  // missing-layout case; a partially written extension must fail atomically.
  uint32_t layoutTag = 0x3154594c; // LYT1
  if (!write) {
    unsigned char first = 0;
    Steinberg::int32 actual = 0;
    const auto result = stream->read(&first, 1, &actual);
    if (actual == 0 && (result == Steinberg::kResultOk ||
                        result == Steinberg::kResultFalse))
      return state.networkBinding.validate().isEmpty() &&
             state.timing.validate().isEmpty();
    std::array<unsigned char, 3> rest{};
    if (result != Steinberg::kResultOk || actual != 1 ||
        !transfer(stream, rest.data(), 3, false) || first != 0x4c ||
        rest != std::array<unsigned char, 3>{0x59, 0x54, 0x31})
      return false;
  } else if (!number(stream, layoutTag, true)) {
    return false;
  }
  for (const auto &field : app::windowLayoutFields) {
    uint32_t value = uint32_t(state.windowLayout.*(field.member));
    if (!number(stream, value, write) || value > 32768)
      return false;
    state.windowLayout.*(field.member) = int(value);
  }
  uint32_t visible = state.windowLayout.logVisible;
  if (!number(stream, visible, write) || visible > 1)
    return false;
  state.windowLayout.logVisible = visible != 0;
  return state.networkBinding.validate().isEmpty() &&
         state.timing.validate().isEmpty() && state.windowLayout.valid();
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
  if (!i18n::valid(value.language) || !value.timing.validate().isEmpty() ||
      !value.networkBinding.validate().isEmpty() || !value.windowLayout.valid())
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
i18n::Message PluginState::configurationError() const {
  return invalidConfiguration.load()
             ? i18n::text(i18n::Id::PluginConfigurationVersionIsInvalidOrData)
             : i18n::Message{};
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
i18n::Language PluginState::language() const {
  std::lock_guard lock(mutex_);
  return language_;
}
void PluginState::setLanguage(i18n::Language value) {
  if (!i18n::valid(value))
    return;
  std::lock_guard lock(mutex_);
  if (language_ == value)
    return;
  language_ = value;
  ++languageRevision;
}
app::WindowLayout PluginState::windowLayout() const {
  std::lock_guard lock(mutex_);
  return windowLayout_;
}
bool PluginState::setWindowLayout(const app::WindowLayout &value) {
  if (!value.valid())
    return false;
  std::lock_guard lock(mutex_);
  if (windowLayout_ == value)
    return false;
  windowLayout_ = value;
  ++windowLayoutRevision;
  return true;
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
