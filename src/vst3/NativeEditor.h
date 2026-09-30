#pragma once
#include <QString>
#include <memory>
class QWidget;
namespace vst3 {
class NativeEditor {
public:
  NativeEditor(void *parent, QWidget *widget, const QString &error = {});
  ~NativeEditor();
  static const char *platformType();
  static bool usesLogicalCoordinates();
  void resize(int width, int height);
  void focus();

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace vst3
