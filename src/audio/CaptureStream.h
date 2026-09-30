#pragma once
#include "CaptureQueue.h"
#include "PcmConverter.h"
#include <memory>

namespace audio {
struct CaptureStream {
  std::shared_ptr<CaptureQueue> queue;
  PcmFormat left, right;
  long blockFrames = 0;
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  // Opt-in diagnostics for VST host pacing; ASIO streams leave this disabled.
  bool rateDiagnostics = false;
#endif
};
} // namespace audio
