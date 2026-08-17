// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QColor>
#include <QImage>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <array>

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
  QString metadataTitle;
  QImage thumbnail;
  bool offline{false};
  bool contentChanged{false};
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
  // Stable edit-model track identity; UI telemetry must not be indexed by
  // presentation order because tracks can be reordered or removed.
  QString id;
  QString displayName;
  bool muted{false};
  bool soloed{false};
  double gain_db{0.0};
  double pan{0.0};
  std::array<float, 2> peak_dbfs{-60.0F, -60.0F};
  std::array<float, 2> rms_dbfs{-60.0F, -60.0F};
  bool meter_active{false};
  bool meter_stale{true};
  struct Effect {
    struct Parameter {
      QString id;
      QVariant value;
    };
    QString id;
    QString type;
    QString displayName;
    QVector<Parameter> parameters;
  };
  QVector<Effect> effects;
};

struct AudioTrackMeterView {
  QString id;
  std::array<float, 2> peak_dbfs{-60.0F, -60.0F};
  std::array<float, 2> rms_dbfs{-60.0F, -60.0F};
  bool active{false};
  bool stale{true};
};

// Times are expressed in the TimelineWidget time scale supplied by the caller.
// The presentation model intentionally carries no edit-model types so the UI can
// be integrated before that module is linked.
struct WaveformBucketView {
  float minimum{-1.0F};
  float maximum{1.0F};
};

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
  QVector<WaveformBucketView> waveform;
};

struct AssetMetadataView {
  QString assetId;
  QString title;
  QStringList tags;
  QString notes;
  int rating{0};
};

struct CacheEntryView {
  QString assetId;
  QString displayName;
  QString kindText;
  qint64 bytes{0};
  qint64 lastAccessUtcMs{0};
};

struct CacheInventoryView {
  qint64 totalBytes{0};
  qint64 budgetBytes{0};
  QVector<CacheEntryView> entries;
};

struct EffectView {
  QString id;
  QString displayName;
  QString category;
  bool accelerated{false};
};

enum class KeyframeInterpolationView {
  Hold,
  Linear,
  Bezier,
};

struct KeyframeView {
  QString id;
  qint64 time{0};
  double value{0.0};
  KeyframeInterpolationView interpolation{KeyframeInterpolationView::Linear};
  QPointF incomingControl{};
  QPointF outgoingControl{};
};

struct EffectParameterView {
  QString effectId;
  QString effectName;
  QString parameterId;
  QString displayName;
  QVariant value;
  qint64 duration{1};
  QVector<KeyframeView> keyframes;
};

// Caption/transcription views deliberately contain only presentation values.
// The controller converts these to edit-model entities after validating the
// captured revision; the widget never mutates the project directly.
struct CaptionWordView final {
  QString id;
  QString text;
  qint64 start{0};
  qint64 end{0};
  double probability{1.0};
};

struct CaptionStyleView final {
  QString fontFamily{QStringLiteral("sans-serif")};
  double fontSize{48.0};
  QColor textColor{Qt::white};
  QColor backgroundColor{0, 0, 0, 178};
  bool bold{false};
  bool italic{false};
  QString alignment{QStringLiteral("center")};
  double verticalPosition{0.9};
  double safeMargin{0.05};
  double outlineWidth{0.0};
  QColor outlineColor{Qt::black};
};

struct CaptionRowView final {
  QString id;
  QString timecode;
  QString text;
  QString language;
  qint64 start{0};
  qint64 end{0};
  QVector<CaptionWordView> words;
  CaptionStyleView style{};
  double confidence{1.0};
  bool suggested{false};
};

enum class TranscriptionState {
  Idle,
  ModelMissing,
  Downloading,
  Ready,
  Running,
  Cancelling,
  Failed,
};

struct ModelDownloadView final {
  QString modelId;
  QString filename;
  QString digestAlgorithm;
  QString digest;
  qint64 receivedBytes{0};
  qint64 totalBytes{0};
  QString status;
  TranscriptionState state{TranscriptionState::ModelMissing};
};

struct TranscriptionOptionsView final {
  QString modelId{QStringLiteral("base")};
  QString language;
  bool translate{false};
  bool preferVulkan{false};
  bool wordTimestamps{true};
  int threadCount{0};
};

struct CaptionProposalView final {
  QString id;
  QString kind;
  QString summary;
  QString previewRange;
  QString confidence;
  bool selected{true};
  bool accepted{false};
  bool rejected{false};
};

} // namespace video_editor::desktop_ui

Q_DECLARE_METATYPE(video_editor::desktop_ui::Workspace)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TrackKind)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineSnapKind)
Q_DECLARE_METATYPE(video_editor::desktop_ui::KeyframeInterpolationView)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TranscriptionState)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TranscriptionOptionsView)
