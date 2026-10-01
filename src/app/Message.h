#pragma once
#include "Language.h"
#include <QJsonArray>
#include <stdexcept>
#include <type_traits>
namespace i18n {
enum class Id {
#define MESSAGE(id, english, chinese) id,
#include "Messages.inc"
#undef MESSAGE
  Count
};
// Immutable, language-independent text with literal or translated arguments.
// Signals carry the built-in QJsonArray representation, never a custom Qt
// metatype whose function pointers could outlive a dynamically unloaded VST.
class Message {
public:
  Message() = default;
  Message(const char *literal) : Message(QString::fromUtf8(literal)) {}
  Message(const QString &literal);
  Message(const QJsonArray &data) : data_(data) {}
  explicit Message(Id id);
  operator QJsonArray() const { return data_; }
  bool isEmpty() const { return data_.isEmpty(); }
  QString render(Language language = Language::English) const;
  Message arg(const Message &value) const;
  Message arg(const Message &a, const Message &b) const {
    return arg(a).arg(b);
  }
  Message arg(const Message &a, const Message &b, const Message &c) const {
    return arg(a).arg(b).arg(c);
  }
  template <class T, class... Args>
    requires std::is_arithmetic_v<T>
  Message arg(T value, Args... args) const {
    return arg(Message(QStringLiteral("%1").arg(value, args...)));
  }
  friend Message operator+(const Message &a, const Message &b);
  bool operator==(const Message &) const = default;

private:
  QJsonArray data_;
};
inline Message text(Id id) { return Message(id); }
class MessageError : public std::runtime_error {
public:
  explicit MessageError(const Message &message)
      : std::runtime_error(message.render().toStdString()), message_(message) {}
  const Message &message() const { return message_; }

private:
  Message message_;
};
Message fromException(const std::exception &error);
} // namespace i18n
