#pragma once
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <cstring>

// Stack-owned by the native host, outliving its controllers and editors.
class TestComponentHandler final : public Steinberg::Vst::IComponentHandler,
                                   public Steinberg::Vst::IComponentHandler2 {
public:
  unsigned dirtyCalls = 0, parameterCalls = 0;
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                               void **object) override {
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if (!object)
      return kInvalidArgument;
    *object = nullptr;
    if (std::memcmp(iid, IComponentHandler::iid, sizeof(TUID)) == 0 ||
        std::memcmp(iid, FUnknown::iid, sizeof(TUID)) == 0)
      *object = static_cast<IComponentHandler *>(this);
    else if (std::memcmp(iid, IComponentHandler2::iid, sizeof(TUID)) == 0)
      *object = static_cast<IComponentHandler2 *>(this);
    if (!*object)
      return kNoInterface;
    addRef();
    return kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
  Steinberg::uint32 PLUGIN_API release() override { return --references_; }
  Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override {
    ++parameterCalls;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API
  performEdit(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override {
    ++parameterCalls;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override {
    ++parameterCalls;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override {
    ++parameterCalls;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API setDirty(Steinberg::TBool dirty) override {
    if (dirty)
      ++dirtyCalls;
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API
  requestOpenEditor(Steinberg::FIDString) override {
    return Steinberg::kNotImplemented;
  }
  Steinberg::tresult PLUGIN_API startGroupEdit() override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API finishGroupEdit() override {
    return Steinberg::kResultOk;
  }

private:
  Steinberg::uint32 references_ = 1;
};
