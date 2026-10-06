#include "AirPlaySession.h"
#include "EventChannel.h"
#include "NowPlaying.h"
#include "app/Message.h"
#include <QNetworkDatagram>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <openssl/crypto.h>

namespace airplay {
namespace {
QVariantMap dictionary(const QVariant &value) {
  if (value.typeId() != QMetaType::QVariantMap)
    throw Error(i18n::text(i18n::Id::ReceiverDidNotReturnAPlistDictionary));
  return value.toMap();
}
bool integer(const QVariant &value) {
  return value.typeId() == QMetaType::LongLong ||
         value.typeId() == QMetaType::ULongLong ||
         value.typeId() == QMetaType::Int || value.typeId() == QMetaType::UInt;
}
quint16 port(const QVariant &value) {
  if (!integer(value) || value.toLongLong() < 1 || value.toLongLong() > 65535)
    throw Error(i18n::text(i18n::Id::ReceiverReturnedAnInvalidPort));
  return quint16(value.toUInt());
}
QString string(const QVariant &value) {
  return value.typeId() == QMetaType::QString ? value.toString() : QString{};
}
QMap<QString, QString> txt(const QByteArray &data) {
  QMap<QString, QString> result;
  for (qsizetype cursor = 0; cursor < data.size();) {
    const int size = uint8_t(data[cursor++]);
    if (cursor + size > data.size())
      throw Error(i18n::text(i18n::Id::TruncatedBonjourTXTRecord));
    auto item = data.mid(cursor, size);
    cursor += size;
    const auto equal = item.indexOf('=');
    result.insert(QString::fromUtf8(equal < 0 ? item : item.left(equal)),
                  equal < 0 ? QString{}
                            : QString::fromUtf8(item.mid(equal + 1)));
  }
  return result;
}
} // namespace
ReceiverInfo receiverInfo(const QByteArray &data) {
  const auto info = dictionary(plistDecode(data));
  if (info.contains("txtAirPlay") &&
      info["txtAirPlay"].typeId() != QMetaType::QByteArray)
    throw Error(i18n::text(i18n::Id::InvalidReceiverTxtAirPlayType));
  const auto fields = txt(info.value("txtAirPlay").toByteArray());
  const auto formats =
      info.value("supportedFormats").toMap().value("audioStream");
  if (integer(formats) && !(formats.toULongLong() & (1ULL << 18)))
    throw Error(i18n::text(i18n::Id::ReceiverDoesNotSupportKHzStereoALAC));
  const auto volume = info.value("initialVolume");
  if ((volume.typeId() != QMetaType::Double && !integer(volume)) ||
      !std::isfinite(volume.toDouble()) || volume.toDouble() < -144 ||
      volume.toDouble() > 0)
    throw Error(i18n::text(i18n::Id::InvalidReceiverInitialVolume));
  auto id = string(info.value("deviceID"));
  if (id.isEmpty())
    id = fields.value("deviceid");
  return {string(info.value("name")), id, fields.value("tsid"),
          fields.value("tsm"), volume.toDouble()};
}
void validateGroup(const ReceiverInfo &a, const ReceiverInfo &b) {
  if (a.stereoId.isEmpty() || a.stereoId != b.stereoId)
    throw Error(i18n::text(i18n::Id::AirPlayReceiversNotInSameStereoPair));
  if (a.deviceId.isEmpty() || b.deviceId.isEmpty() ||
      a.deviceId.compare(b.deviceId, Qt::CaseInsensitive) == 0)
    throw Error(i18n::text(i18n::Id::TwoDifferentReceiverDevicesAreRequired));
}
struct AirPlaySession::Peer {
  enum class Step {
    Info,
    PairChallenge,
    PairProof,
    SessionSetup,
    Event,
    Record,
    StreamSetup,
    Peers,
    InitialVolume,
    Metadata,
    Ready,
    KeepAlive,
    Volume,
    PlaybackInfo,
    PlaybackState,
    StopMetadata,
    Teardown
  };
  QHostAddress host;
  RtspClient rtsp;
  EventChannel event;
  QUdpSocket control;
  quint16 localDataPort = 0;
  Step step = Step::Info;
  ReceiverInfo info;
  SrpProof proof;
  QByteArray key, url;
  quint16 dataPort = 0, controlPort = 0;
  bool infoReady = false, active = false, stopped = false;
  QString sessionId;
  QList<QVariantMap> metadata;
  int metadataIndex = -1;
  double sentVolume = 0, confirmedVolume = 0;
  PlaybackState sentPlaybackState = PlaybackState::Playing;
  PlaybackState confirmedPlaybackState = PlaybackState::Playing;
  explicit Peer(const QHostAddress &address) : host(address) {}
  ~Peer() {
    OPENSSL_cleanse(key.data(), size_t(key.size()));
    OPENSSL_cleanse(proof.key.data(), size_t(proof.key.size()));
  }
};
AirPlaySession::AirPlaySession(app::Timing timing, audio::CaptureStream stream,
                               QList<ReceiverEndpoint> endpoints,
                               QObject *parent, SessionEnvironment environment,
                               NetworkRoute route)
    : QObject(parent), timing_(timing), environment_(std::move(environment)),
      route_(std::move(route)), endpoints_(std::move(endpoints)),
      stream_(std::move(stream)) {
  networkCheck_.setInterval(250);
  connect(&inbound_, &InboundControlServer::failed, this,
          [this](const QJsonArray &error) { stop(error); });
  connect(&inbound_, &InboundControlServer::error, this, &AirPlaySession::log);
  connect(this, &AirPlaySession::controlChanged, this,
          &AirPlaySession::publishControlState);
  connect(this, &AirPlaySession::streamingChanged, this,
          &AirPlaySession::publishControlState);
  connect(this, &AirPlaySession::volumeApplied, this,
          &AirPlaySession::publishControlState);
  connect(&remote_, &RemoteControl::failed, this,
          [this](const i18n::Message &text) { stop(text); });
  connect(&remote_, &RemoteControl::log, this, &AirPlaySession::log);
  connect(this, &AirPlaySession::volumeApplied, &remote_,
          &RemoteControl::setVolume);
  connect(&remote_, &RemoteControl::ready, this, [this] {
    if (state_ != State::Connecting)
      return;
    remoteReady_ = true;
    emit log(i18n::text(i18n::Id::DACPVolumeServicePublishedITunesCtrl) +
             identity_);
    prepared();
  });
  connect(&remote_, &RemoteControl::volumeCommand, this,
          [this](const QString &, const QString &action, double value) {
            remoteVolume(action, value);
          });
  connect(&remote_, &RemoteControl::playbackChanged, this,
          &AirPlaySession::dispatchControl);
  connect(&networkCheck_, &QTimer::timeout, this, [this] {
    try {
      route_.validate();
    } catch (const std::exception &e) {
      fail(e);
    }
  });
  settle_.setSingleShot(true);
  teardown_.setSingleShot(true);
  poll_.setInterval(1);
  poll_.setTimerType(Qt::PreciseTimer);
  keepAlive_.setTimerType(Qt::PreciseTimer);
  connect(&keepAlive_, &QTimer::timeout, this, &AirPlaySession::keepAlive);
  connect(&poll_, &QTimer::timeout, this, &AirPlaySession::poll);
  connect(&teardown_, &QTimer::timeout, this, &AirPlaySession::finishStop);
  connect(&settle_, &QTimer::timeout, this, [this] {
    if (state_ == State::Synchronizing) {
      setState(State::Buffering, i18n::text(i18n::Id::BufferingInput));
      emit startCapture();
    }
  });
  connect(&clock_, &PtpClock::failed, this,
          [this](const i18n::Message &error) { stop(error); });
}
AirPlaySession::~AirPlaySession() {
  state_ = State::Stopped;
  if (sender_) sender_->stop();
  inbound_.stop();
  remote_.stop();
  clock_.stop();
  for (auto &p : peers_) {
    if (p) {
      // Socket destructors can emit signals while the peer container is being
      // destroyed. Disconnect every peer first, while the whole list exists.
      disconnect(&p->rtsp, nullptr, this, nullptr);
      disconnect(&p->event, nullptr, this, nullptr);
      disconnect(&p->control, nullptr, this, nullptr);
      p->rtsp.abort();
      p->event.close();
    }
  }
}
void AirPlaySession::setState(State state, const i18n::Message &text) {
  state_ = state;
  remote_.setEnabled(state == State::Streaming);
  emit streamingChanged(state == State::Streaming);
  emit status(text);
  emit log(text);
}
void AirPlaySession::fail(const std::exception &error) {
  stop(i18n::fromException(error));
}
void AirPlaySession::start() {
  try {
    if (state_ != State::Preparing)
      throw Error(i18n::text(i18n::Id::SessionCannotBeStartedTwice));
    if (const auto error = timing_.validate(); !error.isEmpty())
      throw Error(error);
    if (!stream_.queue)
      throw Error(i18n::text(i18n::Id::CaptureQueueIsNotReady));
    validateEndpoints(endpoints_);
    route_.validate();
    if (!route_.binding.automatic()) {
      networkCheck_.start();
      emit log(i18n::text(i18n::Id::SendingNetwork) +
               route_.binding.interfaceName + " · " + route_.binding.ipv4);
    }
    clockId_ =
        (readBe(randomBytes(8), 0, 8) & 0x7fffffffffffffffULL) | (1ULL << 62);
    identity_ = QString::number(clockId_, 16).toUpper().rightJustified(16, '0');
    activeRemote_ = quint32(readBe(randomBytes(4), 0, 4)) | 1;
    firstRtp_ = uint32_t(readBe(randomBytes(4), 0, 4));
    firstSequence_ = uint16_t(readBe(randomBytes(2), 0, 2));
    sender_ = std::make_unique<RealtimeAudioSender>(timing_, stream_, firstSequence_,
                                                   firstRtp_, size_t(endpoints_.size()));
    if (!sender_->schedulingLog().isEmpty()) emit log(i18n::Message("Audio: " + sender_->schedulingLog()));
    if (!sender_->ready())
      throw Error(i18n::text(i18n::Id::AudioSchedulingRequired) + sender_->schedulingLog());
    sender_->telemetry(telemetryEnabled_, telemetryRevision_);
    groupId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    for (const auto &endpoint : endpoints_)
      peers_.push_back(std::make_unique<Peer>(endpoint.host));
    setState(State::Connecting,
             i18n::text(i18n::Id::ConnectingAndValidatingReceivers));
    for (int i = 0; i < int(peers_.size()); ++i) {
      auto &p = *peers_[i];
      connect(&p.rtsp, &RtspClient::opened, this, [this, i] {
        try {
          opened(i);
        } catch (const std::exception &e) {
          fail(e);
        }
      });
      connect(&p.rtsp, &RtspClient::response, this,
              [this, i](const QByteArray &body) {
                try {
                  reply(i, body);
                } catch (const std::exception &e) {
                  fail(e);
                }
              });
      connect(
          &p.rtsp, &RtspClient::failed, this,
          [this, i](const i18n::Message &error) {
            if (state_ == State::Stopping) {
              peers_[i]->stopped = true;
              if (allStopped())
                finishStop();
            } else
              stop(peers_[i]->host.toString() +
                   (peers_[i]->step == Peer::Step::Metadata ||
                            peers_[i]->step == Peer::Step::PlaybackInfo ||
                            peers_[i]->step == Peer::Step::PlaybackState
                        ? i18n::text(
                              i18n::Id::NowPlayingInformationRemoteControlIsNot)
                        : "：") +
                   error);
          });
      connect(&p.event, &EventChannel::connected, this, [this, i] {
        try {
          eventConnected(i);
        } catch (const std::exception &e) {
          fail(e);
        }
      });
      connect(&p.event, &EventChannel::log, this,
              [this, i](i18n::Message text) {
                emit log(peers_[i]->host.toString() + " · " + text);
              });
      connect(&p.event, &EventChannel::command, &remote_,
              &RemoteControl::handleEventCommand);
      connect(&p.event, &EventChannel::failed, this,
              [this, i](i18n::Message error) {
                if (state_ != State::Stopping && state_ != State::Stopped &&
                    state_ != State::Error)
                  stop(peers_[i]->host.toString() + "：" + error);
              });
      connect(&p.control, &QUdpSocket::readyRead, this, [this, i] {
        try {
          feedback(i);
        } catch (const std::exception &e) {
          fail(e);
        }
      });
    }
    // Automatic mode lets the first TCP connection select the source.
    // Explicit mode binds before connecting and shares the frozen route.
    peers_[0]->rtsp.open(peers_[0]->host, endpoints_[0].port, identity_,
                         timing_.connectTimeoutMs, {}, route_, activeRemote_);
  } catch (const std::exception &e) {
    fail(e);
  }
}
void AirPlaySession::opened(int index) {
  if (state_ != State::Connecting)
    return;
  auto &p = *peers_[index];
  if (index == 0) {
    local_ = p.rtsp.localAddress();
    for (size_t i = 1; i < peers_.size(); ++i)
      peers_[i]->rtsp.open(peers_[i]->host, endpoints_[qsizetype(i)].port,
                           identity_, timing_.connectTimeoutMs, local_, route_,
                           activeRemote_);
  }
  p.rtsp.request("GET", "/info", {}, {}, timing_.requestTimeoutMs);
}
void AirPlaySession::request(Peer &p, const QByteArray &method,
                             const QByteArray &body, const QByteArray &type) {
  p.rtsp.request(method, p.url, body, type, timing_.requestTimeoutMs);
}
void AirPlaySession::setupGroup() {
  volume_ = peers_[0]->info.volume;
  if (peers_.size() == 2) {
    validateGroup(peers_[0]->info, peers_[1]->info);
    volume_ = std::min(volume_, peers_[1]->info.volume);
    emit group(i18n::text(i18n::Id::GroupIdentity)
                   .arg(peers_[0]->info.name)
                   .arg(peers_[1]->info.name)
                   .arg(peers_[0]->info.stereoId)
                   .arg(peers_[0]->info.members)
                   .arg(peers_[1]->info.members));
  } else
    emit group(i18n::text(i18n::Id::SingleReceiverIdentity)
                   .arg(peers_[0]->info.name, endpoints_[0].text()));
  QList<QHostAddress> hosts;
  for (const auto &p : peers_)
    if (!hosts.contains(p->host))
      hosts.append(p->host);
  inbound_.start(local_, route_, [this](const ControlRequest &command) {
    if (state_ != State::Streaming)
      return false;
    const bool schedule = queuedControls_.empty();
    queuedControls_.push_back(command);
    // Drain one explicit FIFO after the transport callback unwinds. This also
    // prevents a synchronous receiver failure from deleting its caller.
    if (schedule) QTimer::singleShot(0, this, [this] {
      while (!queuedControls_.empty()) {
        const auto command = queuedControls_.front();
        queuedControls_.pop_front();
        if (state_ != State::Streaming) {
          queuedControls_.clear();
          break;
        }
        requestControl(command);
      }
      publishControlState();
    });
    return true;
  });
  inbound_.updateState(controlState());
  remote_.start(identity_, local_, route_, hosts, environment_.advertiseRemote,
                activeRemote_);
  if (environment_.startClock)
    environment_.startClock();
  else
    clock_.start(local_, hosts, clockId_, timing_, route_);
  if (state_ != State::Connecting)
    return;
  for (auto &pointer : peers_) {
    auto &p = *pointer;
    p.url = "rtsp://" + local_.toString().toLatin1() + "/" +
            QByteArray::number(readBe(randomBytes(4), 0, 4));
    p.step = Peer::Step::PairChallenge;
    p.rtsp.request("POST", "/pair-setup",
                   tlvEncode({{6, QByteArray::fromHex("01")},
                              {0, QByteArray::fromHex("00")},
                              {19, QByteArray::fromHex("10")}}),
                   "application/pairing+tlv8", timing_.requestTimeoutMs);
  }
}
void AirPlaySession::reply(int index, const QByteArray &body) {
  auto &p = *peers_[index];
  using Step = Peer::Step;
  if (state_ == State::Stopping) {
    if (p.step == Step::StopMetadata) {
      p.step = Step::Teardown;
      p.rtsp.request("TEARDOWN", p.url, {}, {}, timing_.teardownTimeoutMs);
      return;
    }
    p.stopped = true;
    if (allStopped())
      finishStop();
    return;
  }
  if (state_ == State::Stopped || state_ == State::Error)
    return;
  switch (p.step) {
  case Step::Info:
    p.info = receiverInfo(body);
    p.infoReady = true;
    if (std::ranges::all_of(peers_,
                            [](const auto &peer) { return peer->infoReady; }))
      setupGroup();
    break;
  case Step::PairChallenge: {
    const auto tlv = tlvDecode(body);
    if (tlv.contains(7) || tlv.value(6) != QByteArray::fromHex("02"))
      throw Error(i18n::text(i18n::Id::TransientPairingWasRejected));
    p.proof = srp(tlv.value(2), tlv.value(3));
    p.step = Step::PairProof;
    p.rtsp.request("POST", "/pair-setup",
                   tlvEncode({{6, QByteArray::fromHex("03")},
                              {3, p.proof.publicKey},
                              {4, p.proof.proof}}),
                   "application/pairing+tlv8", timing_.requestTimeoutMs);
    break;
  }
  case Step::PairProof: {
    const auto tlv = tlvDecode(body);
    const auto expected = p.proof.expected;
    const auto proof = tlv.value(4);
    if (tlv.contains(7) || tlv.value(6) != QByteArray::fromHex("04") ||
        proof.size() != expected.size() ||
        CRYPTO_memcmp(proof.constData(), expected.constData(),
                      size_t(expected.size())))
      throw Error(i18n::text(i18n::Id::SRPServerProofVerificationFailed));
    p.rtsp.encrypt(p.proof.key);
    p.key = p.proof.key.left(32);
    QVariantMap timing{
        {"ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper()},
        {"DeviceType", 0},
        {"ClockID", QVariant::fromValue<qulonglong>(clockId_)},
        {"Addresses", QVariantList{local_.toString()}},
        {"SupportsClockPortMatchingOverride", false}};
    QStringList id;
    for (int i = 0; i < 16; i += 2)
      id.append(identity_.mid(i, 2));
    p.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    QVariantMap session{{"deviceID", id.join(':')},
                        {"macAddress", id.join(':')},
                        {"name", "AirPlayQt"},
                        {"sessionUUID", p.sessionId},
                        {"timingProtocol", "PTP"},
                        {"groupUUID", groupId_},
                        {"groupContainsGroupLeader", false},
                        {"isMultiSelectAirPlay", true},
                        {"senderSupportsRelay", true},
                        {"timingPeerInfo", timing},
                        {"timingPeerList", QVariantList{timing}}};
    session.insert("uglServerInfo",
                   QVariantMap{{"Port", int(inbound_.port())},
                               {"Addresses", QVariantList{local_.toString()}}});
    if (peers_.size() == 2)
      session.insert("senderPerceivedClusterType", 1);
    p.step = Step::SessionSetup;
    request(p, "SETUP", plistEncode(session),
            "application/x-apple-binary-plist");
    break;
  }
  case Step::SessionSetup: {
    p.active = true;
    const auto eventPort =
        port(dictionary(plistDecode(body)).value("eventPort"));
    p.step = Step::Event;
    p.event.open(p.proof.key, p.host, eventPort, local_, route_,
                 timing_.connectTimeoutMs);
    OPENSSL_cleanse(p.proof.key.data(), size_t(p.proof.key.size()));
    p.proof = {};
    break;
  }
  case Step::Record: {
    QVariantMap stream{{"type", 96},
                       {"audioFormat", 1 << 18},
                       {"audioMode", "default"},
                       {"ct", 2},
                       {"sr", 44100},
                       {"spf", 352},
                       {"isMedia", true},
                       {"dataPort", int(p.localDataPort)},
                       {"controlPort", int(p.control.localPort())},
                       {"latencyMin", 11025},
                       {"latencyMax", 88200},
                       {"shk", p.key},
                       {"streamConnectionID", QVariant::fromValue<qulonglong>(
                                                  readBe(randomBytes(8), 0, 8) &
                                                  0x7fffffffffffffffULL)},
                       {"supportsDynamicStreamID", false}};
    p.step = Step::StreamSetup;
    request(p, "SETUP",
            plistEncode(QVariantMap{{"streams", QVariantList{stream}}}),
            "application/x-apple-binary-plist");
    break;
  }
  case Step::StreamSetup: {
    const auto streams = dictionary(plistDecode(body)).value("streams");
    if (streams.typeId() != QMetaType::QVariantList ||
        streams.toList().size() != 1)
      throw Error(i18n::text(i18n::Id::InvalidStreamSETUPResponse));
    const auto stream = dictionary(streams.toList()[0]);
    p.dataPort = port(stream.value("dataPort"));
    p.controlPort = port(stream.value("controlPort"));
    sender_->preparePeer(size_t(index), p.key, p.dataPort);
    p.step = Step::Peers;
    request(p, "SETPEERS",
            plistEncode(QVariantList{p.host.toString(), local_.toString()}),
            "application/x-apple-binary-plist");
    break;
  }
  case Step::Peers:
    p.step = Step::InitialVolume;
    p.sentVolume = volume_;
    request(p, "SET_PARAMETER",
            "volume: " + QByteArray::number(volume_, 'f', 6) + "\r\n",
            "text/parameters");
    break;
  case Step::InitialVolume:
    p.confirmedVolume = p.sentVolume;
    p.metadata = liveNowPlaying(identity_, p.sessionId, groupId_,
                                remote_.playbackState());
    p.step = Step::Metadata;
    emit log(
        p.host.toString() +
        i18n::text(i18n::Id::PublishingNowPlayingInformationDMAPLiveAudio));
    p.rtsp.request("SET_PARAMETER", p.url, liveDmapMetadata(),
                   "application/x-dmap-tagged", timing_.requestTimeoutMs,
                   firstRtp_);
    break;
  case Step::Metadata:
    ++p.metadataIndex;
    if (p.metadataIndex < p.metadata.size()) {
      const auto command = p.metadata[p.metadataIndex];
      emit log(p.host.toString() +
               i18n::text(i18n::Id::PublishingNowPlayingInformation) +
               command.value("type", "DEVICE_INFO").toString());
      p.rtsp.request("POST", "/command", plistEncode(command),
                     "application/x-apple-binary-plist",
                     timing_.requestTimeoutMs);
      break;
    }
    p.metadata.clear();
    p.step = Step::Ready;
    prepared();
    break;
  case Step::KeepAlive:
    p.step = Step::Ready;
    dispatchControl();
    break;
  case Step::Volume:
    p.confirmedVolume = p.sentVolume;
    p.step = Step::Ready;
    dispatchControl();
    break;
  case Step::PlaybackInfo:
    p.step = Step::PlaybackState;
    p.rtsp.request(
        "POST", "/command", plistEncode(playbackState(p.sentPlaybackState)),
        "application/x-apple-binary-plist", timing_.requestTimeoutMs);
    break;
  case Step::PlaybackState:
    p.confirmedPlaybackState = p.sentPlaybackState;
    p.step = Step::Ready;
    dispatchControl();
    break;
  default:
    throw Error(i18n::text(i18n::Id::UnexpectedSessionResponseState));
  }
}
void AirPlaySession::eventConnected(int index) {
  auto &p = *peers_[index];
  if (state_ != State::Connecting || p.step != Peer::Step::Event)
    return;
  if (!route_.binding.automatic()) route_.bind(p.control);
  else if (!p.control.bind(local_, 0, QUdpSocket::DontShareAddress))
    throw Error(i18n::text(i18n::Id::AudioUDPBindingFailed));
  p.localDataPort = sender_->bindPeer(size_t(index), local_, p.host, route_,
                                      p.control.socketDescriptor());
  p.step = Peer::Step::Record;
  request(p, "RECORD");
}
void AirPlaySession::captureStarted() {
  if (state_ != State::Buffering) return;
  elapsed_.start();
  lastStats_ = 0;
  if (stream_.gapPolicy == audio::GapPolicy::Silence)
    emit log(i18n::text(i18n::Id::LinuxInputWaiting));
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  if (stream_.rateDiagnostics)
    emit log(i18n::text(i18n::Id::VSTRateStartingFramesSBitInput)
                 .arg(stream_.left.bytes * 8).arg(stream_.blockFrames)
                 .arg(double(timing_.prebufferSamples) / 44.1, 0, 'f', 1)
                 .arg(double(timing_.backlogSamples) / 44.1, 0, 'f', 1));
#endif
  sender_->begin();
  poll_.start();
}
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
void AirPlaySession::logRates(bool final) {
  if (!stream_.rateDiagnostics || !elapsed_.isValid()) return;
  const auto now = elapsed_.nsecsElapsed();
  const auto interval = now - rates_.reportedAt;
  if (interval <= 0 || (!final && interval < 1000000000)) return;
  const auto captured = stream_.queue->capturedFrames();
  const double seconds = double(interval) / 1e9;
  const double input = double(captured - rates_.captured) / seconds;
  const double output = double(lastReport_.sentFrames - rates_.sent) / seconds;
  emit log(i18n::text(i18n::Id::VSTRateWindowMsInputFramesS)
      .arg(final ? i18n::text(i18n::Id::StoppedRateSuffix) : "")
      .arg(seconds * 1000, 0, 'f', 1).arg(input, 0, 'f', 1).arg(output, 0, 'f', 1)
      .arg((input - output) / 44.1, 0, 'f', 2)
      .arg(double(lastReport_.buffered) / 44.1, 0, 'f', 2)
      .arg(double(stream_.queue->queuedFrames()) / 44.1, 0, 'f', 2)
      .arg(double(lastReport_.pcmFrames) / 44.1, 0, 'f', 2)
      .arg(double(lastReport_.maxGapNs) / 1e6, 0, 'f', 3)
      .arg(double(lastReport_.maxWorkNs) / 1e6, 0, 'f', 3)
      .arg(captured).arg(lastReport_.sentFrames).arg(stream_.queue->fault.load()));
  rates_ = {now, captured, lastReport_.sentFrames};
}
#endif
void AirPlaySession::setTelemetryEnabled(bool enabled, quint64 revision) {
  telemetryEnabled_ = enabled;
  telemetryRevision_ = revision;
  leftPeak_ = rightPeak_ = 0;
  lastStats_ = elapsed_.isValid() ? elapsed_.nsecsElapsed() : 0;
  if (sender_) sender_->telemetry(enabled, revision);
}
void AirPlaySession::poll() {
  if (state_ != State::Buffering && state_ != State::Streaming) return;
  try {
    const auto report = sender_->poll();
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
    lastReport_ = report;
    logRates();
#endif
    const auto error = sender_->error();
    if (error.code == SendFailure::Interrupted) { stop({}, SessionEnd::HostInterrupted); return; }
    if (error.code != SendFailure::None) {
      using Id = i18n::Id;
      Id id = Id::RealtimeSenderFailed;
      switch (error.code) {
      case SendFailure::InputFault: id = Id::AudioInputFaultBufferIndexOverflowCallback; break;
      case SendFailure::InputTimeout: id = Id::AudioInputTimedOut; break;
      case SendFailure::Backlog: id = Id::CaptureBacklogExceedsTheLimit; break;
      case SendFailure::InvalidPcm: id = Id::AudioInputContainsNaNInfStreamingStopped; break;
      case SendFailure::ClockJump: id = Id::SystemClockOffsetExceedsTheLimit; break;
      case SendFailure::Late: id = Id::SendingFellTooFarBehindTheTimeline; break;
      case SendFailure::NonceExhausted: id = Id::AudioNonceExhausted; break;
      case SendFailure::Encode: id = Id::OpenSSLOperationFailed; break;
      case SendFailure::Send: id = Id::AudioUDPSendFailed; break;
      case SendFailure::Retransmit: id = Id::RetransmissionFailed; break;
      case SendFailure::QueueFull: id = Id::RealtimeRequestQueueFull; break;
      default: break;
      }
      if (error.code == SendFailure::InputFault || error.code == SendFailure::Wait)
        throw Error(i18n::text(id).arg(error.detail));
      throw Error(error.detail ? i18n::text(id) + " (native code=" +
                                    QString::number(error.detail) + ")"
                              : i18n::text(id));
    }
    if (report.inputRevision != inputRevision_) {
      inputRevision_ = report.inputRevision;
      if (stream_.gapPolicy == audio::GapPolicy::Silence) {
        const auto id = report.input == InputState::Playing ? i18n::Id::LinuxInputPlaying
                      : report.input == InputState::Buffering ? i18n::Id::LinuxInputBuffering
                      : i18n::Id::LinuxInputReset;
        emit log(i18n::text(id));
      }
    }
    if (report.streaming && state_ == State::Buffering) {
      audible_ = report.audible;
      nextSync_ = 0;
      setState(State::Streaming, i18n::text(i18n::Id::StreamingKHzBitStereo));
    }
    const auto now = elapsed_.nsecsElapsed();
    if (state_ == State::Streaming && now >= nextSync_) {
      const auto packet = syncPacket(clockId_, firstRtp_, audible_, wallNs(), firstSync_);
      for (auto &p : peers_)
        if (p->control.writeDatagram(packet, p->host, p->controlPort) != packet.size())
          throw Error(i18n::text(i18n::Id::AudioSyncPacketSendFailed));
      firstSync_ = false;
      nextSync_ = now + qint64(timing_.audioSyncMs * 1e6);
    }
    if (report.telemetryRevision == telemetryRevision_ && telemetryEnabled_) {
      leftPeak_ = std::max(leftPeak_, report.leftPeak);
      rightPeak_ = std::max(rightPeak_, report.rightPeak);
      if (now - lastStats_ >= 100000000) {
        emit telemetry(leftPeak_, rightPeak_, double(report.buffered) / 44100,
                       report.packets, report.retransmitted, report.expired, telemetryRevision_);
        leftPeak_ = rightPeak_ = 0;
        lastStats_ = now;
      }
    }
  } catch (const std::exception &e) { fail(e); }
}
void AirPlaySession::feedback(int index) {
  auto &p = *peers_[index];
  for (int batch = 0; batch < 64 && p.control.hasPendingDatagrams(); ++batch) {
    const auto datagram = p.control.receiveDatagram(1024);
    const auto request = datagram.data();
    if (state_ != State::Streaming || datagram.senderAddress() != p.host ||
        request.size() < 8 || (uint8_t(request[1]) & 0x7f) != 0x55) continue;
    const auto count = readBe(request, 6, 2);
    if (count < 1 || count > 1024) continue;
    if (!sender_->retransmit({uint16_t(index), uint16_t(datagram.senderPort()),
                              uint16_t(readBe(request, 2, 2)),
                              uint16_t(readBe(request, 4, 2)), uint16_t(count)}))
      throw Error(i18n::text(i18n::Id::RealtimeRequestQueueFull));
  }
  if (p.control.hasPendingDatagrams())
    QTimer::singleShot(0, this, [this, index] {
      try { feedback(index); } catch (const std::exception &e) { fail(e); }
    });
}
void AirPlaySession::volume(double db) {
  try {
    if (state_ != State::Streaming)
      throw Error(i18n::text(i18n::Id::VolumeCannotBeAdjustedInTheCurrent));
    if (!std::isfinite(db) || db < -144 || db > 0)
      throw Error(i18n::text(i18n::Id::VolumeIsOutOfRange));
    volume_ = db;
    volumePending_ = true;
    dispatchControl();
  } catch (const std::exception &e) {
    fail(e);
  }
}
void AirPlaySession::dispatchControl() {
  if (state_ != State::Streaming || !allReady())
    return;
  try {
    if (volumePending_) {
      bool changed = false;
      for (auto &p : peers_) {
        if (std::abs(p->confirmedVolume - volume_) < .005)
          continue;
        changed = true;
        p->step = Peer::Step::Volume;
        p->sentVolume = volume_;
        request(*p, "SET_PARAMETER",
                "volume: " + QByteArray::number(volume_, 'f', 6) + "\r\n",
                "text/parameters");
      }
      if (changed)
        return;
      volumePending_ = false;
      if (volume_ > -144)
        restoreVolume_ = volume_;
      emit volumeApplied(volume_);
      if (state_ != State::Streaming || !allReady())
        return;
    }
    // Each peer completes info -> state with one immutable snapshot. Requests
    // arriving meanwhile change only the target, never this in-flight pair.
    const auto desired = remote_.playbackState();
    for (auto &p : peers_) {
      if (p->confirmedPlaybackState == desired)
        continue;
      p->sentPlaybackState = desired;
      p->step = Peer::Step::PlaybackInfo;
      p->rtsp.request("POST", "/command", plistEncode(nowPlayingInfo(desired)),
                      "application/x-apple-binary-plist",
                      timing_.requestTimeoutMs);
    }
    if (allReady())
      emit controlChanged();
  } catch (const std::exception &e) {
    fail(e);
  }
}
ControlState AirPlaySession::controlState() const {
  ControlState result;
  result.group = groupId_.toUtf8();
  result.volumeDb = volume_;
  result.available = state_ == State::Streaming;
  result.playback =
      result.available ? remote_.playbackState() : PlaybackState::Stopped;
  result.settled =
      result.available && queuedControls_.empty() && !volumePending_ &&
      allReady() && std::ranges::all_of(peers_, [this](const auto &p) {
        return p->confirmedPlaybackState == remote_.playbackState();
      });
  for (const auto &p : peers_)
    if (p && !p->info.deviceId.isEmpty())
      result.outputs.append(
          {p->info.deviceId.toUtf8(), p->info.name.toUtf8(), {}, p->host});
  return result;
}
void AirPlaySession::publishControlState() {
  if (controlUpdateQueued_)
    return;
  controlUpdateQueued_ = true;
  QTimer::singleShot(0, this, [this] {
    controlUpdateQueued_ = false;
    if (state_ != State::Stopping && state_ != State::Stopped &&
        state_ != State::Error)
      inbound_.updateState(controlState());
  });
}
bool AirPlaySession::requestControl(const ControlRequest &request) {
  if (state_ != State::Streaming)
    return false;
  switch (request.action) {
  case ControlAction::Volume:
    if (!std::isfinite(request.volumeDb) || request.volumeDb < -144 ||
        request.volumeDb > 0)
      return false;
    inputVolume(request.volumeDb);
    break;
  case ControlAction::Play:
    remote_.requestPlayback(PlaybackState::Playing);
    break;
  case ControlAction::Pause:
    remote_.requestPlayback(PlaybackState::Paused);
    break;
  case ControlAction::Toggle:
    remote_.requestPlayback(remote_.playbackState() == PlaybackState::Playing
                                ? PlaybackState::Paused
                                : PlaybackState::Playing);
    break;
  default:
    return false;
  }
  // Absolute repeats still complete, even without a state transition.
  dispatchControl();
  return state_ == State::Streaming;
}
void AirPlaySession::prepared() {
  if (state_ != State::Connecting || !remoteReady_ || !allReady())
    return;
  if (volume_ > -144)
    restoreVolume_ = volume_;
  emit volumeApplied(volume_);
  keepAlive_.start(int(std::ceil(timing_.keepAliveMs)));
  setState(State::Synchronizing,
           i18n::text(i18n::Id::WaitingForPTPSynchronization));
  settle_.start(int(std::ceil(timing_.settleMs)));
}
void AirPlaySession::inputVolume(double db) {
  if (state_ == State::Streaming && std::isfinite(db) && db >= -144 && db <= 0)
    volume(db);
}
void AirPlaySession::inputVolumeStep(int direction) {
  if (direction == 1 || direction == -1)
    remoteVolume(direction == 1 ? "volumeup" : "volumedown", 0);
}
void AirPlaySession::remoteVolume(const QString &action, double value) {
  if (state_ != State::Streaming)
    return;
  if (action == "absolute") {
    if (std::abs(value - volume_) < .005)
      return;
    // A receiver may echo an older in-flight SET_PARAMETER. Do not let it
    // roll back a newer desired value while that transaction is completing.
    if (volumePending_ && std::ranges::any_of(peers_, [value](const auto &p) {
          return std::abs(p->sentVolume - value) < .005;
        }))
      return;
  } else if (action == "mutetoggle") {
    value = volume_ <= -144 ? restoreVolume_ : -144;
  } else {
    if (volume_ <= -144 && action == "volumedown")
      return;
    value = std::clamp((volume_ <= -144 ? restoreVolume_ : volume_) +
                           (action == "volumeup" ? 1. : -1.),
                       -144., 0.);
  }
  volume(value);
}
void AirPlaySession::keepAlive() {
  if (state_ == State::Stopping || state_ == State::Stopped ||
      state_ == State::Error)
    return;
  try {
    if (volumePending_)
      return;
    for (auto &p : peers_)
      if (p->step == Peer::Step::Ready) {
        p->step = Peer::Step::KeepAlive;
        p->rtsp.request("OPTIONS", "*", {}, {}, timing_.requestTimeoutMs);
      }
  } catch (const std::exception &e) {
    fail(e);
  }
}
void AirPlaySession::stop(const i18n::Message &error, SessionEnd reason) {
  networkCheck_.stop();
  remote_.stop();
  if (state_ == State::Stopping) {
    // A real fault during host-interruption cleanup must not remain classified
    // as a resumable interruption. Keep the first real failure if one exists.
    if (!error.isEmpty() && error_.isEmpty()) {
      error_ = error;
      endReason_ = SessionEnd::Failure;
      emit status(i18n::text(i18n::Id::StoppingReasonPrefix) + error);
      emit log(i18n::text(i18n::Id::StoppingReasonPrefix) + error);
    }
    return;
  }
  if (state_ == State::Stopped || state_ == State::Error)
    return;
  error_ = error;
  endReason_ = error.isEmpty() ? reason : SessionEnd::Failure;
  setState(
      State::Stopping,
      endReason_ == SessionEnd::HostInterrupted
          ? i18n::text(i18n::Id::HostAudioProcessingInterruptedCleaningUpThe)
          : (error.isEmpty()
                 ? i18n::text(i18n::Id::Stopping)
                 : i18n::text(i18n::Id::StoppingReasonPrefix) + error));
#ifdef AIRPLAY_VST_RATE_DIAGNOSTICS
  logRates(true);
#endif
  inbound_.beginStop();
  emit stopCapture();
  poll_.stop();
  if (sender_) sender_->stop();
  settle_.stop();
  keepAlive_.stop();
  clock_.stop();
  if (environment_.stopClock)
    environment_.stopClock();
  teardown_.start(int(std::ceil(timing_.teardownTimeoutMs)));
  for (auto &p : peers_)
    if (p) {
      p->event.close();
      p->control.close();
      if (p->active && p->rtsp.connected() && !p->rtsp.busy()) {
        try {
          p->step = Peer::Step::StopMetadata;
          p->rtsp.request("POST", "/command",
                          plistEncode(playbackState(PlaybackState::Stopped)),
                          "application/x-apple-binary-plist",
                          timing_.teardownTimeoutMs);
        } catch (const std::exception &e) {
          emit log("TEARDOWN：" + i18n::fromException(e));
          p->stopped = true;
        }
      } else {
        p->rtsp.abort();
        p->stopped = true;
      }
    }
  if (allStopped())
    finishStop();
}
bool AirPlaySession::allReady() const {
  return !peers_.empty() && std::ranges::all_of(peers_, [](const auto &p) {
    return p->step == Peer::Step::Ready;
  });
}
bool AirPlaySession::allStopped() const {
  return std::ranges::all_of(peers_, [](const auto &p) { return p->stopped; });
}
void AirPlaySession::finishStop() {
  if (state_ != State::Stopping)
    return;
  teardown_.stop();
  inbound_.stop();
  for (auto &p : peers_)
    if (p) {
      p->rtsp.abort();
      p->event.close();
      p->control.close();
    }
  sender_.reset();
  setState(error_.isEmpty() ? State::Stopped : State::Error,
           error_.isEmpty() ? i18n::text(i18n::Id::Stopped)
                            : i18n::text(i18n::Id::ErrorPrefix) + error_);
  emit finished(error_, int(endReason_));
}
} // namespace airplay
