#pragma once
#include "BluezCatalog.h"
#include "InputCapture.h"
#include <pipewire/pipewire.h>
#include <map>
#include <optional>

namespace audio {
struct PipeWireSource {
  QString id, name, serial, codec;
  uint32_t node = SPA_ID_INVALID;
  uint32_t rate = 0, channels = 0;
  bool operator==(const PipeWireSource &) const = default;
};
std::optional<PipeWireSource> pipeWireSource(const QVariantMap &, uint32_t node,
    uint32_t rate, uint32_t channels, const QList<BluetoothSource> &);
class PipeWireCatalog : public QObject {
  Q_OBJECT
public:
  explicit PipeWireCatalog(QObject *parent = nullptr);
  ~PipeWireCatalog() override;
  QList<DriverInfo> devices() const;
  const QList<PipeWireSource> &sources() const { return sources_; }
  pw_thread_loop *loop() const { return loop_; }
  pw_core *core() const { return core_; }
  static PipeWireCatalog &instance();
signals:
  void changed();
  void disconnecting();
private:
  struct Node;
  struct Device;
  void tick();
  void connectServer();
  void disconnectServer();
  static void global(void *, uint32_t, uint32_t, const char *, uint32_t, const spa_dict *);
  static void removed(void *, uint32_t);
  static void coreError(void *, uint32_t, int, int, const char *);
  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_registry *registry_ = nullptr;
  spa_hook coreListener_{}, registryListener_{};
  std::map<uint32_t, std::unique_ptr<Node>> nodes_;
  std::map<uint32_t, std::unique_ptr<Device>> devices_;
  std::atomic<bool> lost_{false};
  BluezCatalog bluez_{this};
  QTimer timer_;
  int retryTicks_ = 0;
  QList<PipeWireSource> sources_;
};
// Main-loop operations are serialized; the realtime process callback never
// takes this lock. pw_stream_destroy joins/quiesces its data-loop work.
class PipeWireLock {
public:
  explicit PipeWireLock(pw_thread_loop *loop) : loop_(loop) { pw_thread_loop_lock(loop_); }
  ~PipeWireLock() { pw_thread_loop_unlock(loop_); }
private:
  pw_thread_loop *loop_;
};
} // namespace audio
