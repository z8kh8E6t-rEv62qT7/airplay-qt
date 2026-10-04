#include "PluginEditor.h"
#include "NativeEditor.h"
#include "PluginRuntime.h"
#include "app/Message.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "public.sdk/source/common/pluginview.h"
#include <QScrollArea>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QWindow>
#include <cmath>
#include <cstring>
namespace vst3 {
using namespace Steinberg;
namespace {
class Editor final : public CPluginView, public IPlugViewContentScaleSupport {
public:
  explicit Editor(std::shared_ptr<PluginState> state,
                  std::function<void()> stateEdited)
      : state_(std::move(state)), stateEdited_(std::move(stateEdited)) {
    rect = {0, 0, 950, 880};
    if (state_) {
      const auto layout = state_->windowLayout();
      if (layout.width > 0)
        rect = {0, 0, layout.width, layout.height};
      expandedWidth_ = layout.expandedWidth;
    }
    PluginRuntime::addComponent();
  }
  ~Editor() override {
    removed();
    PluginRuntime::removeComponent();
  }
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
    return type && std::strcmp(type, NativeEditor::platformType()) == 0
               ? kResultTrue
               : kResultFalse;
  }
  tresult PLUGIN_API attached(void *parent, FIDString type) override {
    if (isAttached() || !parent || isPlatformTypeSupported(type) != kResultTrue)
      return kResultFalse;
    try {
      if (state_) {
        const auto layout = state_->windowLayout();
        if (layout.width > 0)
          rect = {0, 0, qRound(layout.width * scale_), qRound(layout.height * scale_)};
        expandedWidth_ = qRound(layout.expandedWidth * scale_);
      }
      auto &runtime = PluginRuntime::acquire(parent);
      runtime_ = &runtime;
      panel_ = runtime.open(state_);
      QObject::connect(panel_, &ui::StreamingPanel::languageChanged, panel_,
                       [callback = stateEdited_] {
                         if (callback)
                           callback();
                       });
      viewport_ = std::make_unique<QScrollArea>();
      viewport_->setWidgetResizable(true);
      viewport_->setFrameShape(QFrame::NoFrame);
      viewport_->setWidget(panel_);
      QObject::connect(panel_, &ui::StreamingPanel::layoutChanged,
                       viewport_.get(), [this] { saveLayout(); });
      QObject::connect(panel_, &ui::StreamingPanel::logVisibilityChanged,
                       viewport_.get(), [this](bool visible, int removedWidth) {
        if (!visible)
          expandedWidth_ = rect.getWidth();
        const double dpr = NativeEditor::usesLogicalCoordinates()
                               ? 1. : viewport_->devicePixelRatioF();
        ViewRect changed = rect;
        const int width = visible ? expandedWidth_
                                  : rect.getWidth() - qRound(removedWidth * dpr);
        changed.right = changed.left + std::max(width, int(650 * scale_));
        // Host-owned windows may only be resized through IPlugFrame.
        if (plugFrame)
          plugFrame->resizeView(this, &changed);
      });
      // Scope the editor style to our widgets, including when borrowing the
      // host's QApplication. QWidget styles do not propagate to children.
      auto *fusion = QStyleFactory::create("Fusion");
      if (!fusion)
        throw airplay::Error(i18n::text(i18n::Id::QtFusionStyleIsUnavailable));
      fusion->setParent(panel_);
      viewport_->setStyle(fusion);
      for (auto *widget : viewport_->findChildren<QWidget *>())
        widget->setStyle(fusion);
      viewport_->setWindowFlags(Qt::FramelessWindowHint);
      viewport_->setAttribute(Qt::WA_NativeWindow);
      viewport_->winId();
      native_ = std::make_unique<NativeEditor>(parent, viewport_.get());
      applyScale();
      viewport_->show();
    } catch (const std::exception &e) {
      detachPanel();
      try {
        native_ = std::make_unique<NativeEditor>(
            parent, nullptr,
            i18n::fromException(e).render(state_ ? state_->language()
                                                 : i18n::Language::English));
      } catch (...) {
        return kResultFalse;
      }
    }
    CPluginView::attached(parent, type);
    onSize(&rect);
    if (panel_ && state_) {
      panel_->setWindowLayout(scaledLayout(state_->windowLayout()));
      layoutRevision_ = state_->windowLayoutRevision.load();
      restoringLayout_ = false;
      auto *timer = new QTimer(viewport_.get());
      QObject::connect(timer, &QTimer::timeout, viewport_.get(), [this] {
        if (layoutRevision_ == state_->windowLayoutRevision.load())
          return;
        restoreLayout();
      });
      timer->start(50);
    }
    return kResultOk;
  }
  tresult PLUGIN_API removed() override {
    detachPanel();
    return CPluginView::removed();
  }
  tresult PLUGIN_API onSize(ViewRect *value) override {
    if (!value)
      return kInvalidArgument;
    CPluginView::onSize(value);
    if (viewport_) {
      const auto dpr = NativeEditor::usesLogicalCoordinates()
                           ? 1.
                           : viewport_->devicePixelRatioF();
      viewport_->resize(qRound(value->getWidth() / dpr),
                        qRound(value->getHeight() / dpr));
    }
    if (native_)
      native_->resize(value->getWidth(), value->getHeight());
    saveLayout();
    return kResultOk;
  }
  tresult PLUGIN_API onFocus(TBool focus) override {
    if (panel_ && focus) {
      panel_->setFocus(Qt::OtherFocusReason);
      if (native_)
        native_->focus();
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
    if (NativeEditor::usesLogicalCoordinates())
      return kResultTrue;
    ViewRect changed{0, 0, int32(rect.getWidth() * factor / scale_),
                     int32(rect.getHeight() * factor / scale_)};
    expandedWidth_ = qRound(expandedWidth_ * factor / scale_);
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
  void saveLayout() {
    if (restoringLayout_ || !panel_ || !state_ ||
        layoutRevision_ != state_->windowLayoutRevision.load())
      return;
    auto layout = panel_->windowLayout(
        QSize(qRound(rect.getWidth() / scale_), qRound(rect.getHeight() / scale_)),
        qRound(expandedWidth_ / scale_));
    const double ratio = panelScale();
    layout.controlWidth = qRound(layout.controlWidth / ratio);
    layout.logWidth = qRound(layout.logWidth / ratio);
    const bool changed = state_->setWindowLayout(layout);
    layoutRevision_ = state_->windowLayoutRevision.load();
    if (changed && stateEdited_)
      stateEdited_();
  }
  void restoreLayout() {
    restoringLayout_ = true;
    const auto layout = state_->windowLayout();
    expandedWidth_ = qRound(layout.expandedWidth * scale_);
    panel_->setWindowLayout(scaledLayout(layout));
    if (layout.width > 0 && plugFrame) {
      ViewRect changed{0, 0, qRound(layout.width * scale_),
                       qRound(layout.height * scale_)};
      plugFrame->resizeView(this, &changed);
    }
    panel_->setWindowLayout(scaledLayout(layout));
    layoutRevision_ = state_->windowLayoutRevision.load();
    restoringLayout_ = false;
  }
  double panelScale() const {
    return NativeEditor::usesLogicalCoordinates()
               ? 1. : scale_ / viewport_->devicePixelRatioF();
  }
  app::WindowLayout scaledLayout(app::WindowLayout value) const {
    value.controlWidth = qRound(value.controlWidth * panelScale());
    value.logWidth = qRound(value.logWidth * panelScale());
    return value;
  }
  void applyScale() {
    if (!panel_ || !viewport_)
      return;
    const auto ratio = NativeEditor::usesLogicalCoordinates()
                           ? 1.
                           : scale_ / viewport_->devicePixelRatioF();
    auto font = QApplication::font();
    font.setPointSizeF(font.pointSizeF() * ratio);
    panel_->setFont(font);
  }
  void detachPanel() {
    saveLayout();
    restoringLayout_ = true;
    if (panel_) {
      panel_->hide();
      if (viewport_)
        viewport_->takeWidget();
    }
    // The panel owns the shared style. Destroy the viewport while that style
    // is still alive, then let the runtime retire the panel/discovery.
    native_.reset();
    viewport_.reset();
    if (panel_ && runtime_ && state_)
      runtime_->close(state_->id);
    panel_ = nullptr;
    runtime_ = nullptr;
  }
  std::shared_ptr<PluginState> state_;
  std::function<void()> stateEdited_;
  ui::StreamingPanel *panel_ = nullptr;
  PluginRuntime *runtime_ = nullptr;
  std::unique_ptr<NativeEditor> native_;
  std::unique_ptr<QScrollArea> viewport_;
  float scale_ = 1;
  int expandedWidth_ = 0;
  bool restoringLayout_ = true;
  uint64_t layoutRevision_ = 0;
};
} // namespace
Steinberg::IPlugView *createEditor(const std::shared_ptr<PluginState> &state,
                                   std::function<void()> stateEdited) {
  return new Editor(state, std::move(stateEdited));
}
} // namespace vst3
