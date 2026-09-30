#pragma once
#include "PluginState.h"
#include "pluginterfaces/gui/iplugview.h"
namespace vst3 {
Steinberg::IPlugView *createEditor(const std::shared_ptr<PluginState> &);
}
