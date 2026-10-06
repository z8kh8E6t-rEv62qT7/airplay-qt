#include "ThreadScheduling.h"
#include <QFile>
#include <QRegularExpression>
#include <cerrno>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>

#include <avrt.h>
#else
#include <pthread.h>
#include <sys/utsname.h>
#endif
#ifdef Q_OS_MACOS
#include <pthread/qos.h>
#endif

namespace airplay {
#ifndef Q_OS_WIN
static SchedulingResult configureAudioScheduling();
#endif
#ifdef Q_OS_WIN
AudioThreadScheduling::AudioThreadScheduling()
    : AudioThreadScheduling(WindowsCalls{}) {}
AudioThreadScheduling::AudioThreadScheduling(WindowsCalls calls)
    : calls_(calls) {
  try {
    DWORD index = 0;
    task_ = calls_.registerTask(L"Pro Audio", &index);
    const DWORD registrationError = task_ ? ERROR_SUCCESS : GetLastError();
    const bool accepted =
        task_ && calls_.setPriority(task_, AVRT_PRIORITY_HIGH);
    const DWORD priorityError =
        task_ && !accepted ? GetLastError() : ERROR_SUCCESS;
    result_.ready = accepted;
    result_.description =
        QString("Windows audio scheduling: requested=MMCSS/Pro Audio/HIGH "
                "registered=%1 priorityAccepted=%2 taskIndex=%3 "
                "registrationError=%4 priorityError=%5")
            .arg(task_ ? "yes" : "no")
            .arg(result_.ready ? "yes" : "no")
            .arg(index)
            .arg(registrationError)
            .arg(priorityError);
  } catch (...) {
    if (task_)
      calls_.revert(task_);
    throw;
  }
}
#else
AudioThreadScheduling::AudioThreadScheduling() {
  result_ = configureAudioScheduling();
}
#endif
AudioThreadScheduling::~AudioThreadScheduling() {
#ifdef Q_OS_WIN
  if (task_ && !calls_.revert(task_))
    qWarning("MMCSS revert failed (Win32 %lu)", GetLastError());
#endif
}
QString KernelRealtime::description() const {
  return QString("Linux kernel: release=%1 PREEMPT_RT=%2 source=%3")
      .arg(release, realtime ? "yes" : "no", source);
}
KernelRealtime classifyKernelRealtime(const std::optional<QString> &sysfs,
                                      const QString &release) {
  if (sysfs && (sysfs->trimmed() == "0" || sysfs->trimmed() == "1"))
    return {sysfs->trimmed() == "1", release, "/sys/kernel/realtime"};
  static const QRegularExpression marker("-rt[0-9]*(?:-|$)");
  return {marker.match(release).hasMatch(), release,
          sysfs ? "uname (-rt fallback; invalid sysfs value)"
                : "uname (-rt fallback; sysfs unavailable)"};
}
KernelRealtime kernelRealtime() {
#ifdef Q_OS_LINUX
  QFile file("/sys/kernel/realtime");
  std::optional<QString> contents;
  if (file.open(QIODevice::ReadOnly))
    contents = QString::fromLatin1(file.read(32));
  utsname name{};
  const auto release = uname(&name) == 0 ? QString::fromLocal8Bit(name.release)
                                         : QString("unknown");
  return classifyKernelRealtime(contents, release);
#else
  return {false, {}, "not Linux"};
#endif
}
bool realtimeSchedulingAccepted(bool required, int setError, int readError,
                                bool fifo, int priority) noexcept {
  return !required ||
         (setError == 0 && readError == 0 && fifo && priority == 80);
}
#ifndef Q_OS_WIN
static QString failure(int error) {
  if (!error)
    return "none";
  return QString("%1 (%2%3)")
      .arg(QString::fromLocal8Bit(std::strerror(error)))
      .arg(error == EPERM ? "EPERM/" : "")
      .arg(error);
}
#endif
SchedulingResult configureNetworkScheduling() {
#ifdef Q_OS_MACOS
  const int setError =
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  qos_class_t actual = QOS_CLASS_UNSPECIFIED;
  int relative = 0;
  const int readError =
      pthread_get_qos_class_np(pthread_self(), &actual, &relative);
  return {true, QString("macOS thread QoS: requested=USER_INTERACTIVE "
                        "actual=%1 relative=%2 set=%3 read=%4")
                    .arg(actual == QOS_CLASS_USER_INTERACTIVE
                             ? "USER_INTERACTIVE"
                             : QString::number(actual))
                    .arg(relative)
                    .arg(failure(setError), failure(readError))};
#else
  return {};
#endif
}
#ifndef Q_OS_WIN
static SchedulingResult configureAudioScheduling() {
#ifdef Q_OS_LINUX
  pthread_setname_np(pthread_self(), "AirPlayQt audio");
  const auto kernel = kernelRealtime();
  sched_param requested{};
  requested.sched_priority = 80;
  const int setError =
      kernel.realtime
          ? pthread_setschedparam(pthread_self(), SCHED_FIFO, &requested)
          : 0;
  sched_param actual{};
  int policy = -1;
  const int readError = pthread_getschedparam(pthread_self(), &policy, &actual);
  const auto policyName = policy == SCHED_FIFO    ? "SCHED_FIFO"
                          : policy == SCHED_RR    ? "SCHED_RR"
                          : policy == SCHED_OTHER ? "SCHED_OTHER"
                                                  : "unknown";
  return {
      realtimeSchedulingAccepted(kernel.realtime, setError, readError,
                                 policy == SCHED_FIFO, actual.sched_priority),
      kernel.description() +
          QString(
              "; audio scheduling: requested=%1 actual=%2/%3 set=%4 read=%5")
              .arg(kernel.realtime ? "SCHED_FIFO/80"
                                   : "unchanged (non-RT kernel)",
                   policyName)
              .arg(actual.sched_priority)
              .arg(failure(setError), failure(readError))};
#elif defined(Q_OS_MACOS)
  pthread_setname_np("AirPlayQt audio");
  return configureNetworkScheduling();
#else
  return {};
#endif
}
#endif
} // namespace airplay
