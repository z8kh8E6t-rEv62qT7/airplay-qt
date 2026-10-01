#pragma once
#include <QString>
#include <cstdint>
#include <optional>
namespace i18n {
enum class Language : uint32_t { English = 0, Chinese = 1 };
constexpr bool valid(Language value) {
  return value == Language::English || value == Language::Chinese;
}
inline QString languageCode(Language value) {
  return value == Language::Chinese ? QStringLiteral("zh-CN")
                                    : QStringLiteral("en");
}
inline std::optional<Language> parseLanguage(const QString &code) {
  if (code == "en")
    return Language::English;
  if (code == "zh-CN")
    return Language::Chinese;
  return std::nullopt;
}
} // namespace i18n
