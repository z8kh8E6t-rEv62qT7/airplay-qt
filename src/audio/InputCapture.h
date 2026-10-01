#pragma once
#include "CaptureStream.h"
#include "app/Message.h"
#include <QList>
#include <QString>
#include <memory>

namespace audio {
enum class CaptureKind { Input, Loopback };
struct DriverInfo {
  QString id, name;
  CaptureKind kind = CaptureKind::Input;
  i18n::Message displayName() const {
    return kind == CaptureKind::Loopback
               ? i18n::text(i18n::Id::AutoLoopbackDevice).arg(name)
               : i18n::Message(name);
  }
};
struct ChannelInfo {
  int index;
  i18n::Message name;
  long type;
};
// The controller owns one native capture device. Stop completes before buffers
// can be reused; a nonempty result reports a native cleanup failure.
class InputCapture {
public:
  virtual ~InputCapture() = default;
  virtual QList<ChannelInfo> open(const QString &, void *window) = 0;
  virtual void controlPanel() = 0;
  virtual CaptureStream prepare(int left, int right, double maxBacklog) = 0;
  virtual void start() = 0;
  virtual i18n::Message stop() noexcept = 0;
  virtual i18n::Message close() noexcept = 0;
};
QList<DriverInfo> inputDevices();
std::unique_ptr<InputCapture>
createInputCapture(CaptureKind kind = CaptureKind::Input);
} // namespace audio
