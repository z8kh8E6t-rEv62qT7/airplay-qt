#include "FairPlaySetup.h"

namespace airplay {
namespace {
// FairPlay v3 setup replies from pyatv's PlayFair, server_auth.py at
// b277a4c8222ecdcbaab8a24e3e713ca44765adb4. This implements setup only;
// it does not decrypt media keys or implement MFi certificate authentication.
//
// Copyright (c) 2020 Pierre Ståhl
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
const QByteArray FairPlayReplies[] = {
    QByteArray::fromHex(
        "46504c59030102000000008202000f9f3f9e0a2521dbdf312ab2bfb29e8d232b6376a8"
        "c81870"
        "1d22ae93d82737feaf9db4fdf41c2dba9d1f49caaabf6591ac1f7bc6f7e0663d21afe0"
        "15659"
        "53eab81f418ceed095adb7c3d0e254909a79831d49c3982973434facb42c63a1cd911a"
        "6fe941"
        "a8a6d4a743b46c3a7649e44c78955e49d8155009549c4e2f7a3f6d5ba"),
    QByteArray::fromHex(
        "46504c5903010200000000820201cf32a25714b2524f8aa0ad7af164e37bcf4424e200"
        "047efc0"
        "ad67afcd95ded1c2730bb591b962ed63a9c4ded88ba8fc78de64d91ccfd5c7b56da88e"
        "31f5c"
        "ceafc7431995a01665a54e1939d25b94db64b9e45d8d063e1e6af07e9656162b0efa40"
        "4275e"
        "a5a44d9591c7256b9fbe6513898b80227721988571650942ad946688a"),
    QByteArray::fromHex(
        "46504c5903010200000000820202c169a352eeed35b18cdd9c58d64f16c1519a89eb53"
        "17bd0d"
        "4336cd68f638ff9d016a5b52b7fa9216b2b65482c78444118121a2c7fed83db7119e91"
        "82aa"
        "d7d18c7063e2a457555910af9e0efc76347d164043807f581ee4fbe42ca9dedc1b5eb2"
        "a3aa3"
        "d2ecd59e7eee70b3629f22afd161d877353ddb99adc8e07006e56f850ce"),
    QByteArray::fromHex(
        "46504c59030102000000008202039001e1727e0f57f9f5880db104a6257a23f5cfff1a"
        "bbe1e9"
        "3045251afb97eb9fc0011ebe0f3a81df5b691d76acb2f7a5c708e3d328f56bb39dbde5"
        "f29c"
        "8a17f481487e3ae863c678325422e6f78e166d18aa7fd636258bce28726f661f738893"
        "ce4431"
        "1e4be6c0535193e5ef72e8686233729c227d820c999445d89246c8c359")};

} // namespace
std::optional<QByteArray> FairPlaySetup::respond(const QByteArray &body) {
  // Check the complete header before indexing mode or reading the suffix.
  if (body.size() < 12 ||
      body.first(6) != QByteArray::fromHex("46504c590301") || body[7] != 0)
    return std::nullopt;
  if (body[6] == 1) {
    if (body.size() != 16 || body.mid(8, 4) != QByteArray::fromHex("00000004"))
      return std::nullopt;
    const auto mode = uint8_t(body[14]);
    if (mode >= 4)
      return std::nullopt;
    // A new first message restarts the handshake on this connection only.
    awaitingFinish_ = true;
    complete_ = false;
    return FairPlayReplies[mode];
  }
  if (body[6] != 3 || !awaitingFinish_ || body.size() != 164 ||
      body.mid(8, 4) != QByteArray::fromHex("00000098"))
    return std::nullopt;
  awaitingFinish_ = false;
  complete_ = true;
  return QByteArray::fromHex("46504c590301040000000014") + body.right(20);
}
} // namespace airplay
