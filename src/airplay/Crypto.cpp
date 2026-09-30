#include "Crypto.h"
#include <algorithm>
#include <limits>
#include <memory>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace airplay {
static void check(int result) {
  if (result != 1)
    throw Error("OpenSSL 操作失败");
}
template <class T, auto Free> using Owner = std::unique_ptr<T, decltype(Free)>;
static const unsigned char *bytes(const QByteArray &data) {
  return reinterpret_cast<const unsigned char *>(data.constData());
}
static unsigned char *bytes(QByteArray &data) {
  return reinterpret_cast<unsigned char *>(data.data());
}
void appendBe(QByteArray &data, uint64_t number, int size) {
  for (int i = size - 1; i >= 0; --i)
    data.append(char(number >> (8 * i)));
}
uint64_t readBe(const QByteArray &data, qsizetype offset, int size) {
  if (size < 1 || size > 8 || offset < 0 || offset > data.size() - size)
    throw Error("截断的数据字段");
  uint64_t value = 0;
  for (int i = 0; i < size; ++i)
    value = (value << 8) | uint8_t(data[offset + i]);
  return value;
}
QByteArray randomBytes(int size) {
  QByteArray result(size, Qt::Uninitialized);
  check(RAND_bytes(bytes(result), size));
  return result;
}
QByteArray sha512(const QByteArray &data) {
  QByteArray result(64, Qt::Uninitialized);
  unsigned size = 0;
  check(EVP_Digest(data.constData(), size_t(data.size()), bytes(result), &size,
                   EVP_sha512(), nullptr));
  if (size != 64)
    throw Error("SHA512 长度无效");
  return result;
}
QByteArray hkdf(const QByteArray &shared, const QByteArray &salt,
                const QByteArray &info) {
  Owner<EVP_PKEY_CTX, EVP_PKEY_CTX_free> ctx(
      EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!ctx)
    throw Error("HKDF 分配失败");
  check(EVP_PKEY_derive_init(ctx.get()));
  check(EVP_PKEY_CTX_set_hkdf_md(ctx.get(), EVP_sha512()));
  check(EVP_PKEY_CTX_set1_hkdf_salt(ctx.get(), bytes(salt), int(salt.size())));
  check(
      EVP_PKEY_CTX_set1_hkdf_key(ctx.get(), bytes(shared), int(shared.size())));
  check(EVP_PKEY_CTX_add1_hkdf_info(ctx.get(), bytes(info), int(info.size())));
  QByteArray result(32, Qt::Uninitialized);
  size_t size = 32;
  check(EVP_PKEY_derive(ctx.get(), bytes(result), &size));
  if (size != 32)
    throw Error("HKDF 长度无效");
  return result;
}
static QByteArray crypt(bool encrypt, const QByteArray &key,
                        const QByteArray &iv, const QByteArray &input,
                        const QByteArray &aad) {
  if (key.size() != 32 || iv.size() != 12 || input.size() > INT_MAX - 32 ||
      aad.size() > INT_MAX || (!encrypt && input.size() < 16))
    throw Error("ChaCha20Poly1305 参数无效");
  Owner<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> ctx(EVP_CIPHER_CTX_new(),
                                                 EVP_CIPHER_CTX_free);
  if (!ctx)
    throw Error("加密上下文分配失败");
  check(EVP_CipherInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr,
                          bytes(key), bytes(iv), encrypt));
  int size = 0;
  check(
      EVP_CipherUpdate(ctx.get(), nullptr, &size, bytes(aad), int(aad.size())));
  const int payload = int(input.size()) - (encrypt ? 0 : 16);
  QByteArray output(payload + 32, Qt::Uninitialized);
  check(
      EVP_CipherUpdate(ctx.get(), bytes(output), &size, bytes(input), payload));
  int total = size;
  if (!encrypt)
    check(EVP_CIPHER_CTX_ctrl(
        ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16,
        const_cast<unsigned char *>(bytes(input) + payload)));
  if (EVP_CipherFinal_ex(ctx.get(), bytes(output) + total, &size) != 1)
    throw Error("加密记录认证失败");
  total += size;
  if (encrypt) {
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16,
                              bytes(output) + total));
    total += 16;
  }
  output.resize(total);
  return output;
}
QByteArray seal(const QByteArray &k, const QByteArray &n, const QByteArray &p,
                const QByteArray &a) {
  return crypt(true, k, n, p, a);
}
QByteArray unseal(const QByteArray &k, const QByteArray &n, const QByteArray &c,
                  const QByteArray &a) {
  return crypt(false, k, n, c, a);
}
QByteArray nonce(uint64_t counter) {
  QByteArray n(4, '\0');
  for (int i = 0; i < 8; ++i)
    n.append(char(counter >> (8 * i)));
  return n;
}
QByteArray tlvEncode(const QList<std::pair<int, QByteArray>> &fields) {
  QByteArray result;
  for (const auto &[tag, data] : fields) {
    if (tag < 0 || tag > 255)
      throw Error("TLV 标签无效");
    for (qsizetype pos = 0; pos < std::max(qsizetype(1), data.size());
         pos += 255) {
      const auto chunk = data.mid(pos, 255);
      result.append(char(tag));
      result.append(char(chunk.size()));
      result += chunk;
    }
  }
  return result;
}
QMap<int, QByteArray> tlvDecode(const QByteArray &data) {
  QMap<int, QByteArray> result;
  for (qsizetype pos = 0; pos < data.size();) {
    if (pos + 2 > data.size())
      throw Error("TLV 头部截断");
    const int tag = uint8_t(data[pos++]), length = uint8_t(data[pos++]);
    if (pos + length > data.size())
      throw Error("TLV 内容截断");
    result[tag] += data.mid(pos, length);
    pos += length;
  }
  return result;
}
SrpProof srp(const QByteArray &salt, const QByteArray &serverPublic,
             QByteArray privateKey) {
  if (salt.size() != 16 || serverPublic.isEmpty() || serverPublic.size() > 384)
    throw Error("SRP challenge 长度无效");
  using Big = Owner<BIGNUM, BN_clear_free>;
  auto make = []() {
    Big b(BN_new(), BN_clear_free);
    if (!b)
      throw Error("BN 分配失败");
    return b;
  };
  auto integer = [](const QByteArray &data) {
    Big b(BN_bin2bn(bytes(data), int(data.size()), nullptr), BN_clear_free);
    if (!b)
      throw Error("BN 转换失败");
    return b;
  };
  auto encoded = [](const BIGNUM *b, int width = 0) {
    QByteArray out(width ? width : std::max(1, BN_num_bytes(b)),
                   Qt::Uninitialized);
    if (BN_bn2binpad(b, bytes(out), int(out.size())) != out.size())
      throw Error("BN 编码失败");
    return out;
  };
  Owner<BN_CTX, BN_CTX_free> ctx(BN_CTX_new(), BN_CTX_free);
  if (!ctx)
    throw Error("BN 上下文分配失败");
  Big n(BN_get_rfc3526_prime_3072(nullptr), BN_clear_free);
  if (!n)
    throw Error("SRP N 分配失败");
  auto g = make();
  check(BN_set_word(g.get(), 5));
  auto b = integer(serverPublic), remainder = make();
  check(BN_nnmod(remainder.get(), b.get(), n.get(), ctx.get()));
  if (BN_is_zero(remainder.get()))
    throw Error("SRP 服务端公钥无效");
  if (privateKey.isEmpty()) {
    privateKey = randomBytes(32);
    privateKey[0] = char(uint8_t(privateKey[0]) | 0x80);
  }
  auto a = integer(privateKey);
  OPENSSL_cleanse(privateKey.data(), size_t(privateKey.size()));
  BN_set_flags(a.get(), BN_FLG_CONSTTIME);
  if (BN_is_zero(a.get()))
    throw Error("SRP 私钥无效");
  auto A = make();
  check(BN_mod_exp(A.get(), g.get(), a.get(), n.get(), ctx.get()));
  auto k = integer(sha512(encoded(n.get(), 384) + encoded(g.get(), 384)));
  auto u = integer(sha512(encoded(A.get(), 384) + encoded(b.get(), 384)));
  if (BN_is_zero(u.get()))
    throw Error("SRP scrambling 参数为零");
  auto x = integer(sha512(salt + sha512("Pair-Setup:3939")));
  BN_set_flags(x.get(), BN_FLG_CONSTTIME);
  auto gx = make(), base = make(), exponent = make(), shared = make();
  check(BN_mod_exp(gx.get(), g.get(), x.get(), n.get(), ctx.get()));
  check(BN_mod_mul(base.get(), k.get(), gx.get(), n.get(), ctx.get()));
  check(BN_mod_sub(base.get(), b.get(), base.get(), n.get(), ctx.get()));
  check(BN_mul(exponent.get(), u.get(), x.get(), ctx.get()));
  check(BN_add(exponent.get(), exponent.get(), a.get()));
  BN_set_flags(exponent.get(), BN_FLG_CONSTTIME);
  check(
      BN_mod_exp(shared.get(), base.get(), exponent.get(), n.get(), ctx.get()));
  auto key = sha512(encoded(shared.get()));
  auto mixed = sha512(encoded(n.get())), gh = sha512(QByteArray(1, char(5)));
  for (int i = 0; i < 64; ++i)
    mixed[i] = char(uint8_t(mixed[i]) ^ uint8_t(gh[i]));
  auto pub = encoded(A.get());
  auto proof = sha512(mixed + sha512("Pair-Setup") + salt + pub +
                      encoded(b.get()) + key);
  return {pub, proof, sha512(pub + proof + key), key};
}
HapRecords::HapRecords(QByteArray w, QByteArray r)
    : write_(std::move(w)), read_(std::move(r)) {}
