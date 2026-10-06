#include "RealtimeWait.h"
#include <QtGlobal>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#else
#include <mach/mach_time.h>
#include <sys/event.h>
#endif
#endif

namespace airplay {
#ifdef Q_OS_WIN
RealtimeWait::RealtimeWait() : RealtimeWait(WindowsCalls{}) {}
RealtimeWait::RealtimeWait(WindowsCalls calls) : calls_(calls) {
  LARGE_INTEGER frequency{}, origin{};
  if (!QueryPerformanceFrequency(&frequency) ||
      !QueryPerformanceCounter(&origin))
    throw std::system_error(lastError(), std::system_category(), "Audio QPC");
  frequency_ = frequency.QuadPart;
  origin_ = origin.QuadPart;
  timer_ = calls_.createTimer(nullptr, nullptr,
                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                              TIMER_ALL_ACCESS);
  if (timer_)
    wake_ = calls_.createEvent(nullptr, FALSE, FALSE, nullptr);
  if (timer_ && wake_)
    return;
  const int error = lastError();
  if (timer_)
    calls_.close(timer_);
  throw std::system_error(error, std::system_category(),
                          "Audio high-resolution timer initialization");
}
#else
RealtimeWait::RealtimeWait() {
#ifdef Q_OS_LINUX
  timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (timer_ >= 0 && wake_ >= 0)
    return;
#else
  mach_timebase_info_data_t base{};
  mach_timebase_info(&base);
  numer_ = base.numer;
  denom_ = base.denom;
  queue_ = kqueue();
  if (queue_ >= 0 && fcntl(queue_, F_SETFD, FD_CLOEXEC) == 0) {
    struct kevent event{};
    EV_SET(&event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(queue_, &event, 1, nullptr, 0, nullptr) == 0)
      return;
  }
#endif
  const int error = errno;
  if (timer_ >= 0)
    close(timer_);
  if (wake_ >= 0)
    close(wake_);
  if (queue_ >= 0)
    close(queue_);
  throw std::system_error(error, std::generic_category(),
                          "Audio timer initialization");
}
#endif
RealtimeWait::~RealtimeWait() {
#ifdef Q_OS_WIN
  CancelWaitableTimer(timer_);
  calls_.close(timer_);
  calls_.close(wake_);
#else
  if (timer_ >= 0)
    close(timer_);
  if (wake_ >= 0)
    close(wake_);
  if (queue_ >= 0)
    close(queue_);
#endif
}
int RealtimeWait::lastError() noexcept {
#ifdef Q_OS_WIN
  return int(GetLastError());
#else
  return errno;
#endif
}
int64_t RealtimeWait::now() const noexcept {
#ifdef Q_OS_WIN
  LARGE_INTEGER value{};
  if (!QueryPerformanceCounter(&value))
    return -1;
  const auto ticks = value.QuadPart - origin_;
  return ticks / frequency_ * 1000000000 +
         int64_t(static_cast<long double>(ticks % frequency_) * 1000000000 /
                 frequency_);
#elif defined(Q_OS_LINUX)
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value))
    return -1;
  return int64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
#else
  const auto ticks = mach_absolute_time();
  return int64_t(ticks / denom_ * numer_ + ticks % denom_ * numer_ / denom_);
#endif
}
bool RealtimeWait::until(int64_t deadline) noexcept {
#ifdef Q_OS_WIN
  // Auto-reset wake events retain a wake-before-wait and coalesce repeated
  // requests. Put wake first so stop wins when both handles are signaled.
  HANDLE handles[]{wake_, timer_};
  for (;;) {
    const auto current = now();
    if (current < 0)
      return false;
    if (deadline <= current)
      return true;
    const auto remaining = deadline - current;
    LARGE_INTEGER due{};
    due.QuadPart = -(remaining / 100 + (remaining % 100 != 0));
    if (!calls_.arm(timer_, &due, 0, nullptr, nullptr, FALSE))
      return false;
    const auto result = calls_.wait(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0)
      return true;
    if (result == WAIT_OBJECT_0 + 1)
      continue; // Recheck the monotonic deadline, including early wakeups.
    if (result != WAIT_FAILED)
      SetLastError(ERROR_INVALID_DATA);
    return false;
  }
#elif defined(Q_OS_LINUX)
  itimerspec value{};
  value.it_value.tv_sec = deadline / 1000000000;
  value.it_value.tv_nsec = deadline % 1000000000;
  if (timerfd_settime(timer_, TFD_TIMER_ABSTIME, &value, nullptr))
    return false;
  pollfd fds[2]{{timer_, POLLIN, 0}, {wake_, POLLIN, 0}};
  int result;
  do {
    result = poll(fds, 2, -1);
  } while (result < 0 && errno == EINTR);
  if (result < 0 ||
      (fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL))
    return false;
  uint64_t count;
  for (auto &fd : fds)
    if (fd.revents & POLLIN) {
      ssize_t size;
      do {
        size = read(fd.fd, &count, sizeof(count));
      } while (size < 0 && errno == EINTR);
      if (size < 0 && errno != EAGAIN)
        return false;
    }
  return true;
#else
  const uint64_t ns = uint64_t(deadline);
  const auto ticks =
      ns / numer_ * denom_ + (ns % numer_ * denom_ + numer_ - 1) / numer_;
  struct kevent change{}, event{};
  EV_SET(&change, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
         NOTE_ABSOLUTE | NOTE_MACHTIME, intptr_t(ticks), nullptr);
  int result;
  do {
    result = kevent(queue_, &change, 1, &event, 1, nullptr);
  } while (result < 0 && errno == EINTR);
  if (result == 1 && (event.flags & EV_ERROR)) {
    errno = int(event.data);
    return false;
  }
  return result == 1;
#endif
}
void RealtimeWait::wake() noexcept {
#ifdef Q_OS_WIN
  SetEvent(wake_);
#elif defined(Q_OS_LINUX)
  const uint64_t one = 1;
  ssize_t result;
  do {
    result = write(wake_, &one, sizeof(one));
  } while (result < 0 && errno == EINTR);
  // EAGAIN means a wake is already pending.
#else
  struct kevent event{};
  EV_SET(&event, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
  int result;
  do {
    result = kevent(queue_, &event, 1, nullptr, 0, nullptr);
  } while (result < 0 && errno == EINTR);
#endif
}
} // namespace airplay
