// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QtTypes>

namespace video_editor::desktop_ui {

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
  };
  Q_ENUM(ClipHitRegion)

  enum class EditMode {
    Move,
    TrimIn,
    TrimOut,
  };
  Q_ENUM(EditMode)

  enum class EditIntent {
    Normal,
    Ripple,
    Overwrite,
  };
  Q_ENUM(EditIntent)

  explicit TimelineWidget(QWidget* parent = nullptr);

  void setTimeline(qint64 duration, qint64 timeScale, QVector<TimelineTrackView> tracks,
                   QVector<TimelineClipView> clips);
  void setTracks(QVector<TimelineTrackView> tracks);
  void setClips(QVector<TimelineClipView> clips);
  void setDuration(qint64 duration, qint64 timeScale);

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

public slots:
  void setPlayhead(qint64 position);
  void setPixelsPerSecond(double pixelsPerSecond);
  void zoomIn();
  void zoomOut();
  void zoomToFit();
  void setSnapThresholdPixels(int threshold);
  void setFrameRate(quint32 numerator, quint32 denominator);
  void nudgeActiveClipByFrames(int frameCount, EditIntent intent = EditIntent::Normal);

signals:
  void seekRequested(qint64 position);
  void clipActivated(const QString& clipId);
  void clipContextMenuRequested(const QString& clipId, const QPoint& globalPosition);
  void playheadChanged(qint64 position);
  void zoomChanged(double pixelsPerSecond);
  // Deltas use the widget's exact integer time scale. Control requests a ripple
  // edit and Alt requests overwrite during pointer gestures.
  void clipEditPreview(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                       qint64 durationDelta, EditMode mode, EditIntent intent, bool snapped);
  void clipEditCommitted(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                         qint64 durationDelta, EditMode mode, EditIntent intent, bool snapped);
  void clipEditCanceled(const QString& clipId);

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
  [[nodiscard]] int clipAt(const QPoint& position) const;
  [[nodiscard]] int activeClipIndex() const;
  [[nodiscard]] int trackAtY(int viewportY) const;
  [[nodiscard]] EditIntent editIntent(Qt::KeyboardModifiers modifiers) const noexcept;
  [[nodiscard]] qint64 roundedFrameDuration() const noexcept;
  void beginClipGesture(int clipIndex, const QPoint& position, ClipHitRegion hitRegion);
  void updateClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers,
                         bool allowAutoScroll);
  void finishClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers);
  void cancelClipGesture();
  void updateHoverCursor(const QPoint& position);
  void autoScrollForDrag(const QPoint& position);
  void updateScrollBars();
  void revealPlayhead();
  void movePlayheadFromPointer(int viewportX, bool emitRequest);

  QVector<TimelineTrackView> tracks_;
  QVector<TimelineClipView> clips_;
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
  int trim_handle_pixels_{7};
  int auto_scroll_margin_{28};
  quint32 frame_rate_numerator_{30};
  quint32 frame_rate_denominator_{1};
  QString active_clip_id_;

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
  };
  ClipGesture clip_gesture_;
};

} // namespace video_editor::desktop_ui

Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::ClipHitRegion)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::EditMode)
Q_DECLARE_METATYPE(video_editor::desktop_ui::TimelineWidget::EditIntent)
