#include "CaptureTiming.h"
#include <QDebug>
#include <QString>
#include <mmsystem.h>
#include <stdexcept>

namespace audio {
namespace {
void fail(const QString &message) {
  throw std::runtime_error(message.toStdString());
}
PROCESS_POWER_THROTTLING_STATE policy() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                             &state, sizeof(state)))
    fail(QString("读取采集计时策略失败（Win32 %1）").arg(GetLastError()));
  return state;
}
} // namespace

CaptureTiming::CaptureTiming() : original_(policy()) {
  auto requested = original_;
  requested.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  requested.StateMask &= requested.ControlMask;
  requested.StateMask &= ~PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                             &requested, sizeof(requested)))
    fail(QString("禁用采集计时降精度策略失败（Win32 %1）").arg(GetLastError()));
  policyChanged_ = true;
  try {
    const auto status = timeBeginPeriod(1);
    if (status != TIMERR_NOERROR)
      fail(QString("申请 1 ms 采集计时精度失败（MMRESULT %1）").arg(status));
    timerActive_ = true;
    const auto current = policy();
    constexpr auto flag = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (!(current.ControlMask & flag) || (current.StateMask & flag))
      fail("Windows 未接受采集计时策略");
  } catch (...) {
    restoreNoThrow();
    throw;
  }
}
CaptureTiming::~CaptureTiming() { restoreNoThrow(); }

void CaptureTiming::restore() {
  MMRESULT timerError = TIMERR_NOERROR;
  DWORD policyError = ERROR_SUCCESS;
  if (timerActive_) {
    timerError = timeEndPeriod(1);
    if (timerError == TIMERR_NOERROR)
      timerActive_ = false;
  }
  if (policyChanged_) {
    auto previous = original_;
    // System-managed bits must remain managed by Windows, not be pinned to
    // the inferred state from our earlier snapshot.
    previous.StateMask &= previous.ControlMask;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &previous, sizeof(previous)))
      policyChanged_ = false;
    else
      policyError = GetLastError();
  }
  if (timerError != TIMERR_NOERROR || policyError != ERROR_SUCCESS)
    fail(QString("恢复采集计时状态失败（MMRESULT %1，Win32 %2）")
             .arg(timerError)
             .arg(policyError));
}
void CaptureTiming::verifyRestored() const {
  const auto current = policy();
  if (current.ControlMask != original_.ControlMask ||
      (current.StateMask & original_.ControlMask) !=
          (original_.StateMask & original_.ControlMask) ||
      timerActive_ || policyChanged_)
    fail("采集计时策略未恢复到原状态");
}
void CaptureTiming::restoreNoThrow() noexcept {
  try {
    restore();
  } catch (const std::exception &error) {
    qWarning("%s", error.what());
  }
}
} // namespace audio
