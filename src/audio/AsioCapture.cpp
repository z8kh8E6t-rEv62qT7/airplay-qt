#include "AsioCapture.h"
#include "CaptureTiming.h"
#include "app/Message.h"
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace audio {
static_assert(sizeof(ASIOBufferInfo) == 24);
static_assert(offsetof(AsioTimeInfo, sampleRate) == 24);
static_assert(sizeof(ASIOTime) == 148);
static void fail(const i18n::Message &text) { throw i18n::MessageError(text); }
static void check(ASIOError result, const i18n::Message &operation) {
  if (result != ASE_OK)
    fail(i18n::text(i18n::Id::AsioOperationFailed).arg(operation).arg(result));
}
struct RegistryKey {
  HKEY handle = nullptr;
  ~RegistryKey() {
    if (handle)
      RegCloseKey(handle);
  }
};
static QString registryString(HKEY key, const wchar_t *name) {
  wchar_t value[1024]{};
  DWORD size = sizeof(value), type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type,
                       reinterpret_cast<BYTE *>(value),
                       &size) != ERROR_SUCCESS ||
      type != REG_SZ || size < sizeof(wchar_t) || size > sizeof(value) ||
      size % sizeof(wchar_t) || value[size / sizeof(wchar_t) - 1] != 0)
    return {};
  return QString::fromWCharArray(value);
}
std::atomic<AsioCapture *> AsioCapture::active_{nullptr};
AsioCapture::AsioCapture() {
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr))
    fail(i18n::text(i18n::Id::ComInitializationFailed)
             .arg(uint32_t(hr), 8, 16, QChar('0')));
  com_ = true;
}
AsioCapture::~AsioCapture() {
  close();
  timing_.reset();
  auto *expected = this;
  active_.compare_exchange_strong(expected, nullptr);
  if (com_)
    CoUninitialize();
}
QList<DriverInfo> AsioCapture::enumerate() {
  RegistryKey root;
  const auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                                    KEY_READ | KEY_WOW64_64KEY, &root.handle);
  if (result == ERROR_FILE_NOT_FOUND)
    return {};
  if (result != ERROR_SUCCESS)
    fail(i18n::text(i18n::Id::CannotReadTheXASIORegistry));
  QList<DriverInfo> drivers;
  for (DWORD index = 0;; ++index) {
    wchar_t name[256];
    DWORD size = 256;
    const auto status = RegEnumKeyExW(root.handle, index, name, &size, nullptr,
                                      nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS)
      break;
    if (status != ERROR_SUCCESS)
      fail(i18n::text(i18n::Id::ASIORegistryEnumerationFailed));
    RegistryKey entry;
    if (RegOpenKeyExW(root.handle, name, 0, KEY_READ | KEY_WOW64_64KEY,
                      &entry.handle) != ERROR_SUCCESS)
      continue;
    auto id = registryString(entry.handle, L"CLSID");
    auto description = registryString(entry.handle, L"Description");
    if (!id.isEmpty())
      drivers.append({id, description.isEmpty() ? QString::fromWCharArray(name)
                                                : description});
  }
  std::sort(drivers.begin(), drivers.end(),
            [](const auto &a, const auto &b) { return a.name < b.name; });
  return drivers;
}
QList<ChannelInfo> AsioCapture::open(const QString &id, void *window) {
  if (const auto error = close(); !error.isEmpty())
    fail(error);
  CLSID clsid{};
  auto text = id.toStdWString();
  if (FAILED(CLSIDFromString(text.c_str(), &clsid)))
    fail(i18n::text(i18n::Id::InvalidASIOCLSID));
  const HRESULT hr =
      CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                       reinterpret_cast<void **>(&driver_));
  if (FAILED(hr))
    fail(i18n::text(i18n::Id::AsioDriverLoadFailed)
             .arg(uint32_t(hr), 8, 16, QChar('0')));
  try {
    if (!driver_->init(window))
      fail(i18n::text(
          i18n::Id::ASIODriverInitializationFailedAnotherApplicationMay));
    long inputs = 0, outputs = 0;
    check(driver_->getChannels(&inputs, &outputs), "getChannels");
    if (inputs < 2 || inputs > 65536)
      fail(i18n::text(i18n::Id::ASIODriverDidNotProvideTwoValid));
    for (long index = 0; index < inputs; ++index) {
      ASIOChannelInfo info{};
      info.channel = index;
      info.isInput = ASIOTrue;
      check(driver_->getChannelInfo(&info), "getChannelInfo");
      info.name[sizeof(info.name) - 1] = 0;
      channels_.append(
          {int(index), QString::fromLocal8Bit(info.name), info.type});
    }
  } catch (...) {
    close();
    throw;
  }
  return channels_;
}
void AsioCapture::controlPanel() {
  if (!driver_ || buffers_)
    fail(i18n::text(i18n::Id::SelectADriverAndStopCaptureFirst));
  check(driver_->controlPanel(), "controlPanel");
}
CaptureStream AsioCapture::prepare(int left, int right, double backlog) {
  if (!driver_ || buffers_ || timing_ || left < 0 ||
      right < 0 || left >= channels_.size() || right >= channels_.size())
    fail(i18n::text(i18n::Id::InvalidASIODriverOrInputChannelSelection));
  if (!std::isfinite(backlog) || backlog < .01 || backlog > 1)
    fail(i18n::text(i18n::Id::InvalidMaximumBacklog));
  AsioCapture *expected = nullptr;
  if (!active_.compare_exchange_strong(expected, this))
    fail(i18n::text(i18n::Id::AnASIOSessionIsAlreadyActive));
  try {
    timing_ = std::make_unique<CaptureTiming>();
    check(driver_->canSampleRate(44100), "canSampleRate(44100)");
    const auto setResult = driver_->setSampleRate(44100);
    double rate = 0;
    if (setResult != ASE_OK) {
      const auto getResult = driver_->getSampleRate(&rate);
      fail(i18n::text(i18n::Id::SetSampleRateFailedASIOCurrentRateHzQuery)
               .arg(setResult)
               .arg(rate)
               .arg(getResult));
    }
    check(driver_->getSampleRate(&rate), "getSampleRate");
    if (rate != 44100)
      fail(i18n::text(i18n::Id::ASIODidNotAdoptKHzCheckThe));
    long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    check(driver_->getBufferSize(&minimum, &maximum, &preferred, &granularity),
          "getBufferSize");
    if (minimum <= 0 || maximum < minimum || preferred < minimum ||
        preferred > maximum || preferred > 44100 || granularity < -1 ||
        (granularity == -1 && (preferred & (preferred - 1))) ||
        (granularity > 0 && (preferred - minimum) % granularity))
      fail(i18n::text(i18n::Id::InvalidPreferredASIOBufferSize));
    if (preferred > backlog * 44100)
      fail(i18n::text(i18n::Id::ASIOBufferDurationExceedsMaximumBacklogAdjust));
    PcmFormat formats[2];
    int indices[2]{left, right};
    for (int i = 0; i < 2; ++i) {
      ASIOChannelInfo info{};
      info.channel = indices[i];
      info.isInput = ASIOTrue;
      check(driver_->getChannelInfo(&info), "getChannelInfo");
      formats[i] = format(info.type);
      bufferInfo_[i] = {ASIOTrue, indices[i], {nullptr, nullptr}};
    }
    blockFrames_ = preferred;
    leftBytes_ = size_t(preferred) * formats[0].bytes;
    rightBytes_ = size_t(preferred) * formats[1].bytes;
    queue_ = std::make_shared<CaptureQueue>(
        preferred, preferred * formats[0].bytes, preferred * formats[1].bytes,
        size_t(std::ceil(backlog * 44100 / preferred)) + 2);
    callbacks_ = {bufferSwitch, rateChanged, message, timeSwitch};
    check(driver_->createBuffers(bufferInfo_, left == right ? 1 : 2, preferred,
                                 &callbacks_),
          "createBuffers");
    buffers_ = true;
    // Both output sides share one driver buffer when the input is identical.
    if (left == right)
      bufferInfo_[1] = bufferInfo_[0];
    for (const auto &info : bufferInfo_)
      if (!info.buffers[0] || !info.buffers[1])
        fail(i18n::text(i18n::Id::ASIOReturnedANullInputBuffer));
    check(driver_->getSampleRate(&rate), "getSampleRate");
    if (rate != 44100)
      fail(i18n::text(i18n::Id::ASIOSampleRateChangedAfterBufferCreation));
    return {queue_, formats[0], formats[1], preferred};
  } catch (const std::exception &error) {
    const auto cleanup = stop();
    if (!cleanup.isEmpty())
      fail(i18n::fromException(error) + "\n" + cleanup);
    throw;
  } catch (...) {
    stop();
    throw;
  }
}
void AsioCapture::setTrace(CaptureTrace *trace) {
  if (running_)
    fail(i18n::text(i18n::Id::CannotReplaceCallbackDiagnosticsDuringCapture));
  trace_ = trace;
  if (trace_) {
    trace_->used = 0;
    trace_->overflow = 0;
  }
}
void AsioCapture::start() {
  if (!driver_ || !buffers_ || running_)
    fail(i18n::text(i18n::Id::InvalidASIOStartState));
  positionValid_ = false;
  accepting_.store(true, std::memory_order_release);
  // A driver may invoke its callback before start() returns.
  running_ = true;
  try {
    check(driver_->start(), "start");
  } catch (const std::exception &error) {
    const auto cleanup = stop();
    if (!cleanup.isEmpty())
      fail(i18n::fromException(error) + "\n" + cleanup);
    throw;
  }
}
i18n::Message AsioCapture::stop() noexcept {
  accepting_.store(false, std::memory_order_release);
  if (driver_ && running_)
    driver_->stop();
  running_ = false;
  if (driver_ && buffers_)
    driver_->disposeBuffers();
  buffers_ = false;
  trace_ = nullptr;
  queue_.reset();
  if (timing_) {
    try {
      timing_->restore();
      timing_->verifyRestored();
      timing_.reset();
    } catch (const std::exception &error) {
      // Retain ownership and the session guard for a later cleanup retry.
      // Never replace an unrestored policy snapshot with a new session's.
      qWarning("%s", error.what());
      return i18n::fromException(error);
    }
  }
  auto *expected = this;
  active_.compare_exchange_strong(expected, nullptr);
  return {};
}
i18n::Message AsioCapture::close() noexcept {
  const auto error = stop();
  if (driver_)
    driver_->Release();
  driver_ = nullptr;
  channels_.clear();
  return error;
}
void AsioCapture::fault(int code) noexcept {
  if (queue_) {
    int expected = 0;
    queue_->fault.compare_exchange_strong(expected, code);
  }
}
void AsioCapture::copy(long index, CaptureTraceEntry *trace) noexcept {
  if (!accepting_.load(std::memory_order_acquire))
    return;
  if (index < 0 || index > 1) {
    fault(1);
    return;
  }
  const std::span<const std::byte> left{
      static_cast<const std::byte *>(bufferInfo_[0].buffers[index]),
      leftBytes_};
  const std::span<const std::byte> right{
      static_cast<const std::byte *>(bufferInfo_[1].buffers[index]),
      rightBytes_};
  if (trace)
    trace->sourceBefore = captureChecksum(left, right);
  const bool queued =
      queue_->push(left.data(), right.data(), trace ? &trace->copied : nullptr);
  if (trace) {
    trace->queued = queued;
    trace->sourceAfter = captureChecksum(left, right);
  }
  if (!queued)
    fault(2);
}
void AsioCapture::process(long index, const ASIOTime *time,
                          ASIOBool directProcess, bool timeCallback) noexcept {
  if (callbackBusy_.test_and_set(std::memory_order_acquire)) {
    fault(3);
    return;
  }
  CaptureTraceEntry *entry = nullptr;
  if (trace_ && accepting_.load(std::memory_order_acquire)) {
    if (trace_->used < trace_->entries.size()) {
      entry = &trace_->entries[trace_->used++];
      *entry = {};
      LARGE_INTEGER ticks;
      QueryPerformanceCounter(&ticks);
      entry->beginTicks = ticks.QuadPart;
      entry->threadId = GetCurrentThreadId();
      entry->bufferIndex = index;
      entry->timeCallback = timeCallback;
      entry->directProcess = directProcess != ASIOFalse;
      if (time) {
        entry->timeFlags = time->timeInfo.flags;
        entry->samplePosition =
            (uint64_t(time->timeInfo.samplePosition.hi) << 32) |
            time->timeInfo.samplePosition.lo;
        entry->systemTime = (uint64_t(time->timeInfo.systemTime.hi) << 32) |
                            time->timeInfo.systemTime.lo;
      }
    } else {
      ++trace_->overflow;
    }
  }
  if (time && accepting_.load(std::memory_order_acquire)) {
    const auto &info = time->timeInfo;
    if ((info.flags & kSampleRateChanged) ||
        ((info.flags & kSampleRateValid) && info.sampleRate != 44100) ||
        (info.flags & kClockSourceChanged))
      fault(4);
    if (info.flags & kSamplePositionValid) {
      const uint64_t position =
          (uint64_t(info.samplePosition.hi) << 32) | info.samplePosition.lo;
      if (positionValid_ && position != expectedPosition_)
        fault(5);
      expectedPosition_ = position + blockFrames_;
      positionValid_ = true;
    }
  }
  copy(index, entry);
  if (entry) {
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    entry->endTicks = ticks.QuadPart;
  }
  callbackBusy_.clear(std::memory_order_release);
}
void AsioCapture::bufferSwitch(long index, ASIOBool directProcess) {
  if (auto *self = active_.load(std::memory_order_acquire))
    self->process(index, nullptr, directProcess, false);
}
ASIOTime *AsioCapture::timeSwitch(ASIOTime *time, long index,
                                  ASIOBool directProcess) {
  if (auto *self = active_.load(std::memory_order_acquire))
    self->process(index, time, directProcess, true);
  return time;
}
void AsioCapture::rateChanged(ASIOSampleRate rate) {
  if (rate != 44100)
    if (auto *self = active_.load(std::memory_order_acquire))
      self->fault(4);
}
long AsioCapture::message(long selector, long value, void *, double *) {
  if (selector == kAsioSelectorSupported)
    return value == kAsioEngineVersion || value == kAsioSupportsTimeInfo ||
           value == kAsioResetRequest || value == kAsioResyncRequest ||
           value == kAsioOverload;
  if (selector == kAsioEngineVersion)
    return 2;
  if (selector == kAsioSupportsTimeInfo)
    return 1;
  if (selector == kAsioResetRequest || selector == kAsioResyncRequest ||
      selector == kAsioOverload) {
    if (auto *self = active_.load(std::memory_order_acquire))
      self->fault(6);
    return 1;
  }
  return 0;
}
} // namespace audio

namespace audio {
QList<DriverInfo> inputDevices() { return AsioCapture::enumerate(); }
std::unique_ptr<InputCapture> createInputCapture(CaptureKind kind) {
  if (kind != CaptureKind::Input)
    throw i18n::MessageError(i18n::text(i18n::Id::SelectAValidInputDevice));
  return std::make_unique<AsioCapture>();
}
} // namespace audio
