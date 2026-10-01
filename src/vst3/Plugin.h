#pragma once
#include "PluginState.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

namespace vst3 {
inline const Steinberg::FUID processorId(0xD08C4A74, 0xC1944ACC, 0x999AF752,
                                         0x091EA0DA);
inline const Steinberg::FUID controllerId(0x35C88354, 0x6B65444B, 0xA7791553,
                                          0x895D89AA);
inline constexpr Steinberg::Vst::ParamID bypassId = 0;
class Processor final : public Steinberg::Vst::AudioEffect {
public:
  Processor();
  ~Processor() override;
  static Steinberg::FUnknown *create(void *) {
    return static_cast<Steinberg::Vst::IAudioProcessor *>(new Processor);
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *) override;
  Steinberg::tresult PLUGIN_API terminate() override;
  Steinberg::tresult PLUGIN_API setBusArrangements(
      Steinberg::Vst::SpeakerArrangement *, Steinberg::int32,
      Steinberg::Vst::SpeakerArrangement *, Steinberg::int32) override;
  Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32) override;
  Steinberg::tresult PLUGIN_API
  setupProcessing(Steinberg::Vst::ProcessSetup &) override;
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool) override;
  Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool) override;
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &) override;
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *) override;
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *) override;
  Steinberg::tresult PLUGIN_API
  connect(Steinberg::Vst::IConnectionPoint *) override;
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *) override;
  Steinberg::uint32 PLUGIN_API getLatencySamples() override { return 0; }
  Steinberg::uint32 PLUGIN_API getTailSamples() override {
    return Steinberg::Vst::kNoTail;
  }
  std::shared_ptr<PluginState> state() const { return state_; }

private:
  void identify();
  void retire();
  std::shared_ptr<PluginState> state_;
};
class EditController final : public Steinberg::Vst::EditController {
public:
  EditController();
  ~EditController() override;
  static Steinberg::FUnknown *create(void *) {
    return static_cast<Steinberg::Vst::IEditController *>(new EditController);
  }
  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *) override;
  Steinberg::tresult PLUGIN_API
  connect(Steinberg::Vst::IConnectionPoint *) override;
  Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *) override;
  Steinberg::tresult PLUGIN_API
  setComponentState(Steinberg::IBStream *) override;
  Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString) override;

private:
  bool invalidComponentState_ = false;
  std::shared_ptr<PluginState> state_;
};
} // namespace vst3
