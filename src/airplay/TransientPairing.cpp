#include "TransientPairing.h"
#include "airplay/Crypto.h"
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <stdexcept>

namespace airplay {
namespace {
using Big = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
Big own(BIGNUM *value) {
  if (!value)
    throw std::runtime_error("SRP allocation failed");
  return Big(value, BN_clear_free);
}
Big number(const QByteArray &data) {
  return own(
      BN_bin2bn(reinterpret_cast<const unsigned char *>(data.constData()),
                int(data.size()), nullptr));
}
void check(int result) {
  if (result != 1)
    throw std::runtime_error("SRP operation failed");
}
QByteArray encoded(const BIGNUM *value, int width = 0) {
  QByteArray out(width ? width : BN_num_bytes(value), Qt::Uninitialized);
  if (BN_bn2binpad(value, reinterpret_cast<unsigned char *>(out.data()),
                   int(out.size())) != out.size())
    throw std::runtime_error("SRP encoding failed");
  return out;
}
} // namespace
struct TransientPairing::State {
  std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx{BN_CTX_new(),
                                                      BN_CTX_free};
  Big n = own(BN_get_rfc3526_prime_3072(nullptr));
  Big g = number(QByteArray(1, char(5)));
  Big secret = own(BN_new()), verifier = own(BN_new()), pub = own(BN_new());
  QByteArray salt = airplay::randomBytes(16);
  bool consumed = false;
  State() {
    if (!ctx)
      throw std::runtime_error("SRP context allocation failed");
    check(BN_priv_rand(secret.get(), 256, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY));
    BN_set_flags(secret.get(), BN_FLG_CONSTTIME);
    auto x = number(airplay::sha512(salt + airplay::sha512("Pair-Setup:3939")));
    BN_set_flags(x.get(), BN_FLG_CONSTTIME);
    auto k =
        number(airplay::sha512(encoded(n.get(), 384) + encoded(g.get(), 384)));
    auto gb = own(BN_new());
    check(BN_mod_exp(verifier.get(), g.get(), x.get(), n.get(), ctx.get()));
    check(BN_mod_exp(gb.get(), g.get(), secret.get(), n.get(), ctx.get()));
    check(BN_mod_mul(pub.get(), k.get(), verifier.get(), n.get(), ctx.get()));
    check(BN_mod_add(pub.get(), pub.get(), gb.get(), n.get(), ctx.get()));
  }
};
TransientPairing::TransientPairing() : state_(std::make_unique<State>()) {}
TransientPairing::~TransientPairing() = default;
QByteArray TransientPairing::challenge() const {
  return airplay::tlvEncode({{6, QByteArray(1, char(2))},
                             {2, state_->salt},
                             {3, encoded(state_->pub.get())}});
}
TransientPairing::Result TransientPairing::verify(const QByteArray &client,
                                                  const QByteArray &proof) {
  auto &s = *state_;
  if (s.consumed)
    throw std::runtime_error("SRP exchange already consumed");
  s.consumed = true;
  if (client.isEmpty() || client.size() > 384 || proof.size() != 64)
    throw std::runtime_error("Invalid SRP proof size");
  auto a = number(client), remainder = own(BN_new());
  check(BN_nnmod(remainder.get(), a.get(), s.n.get(), s.ctx.get()));
  if (BN_is_zero(remainder.get()))
    throw std::runtime_error("Invalid SRP public key");
  auto u = number(
      airplay::sha512(encoded(a.get(), 384) + encoded(s.pub.get(), 384)));
  if (BN_is_zero(u.get()))
    throw std::runtime_error("Invalid SRP scrambling parameter");
  auto base = own(BN_new()), shared = own(BN_new());
  check(BN_mod_exp(base.get(), s.verifier.get(), u.get(), s.n.get(),
                   s.ctx.get()));
  check(BN_mod_mul(base.get(), base.get(), a.get(), s.n.get(), s.ctx.get()));
  check(BN_mod_exp(shared.get(), base.get(), s.secret.get(), s.n.get(),
                   s.ctx.get()));
  auto secretBytes = encoded(shared.get());
  auto key = airplay::sha512(secretBytes);
  OPENSSL_cleanse(secretBytes.data(), size_t(secretBytes.size()));
  auto mixed = airplay::sha512(encoded(s.n.get()));
  const auto gh = airplay::sha512(QByteArray(1, char(5)));
  for (int i = 0; i < mixed.size(); ++i)
    mixed[i] = char(uint8_t(mixed[i]) ^ uint8_t(gh[i]));
  const auto expected =
      airplay::sha512(mixed + airplay::sha512("Pair-Setup") + s.salt + client +
                      encoded(s.pub.get()) + key);
  if (CRYPTO_memcmp(proof.constData(), expected.constData(), 64)) {
    OPENSSL_cleanse(key.data(), size_t(key.size()));
    throw std::runtime_error("Client SRP proof rejected");
  }
  return {airplay::sha512(client + proof + key), std::move(key)};
}
} // namespace airplay
