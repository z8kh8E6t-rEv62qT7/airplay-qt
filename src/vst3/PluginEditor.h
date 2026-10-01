#pragma once
#include "PluginState.h"
#include "pluginterfaces/gui/iplugview.h"
#include <functional>
namespace vst3 {
Steinberg::IPlugView *createEditor(const std::shared_ptr<PluginState> &,
                                   std::function<void()> languageEdited = {});
}
