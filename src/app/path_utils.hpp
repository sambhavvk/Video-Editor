// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <filesystem>
#include <string>

namespace video_editor::app {

[[nodiscard]] inline std::filesystem::path pathFromQString(const QString& value) {
#ifdef _WIN32
  return std::filesystem::path(value.toStdWString());
#else
  const QByteArray utf8 = value.toUtf8();
  return std::filesystem::path(std::string(utf8.constData(),
                                          static_cast<std::size_t>(utf8.size())));
#endif
}

[[nodiscard]] inline QString qStringFromPath(const std::filesystem::path& value) {
#ifdef _WIN32
  return QString::fromStdWString(value.native());
#else
  const auto& native = value.native();
  return QString::fromUtf8(native.data(), static_cast<qsizetype>(native.size()));
#endif
}

[[nodiscard]] inline std::string utf8StringFromPath(const std::filesystem::path& value) {
  const auto utf8 = value.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] inline std::filesystem::path pathFromUtf8String(const std::string& value) {
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
}

} // namespace video_editor::app
