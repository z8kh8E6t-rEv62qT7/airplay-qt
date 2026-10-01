#pragma once
#include "VstAudioInput.h"
#include "app/Settings.h"
#include "pluginterfaces/base/ibstream.h"
#include <mutex>

namespace vst3 {
struct SavedState {
  app::Timing timing;
  bool bypass = false;
  airplay::NetworkBinding networkBinding;
};
bool readState(Steinberg::IBStream *, SavedState &);
bool writeState(Steinberg::IBStream *, const SavedState &);

class PluginState {
public:
  static std::shared_ptr<PluginState> create();
  static std::shared_ptr<PluginState> find(uint64_t id);
  ~PluginState();
  app::Timing timing() const;
  airplay::NetworkBinding networkBinding() const;
  void setNetworkBinding(const airplay::NetworkBinding &);
  QString configurationError() const;
  std::atomic<bool> invalidConfiguration{false};
  void setTiming(const app::Timing &);
  const uint64_t id;
  VstAudioInput input;
  std::atomic<bool> processorAlive{true};
  std::atomic<uint64_t> stopRevision{0}, timingRevision{0};

private:
  explicit PluginState(uint64_t value) : id(value) {}
  mutable std::mutex mutex_;
  app::Timing timing_;
  airplay::NetworkBinding networkBinding_;
};
} // namespace vst3
