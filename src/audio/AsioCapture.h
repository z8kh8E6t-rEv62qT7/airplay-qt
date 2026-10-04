#pragma once
#include "InputCapture.h"
#include "app/Message.h"
#include <QList>
#include <QString>
#include <memory>
#include <unknwn.h>
#include <vector>
#include <windows.h>
// Steinberg's Windows ABI uses pack(4); its header only applies this for MSVC.
#pragma pack(push, 4)
#include <iasiodrv.h>
#pragma pack(pop)

namespace audio {
class CaptureTiming;
struct CaptureTraceEntry {
  int64_t beginTicks = 0, endTicks = 0;
  uint64_t samplePosition = 0, systemTime = 0;
  uint64_t sourceBefore = 0, copied = 0, sourceAfter = 0;
  uint32_t threadId = 0, timeFlags = 0;
  long bufferIndex = 0, sampleFrames = 0;
  bool timeCallback = false, directProcess = false, queued = false;
};
// Allocate before start; one callback writer, read only after stop has
// returned.
struct CaptureTrace {
  explicit CaptureTrace(size_t capacity) : entries(capacity) {}
  std::vector<CaptureTraceEntry> entries;
  size_t used = 0, overflow = 0;
};
class AsioCapture final : public InputCapture {
public:
  AsioCapture();
  ~AsioCapture();
  AsioCapture(const AsioCapture &) = delete;
  AsioCapture &operator=(const AsioCapture &) = delete;
  static QList<DriverInfo> enumerate();
  QList<ChannelInfo> open(const QString &id, void *window);
  void controlPanel();
  CaptureStream prepare(int left, int right, int packetSamples,
                        int backlogSamples);
  void setTrace(CaptureTrace *trace);
  long callbackSamples() const noexcept { return blockFrames_; }
  void start();
  // Empty on success. Cleanup never throws; callers surface any timer error.
  i18n::Message stop() noexcept;
  i18n::Message close() noexcept;

private:
  // Tests inject an IASIO implementation; no registry registration is required.
  friend class AsioCaptureTestAccess;
  static void bufferSwitch(long index, ASIOBool);
  static ASIOTime *timeSwitch(ASIOTime *time, long index, ASIOBool);
  static void rateChanged(ASIOSampleRate rate);
  static long message(long selector, long value, void *, double *);
  void process(long index, const ASIOTime *time, ASIOBool directProcess,
               bool timeCallback) noexcept;
  void copy(long index, CaptureTraceEntry *trace) noexcept;
  void fault(int code) noexcept;
  static std::atomic<AsioCapture *> active_;
  IASIO *driver_ = nullptr;
  bool com_ = false, buffers_ = false, running_ = false;
  ASIOBufferInfo bufferInfo_[2]{};
  ASIOCallbacks callbacks_{};
  std::shared_ptr<CaptureQueue> queue_;
  std::unique_ptr<CaptureTiming> timing_;
  std::atomic<bool> accepting_{false};
  std::atomic_flag callbackBusy_ = ATOMIC_FLAG_INIT;
  QList<ChannelInfo> channels_;
  bool positionValid_ = false;
  uint64_t expectedPosition_ = 0;
  long blockFrames_ = 0;
  size_t leftBytes_ = 0, rightBytes_ = 0;
  CaptureTrace *trace_ = nullptr;
};
} // namespace audio
