#pragma once
#include <QByteArray>
#include <memory>

namespace airplay {
// One transient SRP exchange, no persistent identity or pairing database.
class TransientPairing {
public:
  TransientPairing();
  ~TransientPairing();
  QByteArray challenge() const;
  struct Result {
    QByteArray proof, key;
  };
  Result verify(const QByteArray &publicKey, const QByteArray &proof);

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace airplay
