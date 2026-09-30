#include "ui/MainWindow.h"
#include <QApplication>
#include <QMessageBox>
#include <stdexcept>
#ifdef AIRPLAY_CLI_TEST
#include "Cli.h"
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
int main(int argc, char **argv) {
  QApplication application(argc, argv);
  QApplication::setStyle("Fusion");
  QCoreApplication::setApplicationName("AirPlayQt");
#ifdef AIRPLAY_CLI_TEST
  if (application.arguments().contains("--cli"))
    return app::runCli(application);
#endif
  try {
#ifdef Q_OS_WIN
    // Apply before MainWindow creates the controller and network thread.
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
      const auto error = GetLastError();
      throw std::runtime_error(
          QString("设置进程 Realtime 优先级失败（Win32 %1）")
              .arg(error)
              .toStdString());
    }
    const auto priority = GetPriorityClass(GetCurrentProcess());
    if (priority == 0) {
      const auto error = GetLastError();
      throw std::runtime_error(
          QString("读取进程优先级失败（Win32 %1）").arg(error).toStdString());
    }
    if (priority != REALTIME_PRIORITY_CLASS)
      throw std::runtime_error(
          QString("Windows 未采用 Realtime 优先级（实际值 0x%1）")
              .arg(priority, 0, 16)
              .toStdString());
#endif
    ui::MainWindow window;
    window.show();
    return application.exec();
  } catch (const std::exception &e) {
    QMessageBox::critical(nullptr, "AirPlayQt", QString::fromUtf8(e.what()));
    return 1;
  }
}
