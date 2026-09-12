// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/timeline_cursors.hpp"
#include "video_editor/desktop_ui/ui_types.hpp"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QtTypes>

#include <functional>

namespace video_editor::desktop_ui {

struct TransitionView {
  QString id;
  QString outgoingClipId;
  QString incomingClipId;
  qint64 start{0};
  qint64 duration{0};
  int trackIndex{0};
  QString kind;
  bool selected{false};
};

class TimelineWidget final : public QAbstractScrollArea {
  Q_OBJECT
  Q_PROPERTY(
      double pixelsPerSecond READ pixelsPerSecond WRITE setPixelsPerSecond NOTIFY zoomChanged)
  Q_PROPERTY(qint64 playhead READ playhead WRITE setPlayhead NOTIFY playheadChanged)

public:
  enum class ClipHitRegion {
    None,
    Body,
    TrimIn,
    TrimOut,
    Transition,
    Envelope,
    EnvelopeKeyframe,
  };
  Q_ENUM(ClipHitRegion)

  enum class EditMode {
    Move,
    TrimIn,
    TrimOut,
    Roll,
    Slip,
    Slide,
  };
  Q_ENUM(EditMode)

  enum class ToolMode {
    Select,
    RippleTrim,
    OverwriteTrim,
    Roll,
    Slip,
    Slide,
    TrackSelectForward,
    Razor,
    Hand,
    Zoom,
    Pen,
  };
  Q_ENUM(ToolMode)

  enum class EditIntent {
    Normal,
    Ripple,
    Overwrite,
  };
  Q_ENUM(EditIntent)

  explicit TimelineWidget(QWidget* parent = nullptr);

  void setTimeline(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                   QVector<TimelineClipView> clips);
  void setTimeline(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                   QVector<TimelineClipView> clips, QVector<TimelineMarkerView> markers,
                   QVector<TimelineGapView> gaps);
  void setTracks(QVector<TimelineTrackView> tracks);
  void setClips(QVector<TimelineClipView> clips);
  void setDuration(qint64 duration, qint64 timeScale);
  void setMarkers(QVector<TimelineMarkerView> markers);
  void setProgramMarks(std::optional<qint64> markIn, std::optional<qint64> markOut);
  [[nodiscard]] std::optional<qint64> programMarkIn() const noexcept {
    return program_mark_in_;
  }
  [[nodiscard]] std::optional<qint64> programMarkOut() const noexcept {
    return program_mark_out_;
  }
  void setGaps(QVector<TimelineGapView> gaps);
  void setSnapResolver(std::function<TimelineSnapResult(const TimelineSnapRequest&)> resolver);
  void setTransitions(const QVector<TransitionView>& transitions);
  [[nodiscard]] QVector<TransitionView> transitions() const noexcept;

