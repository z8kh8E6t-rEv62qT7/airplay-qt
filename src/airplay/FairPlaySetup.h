#pragma once
#include <QByteArray>
#include <optional>
namespace airplay {
class FairPlaySetup {
public:
  bool complete() const { return complete_; }
  std::optional<QByteArray> respond(const QByteArray &body);

private:
  bool awaitingFinish_ = false, complete_ = false;
};
} // namespace airplay
