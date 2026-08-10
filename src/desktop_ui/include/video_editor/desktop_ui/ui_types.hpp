// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace video_editor::desktop_ui {

enum class Workspace {
  Import,
  Edit,
  AudioCaptions,
  Deliver,
};

enum class TrackKind {
  Video,
  Audio,
  Caption,
};

struct MediaItemView {
  QString id;
  QString displayName;
  QString filePath;
  QString durationText;
  QString formatText;
  bool offline{false};
  bool proxyAvailable{false};
  bool proxyRecommended{false};
  bool proxyGenerating{false};
};

struct TimelineTrackView {
  QString id;
  QString displayName;
  TrackKind kind{TrackKind::Video};
  bool muted{false};
  bool soloed{false};
  bool locked{false};
};

struct AudioTrackView {
  QString displayName;
  bool muted{false};
  bool soloed{false};
};

// Times are expressed in the TimelineWidget time scale supplied by the caller.
// The presentation model intentionally carries no edit-model types so the UI can
// be integrated before that module is linked.
struct TimelineClipView {
  QString id;
  QString displayName;
  int trackIndex{0};
  qint64 start{0};
  qint64 duration{0};
  QColor color{82, 126, 183};
  bool selected{false};
  bool offline{false};
  bool proxy{false};
};

struct EffectView {
  QString id;
  QString displayName;
  QString category;
  bool accelerated{false};
};

} // namespace video_editor::desktop_ui

Q_DECLARE_METATYPE(video_editor::desktop_ui::Workspace)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TrackKind)
