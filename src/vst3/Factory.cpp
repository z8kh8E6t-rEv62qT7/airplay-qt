#include "Plugin.h"
#include "PluginRuntime.h"
#include "public.sdk/source/main/pluginfactory.h"
using namespace Steinberg;
using namespace Steinberg::Vst;

bool InitModule() { return true; }
extern "C" SMTG_EXPORT_SYMBOL bool AirPlayQtCanUnload() {
  return gPluginFactory == nullptr && vst3::PluginRuntime::prepareUnload();
}
bool DeinitModule() {
  vst3::PluginRuntime::shutdown();
  return true;
}

BEGIN_FACTORY_DEF("AirPlayQt", "", "")
DEF_CLASS2(INLINE_UID_FROM_FUID(vst3::processorId), PClassInfo::kManyInstances,
           kVstAudioEffectClass, "AirPlayQt", 0, "Fx|Tools", AIRPLAY_VERSION,
           kVstVersionString, vst3::Processor::create)
DEF_CLASS2(INLINE_UID_FROM_FUID(vst3::controllerId), PClassInfo::kManyInstances,
           kVstComponentControllerClass, "AirPlayQt Controller", 0, "",
           AIRPLAY_VERSION, kVstVersionString, vst3::EditController::create)
END_FACTORY
