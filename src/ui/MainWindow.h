#pragma once
#include "StreamingPanel.h"
#include "app/Controller.h"
#include <QComboBox>
#include <QMainWindow>

namespace ui {
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(const airplay::DiscoveryApi &discoveryApi = airplay::defaultDiscoveryApi());

protected:
  void closeEvent(QCloseEvent *) override;

private:
  void setDevices(const QList<audio::DriverInfo> &);
  void setChannels(const QList<audio::ChannelInfo> &);
  Q_SLOT void setBusy(bool);
  app::Controller controller_;
  app::Settings saved_;
  QComboBox *driver_, *left_, *right_;
  QPushButton *panel_;
  StreamingPanel *streaming_;
  bool loading_ = true, closing_ = false;
};
} // namespace ui
