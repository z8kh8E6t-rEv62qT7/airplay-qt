#include "Message.h"
#include <array>
namespace i18n {
namespace {
struct Entry {
  const char *english;
  const char *chinese;
};
constexpr std::array<Entry, size_t(Id::Count)> catalogue{{
#define MESSAGE(id, english, chinese) {english, chinese},
#include "Messages.inc"
#undef MESSAGE
}};
} // namespace
Message::Message(const QString &literal) {
  if (!literal.isEmpty())
    data_ = {0, literal, QJsonArray{}};
}
Message::Message(Id id) : data_{1, int(id), QJsonArray{}} {}
Message Message::arg(const Message &value) const {
  auto copy = data_;
  auto args = copy.at(2).toArray();
  args.append(value.data_);
  if (copy.size() == 3)
    copy[2] = args;
  return Message(copy);
}
Message operator+(const Message &a, const Message &b) {
  if (a.isEmpty())
    return b;
  if (b.isEmpty())
    return a;
  return Message(QJsonArray{2, QJsonArray{a.data_, b.data_}, QJsonArray{}});
}
QString Message::render(Language language) const {
  if (data_.isEmpty())
    return {};
  if (data_.at(0).toInt() == 2) {
    QString result;
    for (const auto &part : data_.at(1).toArray())
      result += Message(part.toArray()).render(language);
    return result;
  }
  QString pattern;
  if (data_.at(0).toInt() == 1) {
    const int index = data_.at(1).toInt(-1);
    if (index < 0 || index >= int(catalogue.size()))
      return {};
    const auto &entry = catalogue[size_t(index)];
    pattern = QString::fromUtf8(language == Language::Chinese ? entry.chinese
                                                              : entry.english);
  } else {
    pattern = data_.at(1).toString();
  }
  const auto args = data_.at(2).toArray();
  if (args.isEmpty())
    return pattern;
  // Substitute once: percent signs in device names and system errors are data,
  // not additional placeholders. Numeric formatting is fixed at creation time.
  QString result;
  for (qsizetype i = 0; i < pattern.size();) {
    if (pattern[i] == '%' && i + 1 < pattern.size() && pattern[i + 1] >= '1' &&
        pattern[i + 1] <= '9') {
      qsizetype end = i + 2;
      int index = pattern[i + 1].unicode() - '0';
      if (end < pattern.size() && pattern[end] >= '0' && pattern[end] <= '9') {
        index = index * 10 + pattern[end].unicode() - '0';
        ++end;
      }
      if (index <= args.size()) {
        result += Message(args.at(index - 1).toArray()).render(language);
        i = end;
        continue;
      }
    }
    result += pattern[i++];
  }
  return result;
}
Message fromException(const std::exception &error) {
  if (const auto *known = dynamic_cast<const MessageError *>(&error))
    return known->message();
  return QString::fromUtf8(error.what());
}
} // namespace i18n
