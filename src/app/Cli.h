#pragma once
class QApplication;
namespace app {
// macOS acceptance entry point in the same bundle as the GUI.
int runCli(QApplication &application);
} // namespace app
