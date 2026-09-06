// SPDX-License-Identifier: MPL-2.0
#include "timecode_util.hpp"

#include <QStringList>
#include <QtGlobal>

#include <cmath>

namespace video_editor::app {
namespace {

[[nodiscard]] bool parseHmsf(const QString& text, const edit::Rate& rate, edit::Time& out) {
  const auto parts = text.split(QLatin1Char(':'));
  if (parts.size() != 4) {
    return false;
  }
  bool ok = false;
  const qint64 hours = parts[0].toLongLong(&ok);
  if (!ok || hours < 0) {
    return false;
  }
  const qint64 minutes = parts[1].toLongLong(&ok);
  if (!ok || minutes < 0 || minutes >= 60) {
    return false;
  }
  const qint64 seconds = parts[2].toLongLong(&ok);
  if (!ok || seconds < 0 || seconds >= 60) {
    return false;
  }
  const qint64 frames = parts[3].toLongLong(&ok);
  if (!ok || frames < 0) {
    return false;
  }
  const std::int64_t nominal_fps = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(std::llround(static_cast<double>(rate.numerator()) /
                                                static_cast<double>(rate.denominator()))));
  if (frames >= nominal_fps) {
    return false;
  }
  const std::int64_t frame_number = ((hours * 3600) + (minutes * 60) + seconds) * nominal_fps + frames;
  out = rate.frameTime(frame_number);
  return true;
}

} // namespace

QString formatTimecode(const edit::Time time, const edit::Rate& rate) {
  const std::int64_t frame_number =
      rate.framesAt(time, edit::RoundingMode::Floor);
  const std::int64_t nominal_fps = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(std::llround(static_cast<double>(rate.numerator()) /
                                                static_cast<double>(rate.denominator()))));
  const std::int64_t frames = frame_number % nominal_fps;
  const std::int64_t total_seconds = frame_number / nominal_fps;
  const std::int64_t seconds = total_seconds % 60;
  const std::int64_t minutes = (total_seconds / 60) % 60;
  const std::int64_t hours = total_seconds / 3'600;
  return QStringLiteral("%1:%2:%3:%4")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(frames, 2, 10, QLatin1Char('0'));
}

std::optional<edit::Time> parseTimecodeInput(const QString& text, const edit::Rate& rate,
                                             const std::uint32_t ui_timescale) {
  const QString trimmed = text.trimmed();
  if (trimmed.isEmpty()) {
    return std::nullopt;
  }
  if (trimmed.contains(QLatin1Char(':'))) {
    edit::Time parsed;
    if (parseHmsf(trimmed, rate, parsed)) {
      return parsed;
    }
    return std::nullopt;
  }
  if (trimmed.endsWith(QLatin1Char('s'), Qt::CaseInsensitive)) {
    bool ok = false;
    const double seconds = trimmed.left(trimmed.size() - 1).toDouble(&ok);
    if (!ok || seconds < 0.0) {
      return std::nullopt;
    }
    const qint64 value = static_cast<qint64>(std::llround(seconds * static_cast<double>(ui_timescale)));
    return edit::Time(value, ui_timescale);
  }
  bool ok = false;
  const qint64 compact = trimmed.toLongLong(&ok);
  if (!ok || compact < 0) {
    return std::nullopt;
  }
  edit::Time parsed;
  const QString padded = QStringLiteral("%1").arg(compact, 8, 10, QLatin1Char('0'));
  if (parseHmsf(QStringLiteral("%1:%2:%3:%4")
                    .arg(padded.mid(0, 2))
                    .arg(padded.mid(2, 2))
                    .arg(padded.mid(4, 2))
                    .arg(padded.mid(6, 2)),
                rate, parsed)) {
    return parsed;
  }
  return edit::Time(compact, ui_timescale);
}

} // namespace video_editor::app
