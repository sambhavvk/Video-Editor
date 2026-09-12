// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QCursor>
#include <QIcon>

namespace video_editor::desktop_ui {

enum class TimelineCursorKind {
  Arrow,
  SelectMove,
  Trim,
  RippleTrim,
  OverwriteTrim,
  Roll,
  Slip,
  Slide,
  Razor,
  HandOpen,
  HandClosed,
  ZoomIn,
  ZoomOut,
  TrackSelectForward,
  Pen,
  Envelope,
};

[[nodiscard]] QCursor timelineCursor(TimelineCursorKind kind);
[[nodiscard]] QIcon timelineToolIcon(TimelineCursorKind kind);
[[nodiscard]] QString timelineCursorName(TimelineCursorKind kind);

} // namespace video_editor::desktop_ui
