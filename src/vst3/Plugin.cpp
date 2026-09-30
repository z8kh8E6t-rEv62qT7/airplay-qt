#include "Plugin.h"
#include "PluginRuntime.h"
#include <QStyleFactory>
#include <QStyle>
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include <QScrollArea>
#include <QWindow>
#include <cmath>
#include <cstring>

namespace vst3 {
using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
class Editor final : public CPluginView, public IPlugViewContentScaleSupport {
public:
  explicit Editor(std::shared_ptr<PluginState> state)
      : state_(std::move(state)) {
    rect = {0, 0, 900, 880};
    PluginRuntime::addComponent();
  }
  ~Editor() override {
    removed();
    PluginRuntime::removeComponent();
  }
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
    return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue
                                                             : kResultFalse;
  }
  tresult PLUGIN_API attached(void *parent, FIDString type) override {
    if (isAttached() || !parent || isPlatformTypeSupported(type) != kResultTrue)
      return kResultFalse;
    try {
      auto &runtime = PluginRuntime::acquire(static_cast<HWND>(parent));
      runtime_ = &runtime;
      panel_ = runtime.open(state_);
      viewport_ = std::make_unique<QScrollArea>();
      viewport_->setWidgetResizable(true);
      viewport_->setFrameShape(QFrame::NoFrame);
      viewport_->setWidget(panel_);
      // Scope the editor style to our widgets, including when borrowing the
      // host's QApplication. QWidget styles do not propagate to children.
      auto *fusion = QStyleFactory::create("Fusion");
      if (!fusion)
        throw airplay::Error("Qt Fusion 样式不可用。");
      fusion->setParent(panel_);
      viewport_->setStyle(fusion);
      for (auto *widget : viewport_->findChildren<QWidget *>())
        widget->setStyle(fusion);
      viewport_->setWindowFlags(Qt::FramelessWindowHint);
      viewport_->setAttribute(Qt::WA_NativeWindow);
      viewport_->winId();
      foreign_.reset(QWindow::fromWinId(reinterpret_cast<WId>(parent)));
      if (!foreign_)
        throw airplay::Error("Qt 无法嵌入宿主 HWND。");
      viewport_->windowHandle()->setParent(foreign_.get());
      applyScale();
      viewport_->show();
    } catch (const std::exception &e) {
      detachPanel();
      const auto message = QString::fromUtf8(e.what()).toStdWString();
      error_ = CreateWindowExW(
          0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 12,
          12, rect.getWidth() - 24, 140, static_cast<HWND>(parent), nullptr,
          GetModuleHandleW(nullptr), nullptr);
      if (!error_)
        return kResultFalse;
    }
    CPluginView::attached(parent, type);
    onSize(&rect);
    return kResultOk;
  }
  tresult PLUGIN_API removed() override {
    detachPanel();
    if (error_) {
      DestroyWindow(error_);
      error_ = nullptr;
    }
    return CPluginView::removed();
  }
  tresult PLUGIN_API onSize(ViewRect *value) override {
    if (!value)
      return kInvalidArgument;
    CPluginView::onSize(value);
    if (viewport_) {
      const auto dpr = viewport_->devicePixelRatioF();
      viewport_->resize(qRound(value->getWidth() / dpr),
                        qRound(value->getHeight() / dpr));
      viewport_->windowHandle()->setPosition(0, 0);
    }
    if (error_)
      MoveWindow(error_, 12, 12, value->getWidth() - 24,
                 value->getHeight() - 24, TRUE);
    return kResultOk;
  }
  tresult PLUGIN_API onFocus(TBool focus) override {
    if (panel_ && focus) {
      panel_->setFocus(Qt::OtherFocusReason);
      SetFocus(reinterpret_cast<HWND>(viewport_->winId()));
    }
    return kResultOk;
  }
  tresult PLUGIN_API canResize() override { return kResultTrue; }
  tresult PLUGIN_API checkSizeConstraint(ViewRect *value) override {
    if (!value)
      return kInvalidArgument;
    value->right =
        value->left + std::max(value->getWidth(), int32(650 * scale_));
    value->bottom =
        value->top + std::max(value->getHeight(), int32(750 * scale_));
    return kResultTrue;
  }
  tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override {
    if (!std::isfinite(factor) || factor <= 0 || factor > 8)
      return kInvalidArgument;
    ViewRect changed{0, 0, int32(rect.getWidth() * factor / scale_),
                     int32(rect.getHeight() * factor / scale_)};
    scale_ = factor;
    applyScale();
    if (plugFrame)
      return plugFrame->resizeView(this, &changed);
    rect = changed;
    return kResultTrue;
  }
  OBJ_METHODS(Editor, CPluginView)
  DEFINE_INTERFACES
  DEF_INTERFACE(IPlugViewContentScaleSupport)
  END_DEFINE_INTERFACES(CPluginView)
  REFCOUNT_METHODS(CPluginView)
private:
  void applyScale() {
    if (!panel_ || !viewport_)
      return;
    const auto ratio = scale_ / viewport_->devicePixelRatioF();
    auto font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * ratio);
    panel_->setFont(font);
    if (auto *title = panel_->findChild<QLabel *>("streamingTitle"))
      title->setStyleSheet(
          QString("font-size: %1px; font-weight: 600; padding: 8px 0;")
              .arg(22 * ratio));
  }
  void detachPanel() {
    if (panel_) {
      panel_->hide();
      if (viewport_)
        viewport_->takeWidget();
      panel_ = nullptr;
      if (runtime_ && state_)
        runtime_->close(state_->id);
    }
    if (viewport_ && viewport_->windowHandle())
      viewport_->windowHandle()->setParent(nullptr);
    viewport_.reset();
    foreign_.reset();
  }
  std::shared_ptr<PluginState> state_;
  ui::StreamingPanel *panel_ = nullptr;
  PluginRuntime *runtime_ = nullptr;
  std::unique_ptr<QWindow> foreign_;
  std::unique_ptr<QScrollArea> viewport_;
  HWND error_ = nullptr;
  float scale_ = 1;
};
} // namespace
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
  return writeState(stream, {state_->timing(), state_->input.bypass()})
             ? kResultOk
             : kResultFalse;
}
tresult PLUGIN_API Processor::setState(IBStream *stream) {
  state_->input.stateLoad();
  ++state_->stopRevision;
  SavedState value;
  if (!readState(stream, value))
    return kResultFalse;
  state_->setTiming(value.timing);
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
    return state_ ? kResultOk : kResultFalse;
  }
  return Steinberg::Vst::EditController::notify(message);
}
tresult PLUGIN_API EditController::setComponentState(IBStream *stream) {
  SavedState value;
  if (!readState(stream, value))
    return kResultFalse;
  setParamNormalized(bypassId, value.bypass ? 1 : 0);
  return kResultOk;
}
IPlugView *PLUGIN_API EditController::createView(FIDString name) {
  if (!name || std::strcmp(name, ViewType::kEditor) != 0)
    return nullptr;
  return new Editor(state_);
}
} // namespace vst3
