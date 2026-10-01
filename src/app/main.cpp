#include "app/Message.h"
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
      throw i18n::MessageError(
          i18n::text(i18n::Id::FailedToSetRealtimeProcessPriorityWin)
              .arg(error));
    }
    const auto priority = GetPriorityClass(GetCurrentProcess());
    if (priority == 0) {
      const auto error = GetLastError();
      throw i18n::MessageError(
          i18n::text(i18n::Id::FailedToReadProcessPriorityWin).arg(error));
    }
    if (priority != REALTIME_PRIORITY_CLASS)
      throw i18n::MessageError(
          i18n::text(i18n::Id::WindowsDidNotApplyRealtimePriorityActual)
              .arg(priority, 0, 16));
#endif
    ui::MainWindow window;
    window.show();
    return application.exec();
  } catch (const std::exception &e) {
    QMessageBox::critical(nullptr, "AirPlayQt",
                          i18n::fromException(e).render());
    return 1;
  }
}
