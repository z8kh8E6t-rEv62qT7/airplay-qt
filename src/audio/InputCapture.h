#pragma once
#include "CaptureStream.h"
#include <QList>
#include <QString>
#include <memory>

namespace audio {
struct DriverInfo {
  QString id, name;
};
struct ChannelInfo {
  int index;
  QString name;
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
  virtual QString stop() noexcept = 0;
  virtual QString close() noexcept = 0;
};
QList<DriverInfo> inputDevices();
std::unique_ptr<InputCapture> createInputCapture();
} // namespace audio
