#pragma once
#include "VstAudioInput.h"
#include "app/Message.h"
#include "app/Settings.h"
#include "pluginterfaces/base/ibstream.h"
#include <mutex>

namespace vst3 {
struct SavedState {
  app::Timing timing;
  bool bypass = false;
  airplay::NetworkBinding networkBinding;
  i18n::Language language = i18n::Language::English;
};
bool readState(Steinberg::IBStream *, SavedState &);
bool writeState(Steinberg::IBStream *, const SavedState &);

class PluginState {
public:
  static std::shared_ptr<PluginState> create();
  static std::shared_ptr<PluginState> find(uint64_t id);
  ~PluginState();
  app::Timing timing() const;
  i18n::Language language() const;
  void setLanguage(i18n::Language);
  airplay::NetworkBinding networkBinding() const;
  void setNetworkBinding(const airplay::NetworkBinding &);
  i18n::Message configurationError() const;
  std::atomic<bool> invalidConfiguration{false};
  void setTiming(const app::Timing &);
  const uint64_t id;
  VstAudioInput input;
  std::atomic<bool> processorAlive{true};
  std::atomic<uint64_t> stopRevision{0}, timingRevision{0}, languageRevision{0};

private:
  explicit PluginState(uint64_t value) : id(value) {}
  mutable std::mutex mutex_;
  app::Timing timing_;
  i18n::Language language_ = i18n::Language::English;
  airplay::NetworkBinding networkBinding_;
};
} // namespace vst3
