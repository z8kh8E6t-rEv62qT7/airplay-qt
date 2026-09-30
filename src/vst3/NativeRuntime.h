#pragma once
#include <functional>
#include <memory>
namespace vst3 {
// Owns only this plugin's event source. The host retains its native event loop.
class NativeRuntime {
public:
  explicit NativeRuntime(std::function<void()> tick);
  ~NativeRuntime();
  static bool validParentThread(void *parent);
  bool onThread() const;
  void invoke(const std::function<void()> &);

private:
  struct State;
  std::unique_ptr<State> state_;
};
void beginSessionTiming();
bool endSessionTiming();
} // namespace vst3