  [[nodiscard]] qint64 duration() const noexcept {
    return duration_;
  }
  [[nodiscard]] qint64 timeScale() const noexcept {
    return time_scale_;
  }
  [[nodiscard]] qint64 playhead() const noexcept {
    return playhead_;
  }
  [[nodiscard]] double pixelsPerSecond() const noexcept {
    return pixels_per_second_;
  }
  [[nodiscard]] const QVector<TimelineTrackView>& tracks() const noexcept {
    return tracks_;
  }
  [[nodiscard]] const QVector<TimelineClipView>& clips() const noexcept {
    return clips_;
  }
  [[nodiscard]] const QVector<TimelineMarkerView>& markers() const noexcept {
    return markers_;
  }
  [[nodiscard]] const QVector<TimelineGapView>& gaps() const noexcept {
    return gaps_;
  }
  [[nodiscard]] QStringList selectedClipIds() const;
  [[nodiscard]] QString activeClipId() const noexcept {
    return active_clip_id_;
  }
  [[nodiscard]] ToolMode toolMode() const noexcept {
    return tool_mode_;
  }
  [[nodiscard]] TimelineCursorKind hoverCursorKind(
      const QPoint& viewportPosition,
      Qt::KeyboardModifiers modifiers = Qt::NoModifier) const;
  [[nodiscard]] int visibleClipCount() const noexcept {
    return visible_clip_count_;
  }
  [[nodiscard]] int snapThresholdPixels() const noexcept {
    return snap_threshold_pixels_;
  }
  [[nodiscard]] quint32 frameRateNumerator() const noexcept {
    return frame_rate_numerator_;
  }
  [[nodiscard]] quint32 frameRateDenominator() const noexcept {
    return frame_rate_denominator_;
  }
  [[nodiscard]] qint64 frameStep() const noexcept;
  [[nodiscard]] ClipHitRegion clipHitRegionAt(const QPoint& viewportPosition) const;
  [[nodiscard]] int transitionAt(const QPoint& viewportPosition) const;

public slots:
  void clearTransitionSelection();
  void setPlayhead(qint64 position);
  void setPixelsPerSecond(double pixelsPerSecond);
  void setPixelsPerSecond(double pixelsPerSecond, int anchorViewportX);
  void zoomIn();
  void zoomOut();
  void zoomAt(int viewportX, double factor);
  void zoomToFit();
  void zoomToSelection();
  void setSnapEnabled(bool enabled);
  [[nodiscard]] bool snapEnabled() const noexcept {
    return snap_enabled_;
  }
  void setFollowPlayheadEnabled(bool enabled);
  [[nodiscard]] bool followPlayheadEnabled() const noexcept {
    return follow_playhead_enabled_;
  }
  void setSnapThresholdPixels(int threshold);
  void setFrameRate(quint32 numerator, quint32 denominator);
  void setToolMode(ToolMode mode);
  void setTrackHeight(int height);
  [[nodiscard]] int trackHeight() const noexcept {
    return track_height_;
  }
  void setLinkedSelectionEnabled(bool enabled);
  [[nodiscard]] bool linkedSelectionEnabled() const noexcept {
    return linked_selection_enabled_;
  }
  void nudgeActiveClipByFrames(int frameCount, EditIntent intent = EditIntent::Normal);

signals:
  void transitionActivated(const QString& transitionId);
  void transitionDurationEdited(const QString& transitionId, qint64 duration);
  void transitionRemoved(const QString& transitionId);
  void transitionPresetChanged(const QString& transitionId, const QString& kind);
  void seekRequested(qint64 position);
  void clipActivated(const QString& clipId);
  void clipInspectorRequested(const QString& clipId);
  void clipCutAtRequested(const QString& clipId, qint64 uiTime);
  void clipDeleteRequested(const QString& clipId, bool ripple);
  void clipReplaceMediaRequested(const QString& clipId);
  void clipUnlinkRequested(const QString& clipId);
  void clipEnabledToggledRequested(const QString& clipId, bool enabled);
  void clipFadeEdited(const QString& clipId, qint64 fadeIn, qint64 fadeOut);
  void clipAudioGainEdited(const QString& clipId, double gainDb);
  void clipOpacityEdited(const QString& clipId, double opacity);
  void clipVolumeKeyframeUpserted(const QString& clipId, const QString& keyframeId,
                                  qint64 localTime, double gainDb);
  void clipVolumeKeyframeRemoved(const QString& clipId, const QString& keyframeId);
  void clipOpacityKeyframeUpserted(const QString& clipId, const QString& keyframeId,
                                   qint64 localTime, double opacity);
  void clipOpacityKeyframeRemoved(const QString& clipId, const QString& keyframeId);
  void clipFreezeFrameRequested(const QString& clipId, qint64 uiTime);
  void followPlayheadDisabled();
  void playheadChanged(qint64 position);
  void zoomChanged(double pixelsPerSecond);
  void toolModeChanged(ToolMode mode);
  void addEditsAtRequested(qint64 uiTime);
  // Deltas use the widget's exact integer time scale. Control requests a ripple
  // edit and Alt requests overwrite during pointer gestures.
  void clipEditPreview(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                       qint64 durationDelta, EditMode mode, EditIntent intent, bool snapped);
  void clipEditCommitted(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                         qint64 durationDelta, EditMode mode, EditIntent intent, bool snapped);
  void clipEditCanceled(const QString& clipId);
  // Rich gesture contract. The old single-clip signals above remain available
  // for callers that have not yet adopted multi-selection.
  void clipSelectionChanged(const QStringList& clipIds, const QString& activeClipId);
  void clipBatchEditPreview(const QStringList& clipIds, int destinationTrackIndex,
                            qint64 startDelta, qint64 durationDelta, EditMode mode,
                            EditIntent intent, const TimelineSnapResult& snap);
  void clipBatchEditCommitted(const QStringList& clipIds, int destinationTrackIndex,
                              qint64 startDelta, qint64 durationDelta, EditMode mode,
                              EditIntent intent, const TimelineSnapResult& snap);
  void clipBatchEditCanceled(const QStringList& clipIds);
  void frameNudgeRequested(const QStringList& clipIds, int frameCount, EditIntent intent);
  void markerSelectionChanged(const QString& markerId);
  void markerMovePreview(const QString& markerId, qint64 start, const TimelineSnapResult& snap);
  void markerMoveCommitted(const QString& markerId, qint64 start, const TimelineSnapResult& snap);
  void markerMoveCanceled(const QString& markerId);
  void markerDurationCommitted(const QString& markerId, qint64 duration,
                               const TimelineSnapResult& snap);
  void markerAddRequested(qint64 start);
  void markerRenameRequested(const QString& markerId, const QString& displayName);
  void markerRemoveRequested(const QString& markerId);
  void gapSelectionChanged(const QString& gapKey);
  void closeGapRequested(const QString& gapKey);
  void insertAtPlayheadRequested();
  void trackAddRequested(TrackKind kind);
  void trackRenameRequested(const QString& trackId, const QString& displayName);
  void trackReorderRequested(const QString& trackId, int destinationIndex);
  void trackLockToggled(const QString& trackId, bool locked);
  void trackVisibilityToggled(const QString& trackId, bool visible);
  void trackTargetToggled(const QString& trackId, bool targeted);
  void trackRemoveRequested(const QString& trackId);
  void editDestinationRejected(const QString& message);
  void editPreviewStatusChanged(const QString& status);

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  [[nodiscard]] int contentWidth() const;
  [[nodiscard]] int contentHeight() const;
  [[nodiscard]] int xForTime(qint64 time) const;
  [[nodiscard]] qint64 timeForX(int viewportX) const;
  [[nodiscard]] QRect clipRect(const TimelineClipView& clip) const;
  [[nodiscard]] QRect transitionRect(const TransitionView& transition) const;
  [[nodiscard]] int clipAt(const QPoint& position) const;
  [[nodiscard]] int activeClipIndex() const;
  [[nodiscard]] int markerAt(const QPoint& position) const;
  [[nodiscard]] int gapAt(const QPoint& position) const;
  [[nodiscard]] int trackHeaderAt(const QPoint& position) const;
  [[nodiscard]] int trackAtY(int viewportY) const;
  [[nodiscard]] EditIntent editIntent(Qt::KeyboardModifiers modifiers) const noexcept;
  [[nodiscard]] qint64 roundedFrameDuration() const noexcept;
  [[nodiscard]] TimelineSnapResult resolveSnap(qint64 proposedTime, const QStringList& exclusions,
                                               bool forMarker, Qt::KeyboardModifiers modifiers,
                                               const QString& excludedMarkerId = {}) const;
  [[nodiscard]] EditMode gestureMode(ClipHitRegion hit) const noexcept;
  void selectClip(int clipIndex, Qt::KeyboardModifiers modifiers);
  void selectForwardFromClip(int clipIndex, bool includeTracksBelow);
  void clearClipSelection();
  void selectMarker(int markerIndex);
  void selectGap(int gapIndex);
  void beginClipGesture(int clipIndex, const QPoint& position, ClipHitRegion hitRegion);
  void beginEnvelopeGesture(int clipIndex, const QPoint& position, ClipHitRegion hitRegion,
                            Qt::KeyboardModifiers modifiers);
  void updateClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers,
                         bool allowAutoScroll);
  void finishClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers);
  void cancelClipGesture();
  void cancelMarkerGesture();
  void beginPanGesture(const QPoint& position);
  void updatePanGesture(const QPoint& position);
  void finishPanGesture();
  void cancelPanGesture();
  void zoomToMarquee(const QRect& rect);
  void updateHoverCursor(const QPoint& position, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
  void autoScrollForDrag(const QPoint& position);
  void updateScrollBars();
  void revealPlayhead();
  void movePlayheadFromPointer(int viewportX, bool emitRequest);
  [[nodiscard]] QRect envelopeBand(const QRect& clipRect) const;
  [[nodiscard]] double envelopeExtraAt(const TimelineClipView& clip, qint64 localTime) const;
  [[nodiscard]] double displayedEnvelopeValue(const TimelineClipView& clip, qint64 localTime) const;
  [[nodiscard]] int envelopeYForValue(const QRect& band, const TimelineClipView& clip,
                                      double value) const;
  [[nodiscard]] double envelopeValueForY(const QRect& band, const TimelineClipView& clip,
                                         int viewportY) const;
  [[nodiscard]] int envelopeKeyframeAt(int clipIndex, const QPoint& position) const;
  [[nodiscard]] bool moveDestinationIsValid(int trackIndex) const;
  void reportEditDestinationRejected(const QString& message);
  void syncClipSelectionChrome();
  void emitEditPreviewStatus();
  [[nodiscard]] QString editPreviewStatusText() const;
  struct NeighborPreviewDelta {
    int clipIndex{-1};
    qint64 startDelta{0};
    qint64 durationDelta{0};
  };
  [[nodiscard]] QVector<NeighborPreviewDelta> neighborPreviewDeltas() const;

  QVector<TimelineTrackView> tracks_;
  QVector<TimelineClipView> clips_;
  QVector<TimelineMarkerView> markers_;
  std::optional<qint64> program_mark_in_;
  std::optional<qint64> program_mark_out_;
  QVector<TimelineGapView> gaps_;
  std::function<TimelineSnapResult(const TimelineSnapRequest&)> snap_resolver_;
  qint64 duration_{10 * 60 * 48'000};
  qint64 time_scale_{48'000};
  qint64 playhead_{0};
  double pixels_per_second_{90.0};
  int track_height_{58};
  int header_width_{176};
  int ruler_height_{30};
  int visible_clip_count_{0};
  bool dragging_playhead_{false};
  int snap_threshold_pixels_{8};
  bool snap_enabled_{true};
  bool follow_playhead_enabled_{true};
  bool marquee_active_{false};
  bool marquee_zoom_{false};
  QPoint marquee_origin_;
  QRect marquee_rect_;
  int trim_handle_pixels_{7};
  int auto_scroll_margin_{28};
  quint32 frame_rate_numerator_{30};
  quint32 frame_rate_denominator_{1};
  QString active_clip_id_;
  QString active_track_id_;
  QString selection_anchor_id_;
  QString active_marker_id_;
  QString active_gap_key_;
  ToolMode tool_mode_{ToolMode::Select};
  bool linked_selection_enabled_{true};

  struct PanGesture {
    bool pointerDown{false};
    bool dragging{false};
    QPoint pressPosition;
    int pressScrollX{0};
    int pressScrollY{0};
  };
  PanGesture pan_gesture_;

  struct ClipGesture {
    bool pointerDown{false};
    bool dragging{false};
    int clipIndex{-1};
    QPoint pressPosition;
    qint64 pressPointerTime{0};
    qint64 originalStart{0};
    qint64 originalDuration{0};
    int originalTrackIndex{-1};
    EditMode mode{EditMode::Move};
    int destinationTrackIndex{-1};
    qint64 startDelta{0};
    qint64 durationDelta{0};
    EditIntent intent{EditIntent::Normal};
    bool snapped{false};
    qint64 snapTime{0};
    TimelineSnapResult snap;
    QStringList clipIds;
    bool rollCutAtStart{false};
    int rollPartnerIndex{-1};
    qint64 rollMinimumDelta{0};
    qint64 rollMaximumDelta{0};
    bool editingFade{false};
    bool fadeInEdge{true};
    qint64 originalFadeIn{0};
    qint64 originalFadeOut{0};
    qint64 targetFadeIn{0};
    qint64 targetFadeOut{0};
    bool editingEnvelope{false};
    bool envelopeSetStatic{false};
    QString envelopeKeyframeId;
    qint64 envelopeLocalTime{0};
    double envelopeKeyValue{0.0};
    double envelopeStaticValue{0.0};
    TimelineClipView::EnvelopeKind envelopeKind{TimelineClipView::EnvelopeKind::None};
    bool sourceTrackLocked{false};
    bool destinationRejectedReported{false};
  };
  ClipGesture clip_gesture_;

  struct MarkerGesture {
    bool pointerDown{false};
    bool dragging{false};
    bool resizingDuration{false};
    int markerIndex{-1};
    QPoint pressPosition;
    qint64 originalStart{0};
    qint64 originalDuration{0};
    qint64 targetStart{0};
    qint64 targetDuration{0};
    TimelineSnapResult snap;
  };
  MarkerGesture marker_gesture_;

  struct TransitionGesture {
    bool pointerDown{false};
    bool dragging{false};
    int index{-1};
    QPoint pressPosition;
    qint64 pressPointerTime{0};
    qint64 originalStart{0};
    qint64 originalDuration{0};
    bool draggingLeft{false};
  };
  TransitionGesture transition_gesture_;

  QVector<TransitionView> transitions_;
  int selected_transition_index_{-1};
  int transition_handle_pixels_{8};
};

} // namespace video_editor::desktop_ui

Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::ClipHitRegion)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::EditMode)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::EditIntent)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::ToolMode)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineSnapResult)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TransitionView)
