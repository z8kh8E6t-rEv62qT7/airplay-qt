#pragma once
#include "BluezCatalog.h"
#include <QElapsedTimer>
#include <QSet>
#include <map>
#include <memory>

namespace audio {
// BlueZ's AVRCP uinput devices identify the adapter in phys and the remote
// device in the name. Some input implementations also supply a remote uniq.
QString bluetoothInputSource(const QString &name, const QString &phys, const QString &uniq,
                             const QList<BluetoothSource> &sources);

class BluetoothVolume : public QObject {
  Q_OBJECT
public:
  explicit BluetoothVolume(BluezCatalog &, QObject *parent = nullptr,
                           QString inputDirectory = "/dev/input");
  ~BluetoothVolume() override;
  void setSource(const QString &id);
  // Native input events are associated with a source before reaching this gate.
  void keyEvent(const QString &sourceId, int code, int value);
signals:
  void absoluteRequested(double db);
  void stepRequested(int direction);
  void log(QString text);
private:
  struct Input;
  void reconcile();
  void scan();
  void read(Input &);
  void reset();
  void report(const QString &text);
  BluezCatalog &catalog_;
  QString selected_, active_, inputDirectory_;
  QTimer scan_, steps_;
  QElapsedTimer absolute_;
  QList<int> pending_;
  QSet<QString> reported_;
  std::map<QString, std::unique_ptr<Input>> inputs_;
};
} // namespace audio
