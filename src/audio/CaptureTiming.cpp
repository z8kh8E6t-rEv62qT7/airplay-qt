#include "CaptureTiming.h"
#include "app/Message.h"
#include <QDebug>
#include <QString>
#include <mmsystem.h>
#include <stdexcept>

namespace audio {
namespace {
void fail(const i18n::Message &message) { throw i18n::MessageError(message); }
PROCESS_POWER_THROTTLING_STATE policy() {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                             &state, sizeof(state)))
    fail(i18n::text(i18n::Id::FailedToReadCaptureTimingPolicyWin)
             .arg(GetLastError()));
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
    fail(i18n::text(i18n::Id::FailedToDisableCaptureTimingThrottlingWin)
             .arg(GetLastError()));
  policyChanged_ = true;
  try {
    const auto status = timeBeginPeriod(1);
    if (status != TIMERR_NOERROR)
      fail(i18n::text(i18n::Id::FailedToRequestMsCaptureTimerResolution)
               .arg(status));
    timerActive_ = true;
    const auto current = policy();
    constexpr auto flag = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (!(current.ControlMask & flag) || (current.StateMask & flag))
      fail(i18n::text(i18n::Id::WindowsDidNotAcceptTheCaptureTiming));
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
    fail(i18n::text(i18n::Id::FailedToRestoreCaptureTimingStateMMRESULT)
             .arg(timerError)
             .arg(policyError));
}
void CaptureTiming::verifyRestored() const {
  const auto current = policy();
  if (current.ControlMask != original_.ControlMask ||
      (current.StateMask & original_.ControlMask) !=
          (original_.StateMask & original_.ControlMask) ||
      timerActive_ || policyChanged_)
    fail(i18n::text(i18n::Id::CaptureTimingPolicyWasNotRestored));
}
void CaptureTiming::restoreNoThrow() noexcept {
  try {
    restore();
  } catch (const std::exception &error) {
    qWarning("%s", error.what());
  }
}
} // namespace audio