HapRecords::~HapRecords() {
  OPENSSL_cleanse(write_.data(), size_t(write_.size()));
  OPENSSL_cleanse(read_.data(), size_t(read_.size()));
}
QByteArray HapRecords::encode(const QByteArray &plain) {
  QByteArray output;
  for (qsizetype pos = 0; pos < plain.size(); pos += 1024) {
    if (txExhausted_)
      throw Error("HAP 发送 nonce 已耗尽");
    const auto chunk = plain.mid(pos, 1024);
    QByteArray length;
    length.append(char(chunk.size()));
    length.append(char(chunk.size() >> 8));
    output += length + seal(write_, nonce(tx), chunk, length);
    if (tx == UINT64_MAX)
      txExhausted_ = true;
    else
      ++tx;
  }
  return output;
}
QByteArray HapRecords::decode(const QByteArray &size,
                              const QByteArray &cipher) {
  if (size.size() != 2)
    throw Error("HAP 头部无效");
  const int length = uint8_t(size[0]) + (uint8_t(size[1]) << 8);
  if (length < 1 || length > 1024 || cipher.size() != length + 16)
    throw Error("HAP 记录长度无效");
  if (rxExhausted_)
    throw Error("HAP 接收 nonce 已耗尽");
  auto result = unseal(read_, nonce(rx), cipher, size);
  if (rx == UINT64_MAX)
    rxExhausted_ = true;
  else
    ++rx;
  return result;
}
QByteArray alac(std::span<const int16_t> samples) {
  if (samples.empty() || samples.size() % 2 || samples.size() > 704)
    throw Error("ALAC 必须包含 1..352 个立体声帧");
  QByteArray out;
  out.reserve(qsizetype(samples.size() * 2 + 8));
  uint64_t bits = 0;
  int count = 0;
  auto put = [&](uint64_t value, int width) {
    bits = (bits << width) | value;
    count += width;
    while (count >= 8) {
      count -= 8;
      out.append(char(bits >> count));
      bits &= (uint64_t(1) << count) - 1;
    }
  };
  put(1, 3);
  put(0, 4);
  put(0, 12);
  put(1, 1);
  put(0, 2);
  put(1, 1);
  put(samples.size() / 2, 32);
  for (auto sample : samples)
    put(uint16_t(sample), 16);
  put(7, 3);
  if (count)
    put(0, 8 - count);
  return out;
}
QByteArray audioPacket(const QByteArray &key, std::span<const int16_t> samples,
                       uint16_t seq, uint32_t ts, uint64_t counter,
                       bool first) {
  QByteArray header;
  header.append(char(0x80));
  header.append(char(first ? 0xe0 : 0x60));
  appendBe(header, seq, 2);
  appendBe(header, ts, 4);
  appendBe(header, 0, 4);
  auto iv = nonce(counter);
  return header + seal(key, iv, alac(samples), header.mid(4, 8)) + iv.mid(4);
}
} // namespace airplay
