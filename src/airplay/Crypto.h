#pragma once
#include <QByteArray>
#include <QMap>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace airplay {
class Error : public std::runtime_error {
public:
  explicit Error(const QString &message)
      : std::runtime_error(message.toStdString()) {}
};
QByteArray randomBytes(int size);
QByteArray sha512(const QByteArray &data);
QByteArray hkdf(const QByteArray &shared, const QByteArray &salt,
                const QByteArray &info);
QByteArray seal(const QByteArray &key, const QByteArray &nonce,
                const QByteArray &plain, const QByteArray &aad);
QByteArray unseal(const QByteArray &key, const QByteArray &nonce,
                  const QByteArray &cipher, const QByteArray &aad);
QByteArray nonce(uint64_t counter);
QByteArray tlvEncode(const QList<std::pair<int, QByteArray>> &fields);
QMap<int, QByteArray> tlvDecode(const QByteArray &data);
struct SrpProof {
  QByteArray publicKey, proof, expected, key;
};
SrpProof srp(const QByteArray &salt, const QByteArray &serverPublic,
             QByteArray privateKey = {});
class HapRecords {
public:
  HapRecords(QByteArray write, QByteArray read);
  ~HapRecords();
  QByteArray encode(const QByteArray &plain);
  QByteArray decode(const QByteArray &size, const QByteArray &cipher);
  uint64_t tx = 0, rx = 0;

private:
  QByteArray write_, read_;
  bool txExhausted_ = false, rxExhausted_ = false;
};
QByteArray alac(std::span<const int16_t> stereo);
QByteArray audioPacket(const QByteArray &key, std::span<const int16_t> stereo,
                       uint16_t sequence, uint32_t timestamp, uint64_t counter,
                       bool first);
void appendBe(QByteArray &data, uint64_t number, int bytes);
uint64_t readBe(const QByteArray &data, qsizetype offset, int bytes);
} // namespace airplay
