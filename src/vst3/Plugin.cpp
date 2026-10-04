#include "Plugin.h"
#include "PluginEditor.h"
#include "PluginRuntime.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <cmath>
#include <cstring>

namespace vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
Processor::Processor() : state_(PluginState::create()) {
  setControllerClass(controllerId);
  PluginRuntime::addComponent();
}
Processor::~Processor() {
  retire();
  PluginRuntime::removeComponent();
}
void Processor::retire() {
  if (state_ && state_->processorAlive.exchange(false)) {
    state_->input.stop();
    PluginRuntime::processorRemoved(state_->id);
  }
}
tresult PLUGIN_API Processor::initialize(FUnknown *context) {
  const auto result = AudioEffect::initialize(context);
  if (result != kResultOk)
    return result;
  if (!state_->processorAlive.load())
    state_ = PluginState::create();
  addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
  addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
  return kResultOk;
}
tresult PLUGIN_API Processor::terminate() {
  retire();
  return AudioEffect::terminate();
}
tresult PLUGIN_API Processor::setBusArrangements(SpeakerArrangement *in,
                                                 int32 ins,
                                                 SpeakerArrangement *out,
                                                 int32 outs) {
  if (ins != 1 || outs != 1 || !in || !out || in[0] != SpeakerArr::kStereo ||
      out[0] != SpeakerArr::kStereo)
    return kResultFalse;
  return AudioEffect::setBusArrangements(in, ins, out, outs);
}
tresult PLUGIN_API Processor::canProcessSampleSize(int32 size) {
  return size == kSample32 || size == kSample64 ? kResultTrue : kResultFalse;
}
tresult PLUGIN_API Processor::setupProcessing(ProcessSetup &setup) {
  if (!std::isfinite(setup.sampleRate) || setup.sampleRate <= 0 ||
      setup.maxSamplesPerBlock <= 0 ||
      canProcessSampleSize(setup.symbolicSampleSize) != kResultTrue)
    return kInvalidArgument;
  state_->input.configure(setup.sampleRate, setup.maxSamplesPerBlock,
                          setup.symbolicSampleSize == kSample64);
  state_->input.setRealtime(setup.processMode == kRealtime);
  return AudioEffect::setupProcessing(setup);
}
tresult PLUGIN_API Processor::setActive(TBool active) {
  state_->input.setActive(active != 0);
  return AudioEffect::setActive(active);
}
tresult PLUGIN_API Processor::setProcessing(TBool processing) {
  state_->input.setProcessing(processing != 0);
  return kResultOk;
}
tresult PLUGIN_API Processor::process(ProcessData &data) {
  if (data.inputParameterChanges)
    for (int32 i = 0; i < data.inputParameterChanges->getParameterCount();
         ++i) {
      auto *queue = data.inputParameterChanges->getParameterData(i);
      if (!queue || queue->getParameterId() != bypassId)
        continue;
      for (int32 p = 0; p < queue->getPointCount(); ++p) {
        int32 offset = 0;
        ParamValue value = 0;
        if (queue->getPoint(p, offset, value) == kResultOk &&
            std::isfinite(value))
          state_->input.setBypass(value >= .5);
      }
    }
  state_->input.setRealtime(data.processMode == kRealtime);
  if (data.numSamples == 0)
    return kResultOk;
  if (data.numSamples < 0 ||
      canProcessSampleSize(data.symbolicSampleSize) != kResultTrue)
    return kInvalidArgument;
  auto *in =
      data.numInputs > 0 && data.inputs && data.inputs[0].numChannels == 2
          ? &data.inputs[0]
          : nullptr;
  auto *out =
      data.numOutputs > 0 && data.outputs && data.outputs[0].numChannels == 2
          ? &data.outputs[0]
          : nullptr;
  const uint64 silence = in ? in->silenceFlags : 3;
  if (out)
    out->silenceFlags = silence & 3;
  if (data.symbolicSampleSize == kSample32)
    state_->input.process(in ? in->channelBuffers32 : nullptr,
                          out ? out->channelBuffers32 : nullptr,
                          data.numSamples, silence,
                          data.processMode == kRealtime);
  else
    state_->input.process(in ? in->channelBuffers64 : nullptr,
                          out ? out->channelBuffers64 : nullptr,
                          data.numSamples, silence,
                          data.processMode == kRealtime);
  return kResultOk;
}
tresult PLUGIN_API Processor::getState(IBStream *stream) {
  if (state_->invalidConfiguration.load())
    return kResultFalse;
  return writeState(stream, {state_->timing(), state_->input.bypass(),
                             state_->networkBinding(), state_->language(),
                             state_->windowLayout()})
             ? kResultOk
             : kResultFalse;
}
tresult PLUGIN_API Processor::setState(IBStream *stream) {
  state_->input.stateLoad();
  ++state_->stopRevision;
  SavedState value;
  if (!readState(stream, value)) {
    state_->invalidConfiguration = true;
    return kResultFalse;
  }
  state_->invalidConfiguration = false;
  state_->setNetworkBinding(value.networkBinding);
  state_->setTiming(value.timing);
  state_->setLanguage(value.language);
  state_->setWindowLayout(value.windowLayout);
  state_->input.setBypass(value.bypass);
  return kResultOk;
}
void Processor::identify() {
  if (auto message = owned(allocateMessage())) {
    message->setMessageID("AirPlayQt.Instance");
    message->getAttributes()->setInt("id", int64(state_->id));
    sendMessage(message);
  }
}
tresult PLUGIN_API Processor::connect(IConnectionPoint *peer) {
  const auto result = AudioEffect::connect(peer);
  if (result == kResultOk)
    identify();
  return result;
}
tresult PLUGIN_API Processor::notify(IMessage *message) {
  if (message &&
      std::strcmp(message->getMessageID(), "AirPlayQt.Identify") == 0) {
    identify();
    return kResultOk;
  }
  return AudioEffect::notify(message);
}
EditController::EditController() { PluginRuntime::addComponent(); }
EditController::~EditController() { PluginRuntime::removeComponent(); }
tresult PLUGIN_API EditController::initialize(FUnknown *context) {
  const auto result = Steinberg::Vst::EditController::initialize(context);
  if (result == kResultOk)
    parameters.addParameter(
        STR16("Bypass"), nullptr, 1, 0,
        ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, bypassId);
  return result;
}
tresult PLUGIN_API EditController::connect(IConnectionPoint *peer) {
  const auto result = Steinberg::Vst::EditController::connect(peer);
  if (result == kResultOk)
    sendMessageID("AirPlayQt.Identify");
  return result;
}
tresult PLUGIN_API EditController::notify(IMessage *message) {
  if (message &&
      std::strcmp(message->getMessageID(), "AirPlayQt.Instance") == 0) {
    int64 id = 0;
    if (message->getAttributes()->getInt("id", id) != kResultOk || id <= 0)
      return kInvalidArgument;
    state_ = PluginState::find(uint64_t(id));
    if (state_ && invalidComponentState_) {
      state_->invalidConfiguration = true;
      ++state_->stopRevision;
    }
    return state_ ? kResultOk : kResultFalse;
  }
  return Steinberg::Vst::EditController::notify(message);
}
tresult PLUGIN_API EditController::setComponentState(IBStream *stream) {
  SavedState value;
  invalidComponentState_ = !readState(stream, value);
  if (invalidComponentState_) {
    if (state_) {
      state_->invalidConfiguration = true;
      ++state_->stopRevision;
    }
    return kResultFalse;
  }
  setParamNormalized(bypassId, value.bypass ? 1 : 0);
  return kResultOk;
}
IPlugView *PLUGIN_API EditController::createView(FIDString name) {
  if (!name || std::strcmp(name, ViewType::kEditor) != 0)
    return nullptr;
  return createEditor(state_, [controller = IPtr<EditController>(this)] {
    controller->setDirty(true);
  });
}
} // namespace vst3
