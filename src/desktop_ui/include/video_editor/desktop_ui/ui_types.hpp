// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QStringList>
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
  bool visible{true};
  bool targeted{true};
};

// Marker and gap identities are presentation values.  A gap key is scoped to the
// revision that produced this view and must be resolved by the controller again
// before it edits the model.
struct TimelineMarkerView {
  QString id;
  QString displayName;
  qint64 start{0};
  qint64 duration{0};
  QColor color{238, 183, 72};
  bool selected{false};
};

struct TimelineGapView {
  QString key;
  QString trackId;
  int trackIndex{0};
  qint64 start{0};
  qint64 duration{0};
  bool selected{false};
};

enum class TimelineSnapKind {
  None,
  Frame,
  Marker,
  Playhead,
  ClipEdge,
};

struct TimelineSnapRequest {
  qint64 proposedTime{0};
  int thresholdPixels{0};
  QStringList excludedClipIds;
  // A marker drag must not immediately snap back to the marker being moved.
  // This is optional for clip gestures and empty while adding a new marker.
  QString excludedMarkerId;
  bool forMarker{false};
};

struct TimelineSnapResult {
  qint64 time{0};
  TimelineSnapKind kind{TimelineSnapKind::None};
  QString label;
  [[nodiscard]] bool snapped() const noexcept {
    return kind != TimelineSnapKind::None;
  }
};

struct AudioTrackView {
  QString displayName;
  bool muted{false};
  bool soloed{false};
  double gain_db{0.0};
  double pan{0.0};
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
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineSnapKind)
