#include "PipeWireCapture.h"
#include "BluetoothVolume.h"
#include "PipeWireBuffer.h"
#include "PipeWireCatalog.h"
#include "PipeWireRecovery.h"
#include <QTimer>
#include <spa/buffer/meta.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <cstring>

namespace audio {
struct PipeWireCapture::State {
  PipeWireCapture &owner;
  PipeWireCatalog &catalog = PipeWireCatalog::instance();
  BluetoothVolume volume{catalog.bluetooth()};
  QString selected, lastLog;
  PipeWireSource bound;
  QTimer timer;
  bool running = false;
  PipeWireRetry retry;
  std::shared_ptr<CaptureQueue> queue;
  std::unique_ptr<PipeWireBuffer> buffer;
  pw_stream *stream = nullptr;
  spa_hook listener{};
  // Format/state callbacks use the PipeWire main loop; GUI reads under its lock.
  QString nativeError;
  spa_audio_info_raw format{};
  pw_stream_state state = PW_STREAM_STATE_UNCONNECTED;
  std::atomic<bool> formatValid{false};
  std::atomic<bool> receiving{false};
  std::atomic<int> processError{0};
  std::atomic<uint64_t> processed{0};
  uint64_t loggedFrames = 0;
  int left = 0, right = 1;
  explicit State(PipeWireCapture &o) : owner(o) {
    QObject::connect(&volume, &BluetoothVolume::absoluteRequested, &owner, &InputCapture::volumeRequested);
    QObject::connect(&volume, &BluetoothVolume::stepRequested, &owner, &InputCapture::volumeStepRequested);
    QObject::connect(&volume, &BluetoothVolume::log, &owner, [this](const QString &text) {
      emit owner.log(i18n::text(i18n::Id::LinuxInputLog).arg(text));
    });
    QObject::connect(&timer, &QTimer::timeout, &owner, [this] { tick(); });
    QObject::connect(&catalog, &PipeWireCatalog::changed, &owner, [this] {
      emit owner.devicesChanged();
      if (running) tick();
    });
    QObject::connect(&catalog, &PipeWireCatalog::disconnecting, &owner, [this] { destroyStream(); });
  }
  void log(const QString &text) {
    if (text == lastLog) return;
    lastLog = text;
    emit owner.log(i18n::text(i18n::Id::LinuxInputLog).arg(text));
  }
  void destroyStream() {
    receiving.store(false, std::memory_order_release);
    formatValid.store(false, std::memory_order_release);
    if (stream) {
      PipeWireLock lock(catalog.loop());
      spa_hook_remove(&listener);
      pw_stream_set_active(stream, false);
      pw_stream_disconnect(stream);
      pw_stream_destroy(stream);
      stream = nullptr;
    }
    if (queue) queue->generation.fetch_add(1, std::memory_order_acq_rel);
    buffer.reset();
    bound = {};
    processError.store(0);
  }
  static void stateChanged(void *data, pw_stream_state previous, pw_stream_state state, const char *error) {
    auto &s = *static_cast<State *>(data);
    s.state = state;
    s.receiving.store(state == PW_STREAM_STATE_STREAMING, std::memory_order_release);
    s.nativeError = QString::fromUtf8(error ? error : "");
    if (pipeWireStateInvalidates(previous, state) && s.queue)
      s.queue->generation.fetch_add(1, std::memory_order_acq_rel);
    if (state == PW_STREAM_STATE_UNCONNECTED && previous != PW_STREAM_STATE_UNCONNECTED)
      s.processError.store(ENOTCONN, std::memory_order_release);
  }
  static void paramChanged(void *data, uint32_t id, const spa_pod *param) {
    if (id != SPA_PARAM_Format) return;
    auto &s = *static_cast<State *>(data);
    spa_audio_info_raw next{};
    const bool valid = param && spa_format_audio_raw_parse(param, &next) >= 0 &&
        next.format == SPA_AUDIO_FORMAT_F32 && next.rate == 44100 && next.channels == 2 &&
        next.position[0] == SPA_AUDIO_CHANNEL_FL && next.position[1] == SPA_AUDIO_CHANNEL_FR;
    // Repeated notifications of the fixed negotiated format are not gaps.
    const bool wasValid = s.formatValid.exchange(valid, std::memory_order_acq_rel);
    s.format = next;
    if (wasValid && !valid && s.queue)
      s.queue->generation.fetch_add(1, std::memory_order_acq_rel);
    if (param && !valid) {
      s.processError.store(EINVAL, std::memory_order_release);
    }
  }
  static void process(void *data) {
    auto &s = *static_cast<State *>(data);
    auto *packet = pw_stream_dequeue_buffer(s.stream);
    if (!packet) return;
    auto *audio = packet->buffer;
    if (s.receiving.load(std::memory_order_acquire) && s.formatValid.load(std::memory_order_acquire) &&
        !s.processError.load(std::memory_order_acquire)) {
      bool valid = audio->n_datas == 1;
      auto *d = valid ? &audio->datas[0] : nullptr;
      valid = valid && d->data && d->chunk && d->chunk->offset <= d->maxsize &&
          d->chunk->size <= d->maxsize - d->chunk->offset && d->chunk->size % (2 * sizeof(float)) == 0 &&
          (d->chunk->stride == 0 || d->chunk->stride == 2 * int(sizeof(float))) &&
          !(d->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) &&
          d->chunk->offset % alignof(float) == 0;
      if (valid && !(d->chunk->flags & SPA_CHUNK_FLAG_EMPTY)) {
        auto *header = static_cast<spa_meta_header *>(spa_buffer_find_meta_data(audio, SPA_META_Header, sizeof(spa_meta_header)));
        if (header && (header->flags & SPA_META_HEADER_FLAG_DISCONT))
          s.buffer->invalidate();
        const auto frames = d->chunk->size / (2 * sizeof(float));
        const auto correction = s.buffer->rate(
            double(s.queue->bufferedFrames.load(std::memory_order_acquire)),
            double(s.queue->targetFrames.load(std::memory_order_acquire)), uint32_t(frames),
            s.queue->playbackActive.load(std::memory_order_acquire));
        const int result = pw_stream_set_rate(s.stream, correction);
        if (result < 0) {
          s.buffer->invalidate();
          s.processError.store(-result, std::memory_order_release);
        } else if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) {
          s.buffer->invalidate();
        } else if (s.buffer->append(reinterpret_cast<const float *>(
                       static_cast<const std::byte *>(d->data) + d->chunk->offset), frames)) {
          s.processed.fetch_add(frames, std::memory_order_relaxed);
        }
      } else if (!valid) {
        s.buffer->invalidate();
        s.processError.store(EBADMSG, std::memory_order_release);
      }
    }
    pw_stream_queue_buffer(s.stream, packet);
  }
  void createStream(const PipeWireSource &source) {
    bound = source;
    processError.store(0);
    processed.store(0);
    loggedFrames = 0;
    buffer = std::make_unique<PipeWireBuffer>(*queue, left, right);
    PipeWireLock lock(catalog.loop());
    state = PW_STREAM_STATE_UNCONNECTED; nativeError.clear(); format = {};
    const auto serial = source.serial.toUtf8();
    auto *props = pw_properties_new(PW_KEY_APP_NAME, "AirPlayQt", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_TARGET_OBJECT, serial.constData(), PW_KEY_NODE_DONT_RECONNECT, "true",
        "node.dont-fallback", "true", "node.latency", "352/44100",
        "resample.disable", "false", nullptr);
    stream = pw_stream_new(catalog.core(), "AirPlayQt input", props);
    if (!stream) { processError.store(ENOMEM); return; }
    static const pw_stream_events events = [] {
      pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS;
      e.state_changed = stateChanged; e.param_changed = paramChanged; e.process = process; return e;
    }();
    pw_stream_add_listener(stream, &listener, &events, this);
    uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_audio_info_raw requested{};
    requested.format = SPA_AUDIO_FORMAT_F32; requested.rate = 44100; requested.channels = 2;
    requested.position[0] = SPA_AUDIO_CHANNEL_FL; requested.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &requested)};
    const int result = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                        PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_DONT_RECONNECT), params, 1);
    if (result < 0) processError.store(-result);
  }
  void tick() {
    if (!running) return;
    const auto &sources = catalog.sources();
    const auto found = std::find_if(sources.begin(), sources.end(), [this](const auto &s) { return s.id == selected; });
    if (found == sources.end() || !catalog.core()) {
      if (stream) destroyStream();
      retry.clear();
      log("Waiting: " + selected + (catalog.core() ? "; selected node absent" : "; PipeWire server unavailable") +
          "; AirPlay continues with silence");
      return;
    }
    if (stream && !samePipeWireTarget(bound, *found)) {
      log("Rebuilding selected input: target node or codec changed");
      destroyStream();
    }
    if (stream) bound = *found;
    if (!stream) {
      if (retry.waiting(*found)) return;
      log(QString("Negotiating %1; codec=%2; source PCM rate=%3; requested=44100 Hz, Float32, FL/FR")
          .arg(selected, found->codec.isEmpty() ? "unknown" : found->codec,
               found->rate ? QString::number(found->rate) : "unknown"));
      createStream(*found);
    }
    QString error;
    pw_stream_state current;
    spa_audio_info_raw actual;
    {
      PipeWireLock lock(catalog.loop());
      error = nativeError; current = state; actual = format;
    }
    const int failure = processError.load(std::memory_order_acquire);
    if (failure || current == PW_STREAM_STATE_ERROR) {
      const auto reason = failure == EINVAL ? "format mismatch" :
          failure == ENOTCONN ? "capture stream disconnected" : "capture stream failure";
      log(QString("Input unavailable (%6): %1 (code %2); actual=%3 Hz, %4 channels, format=%5; retry in 2000 ms unless target changes; sending silence")
          .arg(error.isEmpty() ? QString::fromLocal8Bit(std::strerror(failure ? failure : EIO)) : error)
          .arg(failure)
          .arg(actual.rate ? QString::number(actual.rate) : "unknown")
          .arg(actual.channels ? QString::number(actual.channels) : "unknown")
          .arg(actual.format == SPA_AUDIO_FORMAT_UNKNOWN ? "unknown" : QString::number(actual.format))
          .arg(reason));
      retry.failed(*found);
      destroyStream();
    } else if (current == PW_STREAM_STATE_STREAMING && actual.rate && processed.load() != loggedFrames) {
      loggedFrames = processed.load();
      log(QString("Receiving %1; codec=%2; source PCM rate=%3; requested=44100 Hz; obtained=%4 Hz, %5 channels, Float32")
          .arg(selected, bound.codec.isEmpty() ? "unknown" : bound.codec,
               bound.rate ? QString::number(bound.rate) : "unknown").arg(actual.rate).arg(actual.channels));
    } else if (current == PW_STREAM_STATE_PAUSED) {
      log("Paused: " + selected + "; AirPlay continues with silence");
    }
  }
};
PipeWireCapture::PipeWireCapture() : state_(std::make_unique<State>(*this)) {}
PipeWireCapture::~PipeWireCapture() { stop(); }
QList<ChannelInfo> PipeWireCapture::open(const QString &id, void *) {
  close();
  const auto devices = inputDevices();
  if (std::none_of(devices.begin(), devices.end(), [&](const auto &d) { return d.id == id; }))
    throw i18n::MessageError(i18n::text(i18n::Id::SelectAValidInputDevice));
  state_->selected = id;
  return {{0, i18n::Message("FL"), 0}, {1, i18n::Message("FR"), 0}};
}
CaptureStream PipeWireCapture::prepare(int left, int right, double maxBacklog) {
  stop();
  if (state_->selected.isEmpty() || left < 0 || left > 1 || right < 0 || right > 1 ||
      !std::isfinite(maxBacklog) || maxBacklog <= 0 || maxBacklog > 1)
    throw i18n::MessageError(i18n::text(i18n::Id::SelectAValidInputDevice));
  state_->left = left;
  state_->right = right;
  const auto blocks = size_t(std::ceil(maxBacklog * 44100 / 352)) + 2;
  state_->queue = std::make_shared<CaptureQueue>(352, 352 * sizeof(float), 352 * sizeof(float), blocks);
  CaptureStream result{state_->queue, {4, 32, false, true}, {4, 32, false, true}, 352};
  result.gapPolicy = GapPolicy::Silence;
  return result;
}
void PipeWireCapture::start() {
  if (!state_->queue) throw i18n::MessageError(i18n::text(i18n::Id::AudioInputIsNotReady));
  state_->running = true;
  state_->retry.clear();
  state_->timer.start(100);
  state_->tick();
}
i18n::Message PipeWireCapture::stop() noexcept {
  state_->volume.setSource({});
  state_->running = false;
  state_->timer.stop();
  state_->destroyStream();
  state_->queue.reset();
  return {};
}
void PipeWireCapture::setVolumeControlEnabled(bool enabled) {
  state_->volume.setSource(enabled && state_->running ? state_->selected : QString{});
}
i18n::Message PipeWireCapture::close() noexcept { auto error = stop(); state_->selected.clear(); return error; }
QList<DriverInfo> inputDevices() { return PipeWireCatalog::instance().devices(); }
std::unique_ptr<InputCapture> createInputCapture(CaptureKind) { return std::make_unique<PipeWireCapture>(); }
} // namespace audio
