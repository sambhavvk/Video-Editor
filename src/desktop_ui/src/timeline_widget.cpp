// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFontMetrics>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyleOptionFocusRect>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace video_editor::desktop_ui {
namespace {

constexpr QColor kCanvas{24, 26, 31};
constexpr QColor kHeader{31, 34, 40};
constexpr QColor kRuler{28, 30, 36};
constexpr QColor kDivider{57, 61, 70};
constexpr QColor kText{202, 207, 216};
constexpr QColor kMutedText{139, 145, 157};
constexpr QColor kPlayhead{244, 89, 93};
constexpr QColor kSnapGuide{94, 214, 194};
constexpr QColor kEditPreview{230, 238, 255, 105};

void paintClipWaveform(QPainter& painter, const QRect& rect,
                       const QVector<WaveformBucketView>& waveform, const bool leaveBottomBadge) {
  if (waveform.isEmpty() || rect.width() < 16 || rect.height() < 24) {
    return;
  }
  const int top_inset = 18;
  const int bottom_inset = leaveBottomBadge ? 16 : 5;
  const QRect band{rect.left() + 6, rect.top() + top_inset, std::max(0, rect.width() - 12),
                   rect.height() - top_inset - bottom_inset};
  if (band.width() < 4 || band.height() < 8) {
    return;
  }
  const int buckets = waveform.size();
  const qreal center = static_cast<qreal>(band.center().y());
  const qreal half = static_cast<qreal>(band.height()) * 0.5;
  painter.setPen(QPen{QColor{236, 240, 247, 170}, 1});
  for (int x = 0; x < band.width(); ++x) {
    const int index = static_cast<int>(static_cast<qint64>(x) * buckets / band.width());
    const auto& bucket = waveform.at(index);
    if (bucket.minimum > bucket.maximum) {
      continue;
    }
    const qreal min_y = center + static_cast<qreal>(bucket.minimum) * half;
    const qreal max_y = center + static_cast<qreal>(bucket.maximum) * half;
    const qreal px = static_cast<qreal>(band.left() + x);
    painter.drawLine(QPointF{px, min_y}, QPointF{px, max_y});
  }
}

QString formatRulerTime(double seconds) {
  const auto total = static_cast<qint64>(std::max(0.0, seconds));
  const auto hours = total / 3600;
  const auto minutes = (total / 60) % 60;
  const auto secs = total % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(secs, 2, 10, QLatin1Char('0'));
}

QColor trackTint(TrackKind kind) {
  switch (kind) {
  case TrackKind::Video:
    return QColor{38, 43, 52};
  case TrackKind::Audio:
    return QColor{35, 47, 45};
  case TrackKind::Caption:
    return QColor{48, 42, 34};
  }
  return kCanvas;
}

} // namespace

TimelineWidget::TimelineWidget(QWidget* parent) : QAbstractScrollArea(parent) {
  setObjectName(QStringLiteral("timelineWidget"));
  setAccessibleName(tr("Timeline"));
  setAccessibleDescription(tr("Track timeline. Use Left and Right Arrow to move the playhead, and "
                              "Control plus the mouse wheel to zoom. Drag clip bodies to move, "
                              "drag their edges to trim, and use Alt plus Left or Right Arrow to "
                              "nudge the active clip by one frame."));
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  viewport()->setObjectName(QStringLiteral("timelineViewport"));
  viewport()->setCursor(Qt::ArrowCursor);

  connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(),
          qOverload<>(&QWidget::update));
  connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
          qOverload<>(&QWidget::update));
  updateScrollBars();
}

void TimelineWidget::setTimeline(qint64 duration, qint64 timeScale,
                                 QVector<TimelineTrackView> tracks,
                                 QVector<TimelineClipView> clips) {
  setTimeline(duration, timeScale, std::move(tracks), std::move(clips), {}, {});
}

void TimelineWidget::setTimeline(qint64 duration, qint64 timeScale,
                                 QVector<TimelineTrackView> tracks, QVector<TimelineClipView> clips,
                                 QVector<TimelineMarkerView> markers,
                                 QVector<TimelineGapView> gaps) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  if (marker_gesture_.pointerDown) {
    cancelMarkerGesture();
  }
  if (transition_gesture_.pointerDown) {
    transition_gesture_ = TransitionGesture{};
  }
  duration_ = std::max<qint64>(0, duration);
  time_scale_ = std::max<qint64>(1, timeScale);
  tracks_ = std::move(tracks);
  clips_ = std::move(clips);
  markers_ = std::move(markers);
  gaps_ = std::move(gaps);
  transitions_.clear();
  selected_transition_index_ = -1;
  const auto clipIds = selectedClipIds();
  active_clip_id_ = clipIds.contains(active_clip_id_) ? active_clip_id_ : clipIds.value(0);
  selection_anchor_id_ =
      clipIds.contains(selection_anchor_id_) ? selection_anchor_id_ : active_clip_id_;
  const auto markerIsSelected = [this](const QString& id) {
    return std::any_of(markers_.cbegin(), markers_.cend(),
                       [&id](const auto& marker) { return marker.id == id && marker.selected; });
  };
  if (!markerIsSelected(active_marker_id_)) {
    active_marker_id_.clear();
    for (const auto& marker : markers_) {
      if (marker.selected) {
        active_marker_id_ = marker.id;
        break;
      }
    }
  }
  const auto gapIsSelected = [this](const QString& key) {
    return std::any_of(gaps_.cbegin(), gaps_.cend(),
                       [&key](const auto& gap) { return gap.key == key && gap.selected; });
  };
  if (!gapIsSelected(active_gap_key_)) {
    active_gap_key_.clear();
    for (const auto& gap : gaps_) {
      if (gap.selected) {
        active_gap_key_ = gap.key;
        break;
      }
    }
  }
  const auto trackExists = [this](const QString& id) {
    return std::any_of(tracks_.cbegin(), tracks_.cend(),
                       [&id](const auto& track) { return track.id == id; });
  };
  if (!trackExists(active_track_id_)) {
    active_track_id_.clear();
  }
  playhead_ = std::clamp(playhead_, qint64{0}, duration_);
  updateScrollBars();
  viewport()->update();
}

void TimelineWidget::setTracks(QVector<TimelineTrackView> tracks) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  tracks_ = std::move(tracks);
  if (!std::any_of(tracks_.cbegin(), tracks_.cend(),
                   [this](const auto& track) { return track.id == active_track_id_; })) {
    active_track_id_.clear();
  }
  updateScrollBars();
  viewport()->update();
}

void TimelineWidget::setClips(QVector<TimelineClipView> clips) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  if (transition_gesture_.pointerDown) {
    transition_gesture_ = TransitionGesture{};
  }
  clips_ = std::move(clips);
  const auto clipIds = selectedClipIds();
  active_clip_id_ = clipIds.contains(active_clip_id_) ? active_clip_id_ : clipIds.value(0);
  selection_anchor_id_ =
      clipIds.contains(selection_anchor_id_) ? selection_anchor_id_ : active_clip_id_;
  clearTransitionSelection();
  viewport()->update();
}

void TimelineWidget::setMarkers(QVector<TimelineMarkerView> markers) {
  if (marker_gesture_.pointerDown) {
    cancelMarkerGesture();
  }
  markers_ = std::move(markers);
  const auto selected =
      std::find_if(markers_.cbegin(), markers_.cend(), [this](const auto& marker) {
        return marker.id == active_marker_id_ && marker.selected;
      });
  if (selected == markers_.cend()) {
    active_marker_id_.clear();
    const auto firstSelected = std::find_if(markers_.cbegin(), markers_.cend(),
                                            [](const auto& marker) { return marker.selected; });
    if (firstSelected != markers_.cend()) {
      active_marker_id_ = firstSelected->id;
    }
  }
  viewport()->update();
}

void TimelineWidget::setGaps(QVector<TimelineGapView> gaps) {
  gaps_ = std::move(gaps);
  const auto selected = std::find_if(gaps_.cbegin(), gaps_.cend(), [this](const auto& gap) {
    return gap.key == active_gap_key_ && gap.selected;
  });
  if (selected == gaps_.cend()) {
    active_gap_key_.clear();
    const auto firstSelected =
        std::find_if(gaps_.cbegin(), gaps_.cend(), [](const auto& gap) { return gap.selected; });
    if (firstSelected != gaps_.cend()) {
      active_gap_key_ = firstSelected->key;
    }
  }
  viewport()->update();
}

void TimelineWidget::setSnapResolver(
    std::function<TimelineSnapResult(const TimelineSnapRequest&)> resolver) {
  snap_resolver_ = std::move(resolver);
}

void TimelineWidget::setTransitions(const QVector<TransitionView>& transitions) {
  transitions_ = transitions;
  viewport()->update();
}

QVector<TransitionView> TimelineWidget::transitions() const noexcept {
  return transitions_;
}

void TimelineWidget::clearTransitionSelection() {
  for (auto& transition : transitions_) {
    transition.selected = false;
  }
  selected_transition_index_ = -1;
  viewport()->update();
}

QStringList TimelineWidget::selectedClipIds() const {
  QStringList result;
  for (const auto& clip : clips_) {
    if (clip.selected) {
      result.append(clip.id);
    }
  }
  return result;
}

void TimelineWidget::setDuration(qint64 duration, qint64 timeScale) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  if (marker_gesture_.pointerDown) {
    cancelMarkerGesture();
  }
  duration_ = std::max<qint64>(0, duration);
  time_scale_ = std::max<qint64>(1, timeScale);
  playhead_ = std::clamp(playhead_, qint64{0}, duration_);
  updateScrollBars();
  viewport()->update();
}

void TimelineWidget::setPlayhead(qint64 position) {
  const auto bounded = std::clamp(position, qint64{0}, duration_);
  if (playhead_ == bounded) {
    return;
  }
  playhead_ = bounded;
  revealPlayhead();
  viewport()->update();
  emit playheadChanged(playhead_);
}

void TimelineWidget::setPixelsPerSecond(double pixelsPerSecond) {
  const auto bounded = std::clamp(pixelsPerSecond, 8.0, 2'400.0);
  if (qFuzzyCompare(pixels_per_second_, bounded)) {
    return;
  }

  const auto centerTime = timeForX(std::max(header_width_, viewport()->width() / 2));
  pixels_per_second_ = bounded;
  updateScrollBars();

  const auto centerPixels = static_cast<int>(std::llround(
      static_cast<double>(centerTime) / static_cast<double>(time_scale_) * pixels_per_second_));
  horizontalScrollBar()->setValue(centerPixels -
                                  std::max(0, viewport()->width() / 2 - header_width_));
  viewport()->update();
  emit zoomChanged(pixels_per_second_);
}

void TimelineWidget::zoomIn() {
  setPixelsPerSecond(pixels_per_second_ * 1.25);
}

void TimelineWidget::zoomOut() {
  setPixelsPerSecond(pixels_per_second_ / 1.25);
}

void TimelineWidget::zoomToFit() {
  const auto available = std::max(1, viewport()->width() - header_width_ - 12);
  const auto seconds = static_cast<double>(std::max<qint64>(duration_, time_scale_)) /
                       static_cast<double>(time_scale_);
  setPixelsPerSecond(static_cast<double>(available) / seconds);
  horizontalScrollBar()->setValue(0);
}

void TimelineWidget::setSnapThresholdPixels(int threshold) {
  snap_threshold_pixels_ = std::clamp(threshold, 0, 64);
}

void TimelineWidget::setFrameRate(quint32 numerator, quint32 denominator) {
  frame_rate_numerator_ = std::max<quint32>(1, numerator);
  frame_rate_denominator_ = std::max<quint32>(1, denominator);
}

void TimelineWidget::setToolMode(ToolMode mode) {
  if (tool_mode_ == mode) {
    return;
  }
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  tool_mode_ = mode;
  updateHoverCursor(viewport()->mapFromGlobal(QCursor::pos()));
  viewport()->update();
}

qint64 TimelineWidget::frameStep() const noexcept {
  return roundedFrameDuration();
}

TimelineWidget::ClipHitRegion
TimelineWidget::clipHitRegionAt(const QPoint& viewportPosition) const {
  const auto index = clipAt(viewportPosition);
  if (index < 0) {
    return ClipHitRegion::None;
  }

  const auto rect = clipRect(clips_.at(index));
  const auto edgeWidth = std::min(trim_handle_pixels_, std::max(1, rect.width() / 3));
  if (viewportPosition.x() <= rect.left() + edgeWidth) {
    return ClipHitRegion::TrimIn;
  }
  if (viewportPosition.x() >= rect.right() - edgeWidth) {
    return ClipHitRegion::TrimOut;
  }
  return ClipHitRegion::Body;
}

void TimelineWidget::nudgeActiveClipByFrames(int frameCount, EditIntent intent) {
  const auto ids = selectedClipIds();
  if (ids.isEmpty() || frameCount == 0) {
    return;
  }
  emit frameNudgeRequested(ids, frameCount, intent);

  // Compatibility preview for old controller integrations. New integrations use
  // frameNudgeRequested so conversion stays exact in the edit model.
  const auto index = activeClipIndex();
  if (index < 0) {
    return;
  }
  const auto& clip = clips_.at(index);
  if (clip.trackIndex < 0 || clip.trackIndex >= tracks_.size() ||
      tracks_.at(clip.trackIndex).locked) {
    return;
  }

  const auto frameDuration = roundedFrameDuration();
  qint64 requestedDelta = 0;
  const auto count = static_cast<qint64>(frameCount);
  if (count > 0) {
    requestedDelta = frameDuration > std::numeric_limits<qint64>::max() / count
                         ? std::numeric_limits<qint64>::max()
                         : frameDuration * count;
  } else {
    const auto magnitude = -count;
    requestedDelta = frameDuration > std::numeric_limits<qint64>::max() / magnitude
                         ? std::numeric_limits<qint64>::min()
                         : -(frameDuration * magnitude);
  }
  const auto latestStart = std::max<qint64>(0, duration_ - clip.duration);
  const auto boundedDelta = requestedDelta > 0 ? std::min(requestedDelta, latestStart - clip.start)
                                               : std::max(requestedDelta, -clip.start);
  const auto targetStart = clip.start + boundedDelta;
  const auto delta = targetStart - clip.start;
  if (delta == 0) {
    return;
  }

  emit clipEditPreview(clip.id, clip.trackIndex, delta, 0, EditMode::Move, intent, false);
  emit clipEditCommitted(clip.id, clip.trackIndex, delta, 0, EditMode::Move, intent, false);
}

void TimelineWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(viewport());
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.fillRect(viewport()->rect(), kCanvas);

  const auto scrollY = verticalScrollBar()->value();
  const auto timelineLeft = header_width_;
  const QRect rulerRect{timelineLeft, 0, viewport()->width() - timelineLeft, ruler_height_};
  painter.fillRect(rulerRect, kRuler);

  const auto firstTrack = std::max(0, scrollY / track_height_);
  const auto lastTrack =
      std::min(static_cast<int>(tracks_.size()) - 1,
               (scrollY + viewport()->height() - ruler_height_) / track_height_ + 1);

  if (!tracks_.isEmpty()) {
    for (int index = firstTrack; index <= lastTrack; ++index) {
      const auto y = ruler_height_ + index * track_height_ - scrollY;
      const QRect body{timelineLeft, y, viewport()->width() - timelineLeft, track_height_};
      painter.fillRect(body, trackTint(tracks_.at(index).kind));
      painter.setPen(kDivider);
      painter.drawLine(body.bottomLeft(), body.bottomRight());
    }
  }

  // Pick a ruler interval that keeps labels legible at every zoom level.
  constexpr double intervals[] = {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0};
  double interval = intervals[std::size(intervals) - 1];
  for (const auto candidate : intervals) {
    if (candidate * pixels_per_second_ >= 72.0) {
      interval = candidate;
      break;
    }
  }
  const auto leftSeconds = static_cast<double>(horizontalScrollBar()->value()) / pixels_per_second_;
  const auto rightSeconds =
      leftSeconds +
      static_cast<double>(std::max(0, viewport()->width() - header_width_)) / pixels_per_second_;
  const auto firstTick = std::floor(leftSeconds / interval) * interval;

  painter.setFont(QFont{font().family(), std::max(8, font().pointSize() - 1)});
  for (auto second = firstTick; second <= rightSeconds + interval; second += interval) {
    const auto time = static_cast<qint64>(std::llround(second * static_cast<double>(time_scale_)));
    const auto x = xForTime(time);
    if (x < header_width_ || x > viewport()->width()) {
      continue;
    }
    painter.setPen(QColor{67, 71, 81});
    painter.drawLine(x, ruler_height_ - 8, x, viewport()->height());
    painter.setPen(kMutedText);
    painter.drawText(QRect{x + 5, 0, 100, ruler_height_ - 5}, Qt::AlignLeft | Qt::AlignVCenter,
                     formatRulerTime(second));
  }

  visible_clip_count_ = 0;
  for (const auto& gap : gaps_) {
    if (gap.trackIndex < firstTrack || gap.trackIndex > lastTrack || gap.trackIndex < 0 ||
        gap.trackIndex >= tracks_.size() || gap.duration <= 0) {
      continue;
    }
    const QRect gapRect{
        xForTime(gap.start), ruler_height_ + gap.trackIndex * track_height_ - scrollY + 5,
        std::max(3, xForTime(gap.start + gap.duration) - xForTime(gap.start)), track_height_ - 10};
    painter.fillRect(gapRect, QColor{16, 18, 22, 150});
    painter.setPen(QPen{gap.selected || gap.key == active_gap_key_ ? QColor{238, 183, 72}
                                                                   : QColor{97, 102, 114},
                        1, Qt::DashLine});
    painter.drawRect(gapRect.adjusted(0, 0, -1, -1));
    if (gapRect.width() > 44) {
      painter.setPen(kMutedText);
      painter.drawText(gapRect, Qt::AlignCenter, tr("Gap"));
    }
  }
  for (const auto& clip : clips_) {
    if (clip.trackIndex < firstTrack || clip.trackIndex > lastTrack || clip.trackIndex < 0 ||
        clip.trackIndex >= tracks_.size()) {
      continue;
    }
    auto rect = clipRect(clip);
    if (!rect.intersects(viewport()->rect().adjusted(header_width_, ruler_height_, 0, 0))) {
      continue;
    }
    ++visible_clip_count_;

    QPainterPath shape;
    shape.addRoundedRect(QRectF{rect}, 4.0, 4.0);
    auto fill = clip.offline ? QColor{104, 69, 72} : clip.color;
    if (tracks_.at(clip.trackIndex).muted) {
      fill = fill.darker(145);
    }
    painter.fillPath(shape, fill);
    painter.setPen(clip.selected ? QColor{236, 242, 255} : fill.lighter(128));
    painter.drawPath(shape);

    painter.save();
    painter.setClipRect(rect.adjusted(7, 3, -5, -3));
    painter.setPen(QColor{244, 246, 250});
    QFont clipFont = font();
    clipFont.setWeight(QFont::DemiBold);
    painter.setFont(clipFont);
    const auto badgeSpace = clip.proxy ? 45 : 8;
    painter.drawText(rect.adjusted(8, 4, -badgeSpace, -4), Qt::AlignLeft | Qt::AlignTop,
                     fontMetrics().elidedText(clip.displayName, Qt::ElideRight,
                                              std::max(0, rect.width() - badgeSpace - 12)));
    paintClipWaveform(painter, rect, clip.waveform, clip.offline);
    if (clip.proxy && rect.width() > 70) {
      painter.setFont(QFont{font().family(), std::max(7, font().pointSize() - 2), QFont::DemiBold});
      painter.setPen(QColor{224, 233, 246});
      painter.drawText(rect.adjusted(0, 4, -6, 0), Qt::AlignTop | Qt::AlignRight, tr("PROXY"));
    }
    if (clip.offline && rect.width() > 80) {
      painter.setFont(font());
      painter.drawText(rect.adjusted(8, 4, -8, -5), Qt::AlignLeft | Qt::AlignBottom,
                       tr("Media offline"));
    }
    painter.restore();

    if (clip.selected || clip.id == active_clip_id_) {
      painter.setPen(QPen{QColor{238, 243, 252, 185}, 2});
      painter.drawLine(rect.left() + 2, rect.top() + 6, rect.left() + 2, rect.bottom() - 6);
      painter.drawLine(rect.right() - 2, rect.top() + 6, rect.right() - 2, rect.bottom() - 6);
    }
  }

  for (const auto& transition : transitions_) {
    if (transition.trackIndex < firstTrack || transition.trackIndex > lastTrack ||
        transition.trackIndex < 0 || transition.trackIndex >= tracks_.size()) {
      continue;
    }
    auto rect = transitionRect(transition);
    if (!rect.intersects(viewport()->rect().adjusted(header_width_, ruler_height_, 0, 0))) {
      continue;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor fill;
    if (transition.kind == QStringLiteral("cross_dissolve")) {
      fill = QColor{180, 120, 220};
    } else if (transition.kind == QStringLiteral("dip_to_black")) {
      fill = QColor{120, 120, 140};
    } else {
      fill = QColor{180, 120, 220};
    }

    if (transition.selected) {
      fill = fill.lighter(130);
    }

    QPainterPath shape;
    shape.addRoundedRect(QRectF{rect}, 4.0, 4.0);
    painter.fillPath(shape, fill);

    if (transition.selected) {
      painter.setPen(QPen{QColor{236, 242, 255}, 2});
    } else {
      painter.setPen(QPen{fill.lighter(120), 1});
    }
    painter.drawPath(shape);

    painter.setClipRect(rect);
    painter.setPen(QColor{244, 246, 250});
    QFont transitionFont = font();
    transitionFont.setWeight(QFont::DemiBold);
    painter.setFont(transitionFont);
    const QString label = transition.kind == QStringLiteral("cross_dissolve")
                              ? QStringLiteral("\u21d4")
                              : transition.kind == QStringLiteral("dip_to_black")
                                    ? QStringLiteral("\u25a0")
                                    : transition.kind;
    painter.drawText(rect, Qt::AlignCenter, label);

    const int handleWidth = transition_handle_pixels_;
    const QRect leftHandle{rect.left(), rect.top() + 4, handleWidth, rect.height() - 8};
    const QRect rightHandle{rect.right() - handleWidth + 1, rect.top() + 4, handleWidth,
                            rect.height() - 8};

    painter.setPen(QColor{202, 207, 216});
    for (int y = leftHandle.top() + 2; y < leftHandle.bottom(); y += 4) {
      painter.drawLine(leftHandle.left() + 2, y, leftHandle.right() - 2, y);
    }
    for (int y = rightHandle.top() + 2; y < rightHandle.bottom(); y += 4) {
      painter.drawLine(rightHandle.left() + 2, y, rightHandle.right() - 2, y);
    }

    painter.restore();
  }

  // Markers are drawn in the ruler and deliberately stay independent of the
  // clip paint order.
  for (const auto& markerView : markers_) {
    const auto x = xForTime(markerView.start);
    if (x < header_width_ - 8 || x > viewport()->width() + 8) {
      continue;
    }
    const auto color = markerView.selected || markerView.id == active_marker_id_
                           ? markerView.color.lighter(130)
                           : markerView.color;
    painter.setPen(QPen{color, 1});
    painter.drawLine(x, 0, x, ruler_height_ - 1);
    QPainterPath pin;
    pin.moveTo(x, 1);
    pin.lineTo(x + 9, 5);
    pin.lineTo(x, 10);
    pin.closeSubpath();
    painter.fillPath(pin, color);
    if (markerView.duration > 0) {
      painter.fillRect(
          QRect{x, 1, std::max(1, xForTime(markerView.start + markerView.duration) - x), 3}, color);
    }
  }

  if (clip_gesture_.dragging && clip_gesture_.clipIndex >= 0 &&
      clip_gesture_.clipIndex < clips_.size()) {
    auto previewClip = clips_.at(clip_gesture_.clipIndex);
    if (clip_gesture_.mode != EditMode::Slip) {
      previewClip.start += clip_gesture_.startDelta;
      previewClip.duration += clip_gesture_.durationDelta;
    }
    if (clip_gesture_.mode == EditMode::Move) {
      previewClip.trackIndex = clip_gesture_.destinationTrackIndex;
    }
    const auto previewRect = clipRect(previewClip);
    if (previewRect.intersects(viewport()->rect())) {
      painter.fillRect(previewRect, kEditPreview);
      painter.setPen(QPen{QColor{238, 243, 252}, 1, Qt::DashLine});
      painter.drawRoundedRect(previewRect, 4, 4);
    }
  }

  // Track headers are painted last so scrolled clips cannot cover them.
  painter.fillRect(QRect{0, 0, header_width_, viewport()->height()}, kHeader);
  painter.fillRect(QRect{0, 0, header_width_, ruler_height_}, QColor{35, 38, 45});
  painter.setPen(kDivider);
  painter.drawLine(header_width_ - 1, 0, header_width_ - 1, viewport()->height());

  for (int index = firstTrack; index <= lastTrack && index >= 0; ++index) {
    const auto& track = tracks_.at(index);
    const auto y = ruler_height_ + index * track_height_ - scrollY;
    const QRect header{0, y, header_width_, track_height_};
    painter.fillRect(header, index % 2 == 0 ? kHeader : QColor{34, 37, 43});
    painter.setPen(kDivider);
    painter.drawLine(header.bottomLeft(), header.bottomRight());
    painter.setPen(track.locked ? kMutedText : kText);
    QFont trackFont = font();
    trackFont.setWeight(QFont::DemiBold);
    painter.setFont(trackFont);
    painter.drawText(
        header.adjusted(12, 7, -62, -25), Qt::AlignLeft | Qt::AlignVCenter,
        painter.fontMetrics().elidedText(track.displayName, Qt::ElideRight, header_width_ - 80));
    painter.setFont(font());
    painter.setPen(kMutedText);
    const auto kindText = track.kind == TrackKind::Video   ? tr("Video")
                          : track.kind == TrackKind::Audio ? tr("Audio")
                                                           : tr("Captions");
    painter.drawText(header.adjusted(12, 27, -65, -5), Qt::AlignLeft | Qt::AlignVCenter, kindText);

    QString states;
    if (track.muted) {
      states += QStringLiteral("M ");
    }
    if (track.soloed) {
      states += QStringLiteral("S ");
    }
    if (track.locked) {
      states += QStringLiteral("L");
    }
    if (!track.visible) {
      states += QStringLiteral(" H");
    }
    if (track.targeted) {
      states += QStringLiteral(" T");
    }
    painter.setPen(QColor{173, 183, 202});
    painter.drawText(header.adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter,
                     states.trimmed());

    const auto controls = QRect{header_width_ - 58, y + 4, 54, track_height_ - 8};
    painter.setPen(QColor{173, 183, 202});
    painter.drawText(
        controls, Qt::AlignCenter,
        QStringLiteral("%1%2%3").arg(track.locked ? QStringLiteral("L") : QStringLiteral("·"),
                                     track.visible ? QStringLiteral("◉") : QStringLiteral("○"),
                                     track.targeted ? QStringLiteral("T") : QStringLiteral("·")));
  }

  if (clip_gesture_.dragging && clip_gesture_.snapped) {
    const auto snapX = xForTime(clip_gesture_.snapTime);
    if (snapX >= header_width_ && snapX <= viewport()->width()) {
      painter.setPen(QPen{kSnapGuide, 1, Qt::DashLine});
      painter.drawLine(snapX, 0, snapX, viewport()->height());
      painter.setPen(kSnapGuide);
      painter.drawText(QRect{snapX + 5, 1, 70, ruler_height_ - 2}, Qt::AlignLeft | Qt::AlignVCenter,
                       tr("Snap"));
    }
  }

  const auto playheadX = xForTime(playhead_);
  if (playheadX >= header_width_ && playheadX <= viewport()->width()) {
    painter.setPen(QPen{kPlayhead, 2});
    painter.drawLine(playheadX, ruler_height_ - 1, playheadX, viewport()->height());
    QPainterPath marker;
    marker.moveTo(playheadX - 6, 0);
    marker.lineTo(playheadX + 6, 0);
    marker.lineTo(playheadX + 4, 11);
    marker.lineTo(playheadX, 15);
    marker.lineTo(playheadX - 4, 11);
    marker.closeSubpath();
    painter.fillPath(marker, kPlayhead);
  }

  if (tracks_.isEmpty()) {
    painter.setPen(kText);
    QFont emptyFont = font();
    emptyFont.setPointSize(emptyFont.pointSize() + 1);
    painter.setFont(emptyFont);
    painter.drawText(QRect{header_width_, ruler_height_, viewport()->width() - header_width_,
                           viewport()->height() - ruler_height_},
                     Qt::AlignCenter, tr("Import media, then drag clips here to start editing"));
  }

  if (hasFocus()) {
    QStyleOptionFocusRect focus;
    focus.initFrom(this);
    focus.rect = viewport()->rect().adjusted(1, 1, -2, -2);
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
  }
}

void TimelineWidget::resizeEvent(QResizeEvent* event) {
  QAbstractScrollArea::resizeEvent(event);
  updateScrollBars();
}

void TimelineWidget::wheelEvent(QWheelEvent* event) {
  if (event->modifiers().testFlag(Qt::ControlModifier)) {
    event->angleDelta().y() > 0 ? zoomIn() : zoomOut();
    event->accept();
    return;
  }
  if (event->modifiers().testFlag(Qt::ShiftModifier)) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().y());
    event->accept();
    return;
  }
  QAbstractScrollArea::wheelEvent(event);
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
  setFocus(Qt::MouseFocusReason);
  const auto position = event->position().toPoint();

  if (event->button() == Qt::LeftButton) {
    const auto transitionIndex = transitionAt(position);
    if (transitionIndex >= 0) {
      clearTransitionSelection();
      transitions_[transitionIndex].selected = true;
      selected_transition_index_ = transitionIndex;
      const auto& transition = transitions_.at(transitionIndex);
      const auto rect = transitionRect(transition);
      const int handleWidth = transition_handle_pixels_;
      const bool onLeftEdge = position.x() <= rect.left() + handleWidth;
      const bool onRightEdge = position.x() >= rect.right() - handleWidth + 1;

      if (onLeftEdge || onRightEdge) {
        transition_gesture_ = TransitionGesture{
            .pointerDown = true,
            .index = transitionIndex,
            .pressPosition = position,
            .pressPointerTime = timeForX(position.x()),
            .originalStart = transition.start,
            .originalDuration = transition.duration,
            .draggingLeft = onLeftEdge,
        };
        if (QApplication::platformName() != QStringLiteral("offscreen")) {
          viewport()->grabMouse();
        }
        event->accept();
        return;
      } else {
        emit transitionActivated(transition.id);
        event->accept();
        return;
      }
    }

    const auto markerIndex = markerAt(position);
    if (markerIndex >= 0) {
      selectMarker(markerIndex);
      marker_gesture_ = {.pointerDown = true,
                         .markerIndex = markerIndex,
                         .pressPosition = position,
                         .originalStart = markers_.at(markerIndex).start,
                         .targetStart = markers_.at(markerIndex).start,
                         .snap = {}};
      event->accept();
      return;
    }
    const auto headerTrack = trackHeaderAt(position);
    if (headerTrack >= 0) {
      const auto& track = tracks_.at(headerTrack);
      active_track_id_ = track.id;
      const auto x = position.x();
      if (x >= header_width_ - 58) {
        if (x < header_width_ - 40) {
          emit trackLockToggled(track.id, !track.locked);
        } else if (x < header_width_ - 20) {
          emit trackVisibilityToggled(track.id, !track.visible);
        } else {
          emit trackTargetToggled(track.id, !track.targeted);
        }
      } else if (event->modifiers().testFlag(Qt::ControlModifier)) {
        emit trackReorderRequested(track.id, std::max(0, headerTrack - 1));
      }
      event->accept();
      return;
    }
    const auto gapIndex = gapAt(position);
    if (gapIndex >= 0) {
      selectGap(gapIndex);
      event->accept();
      return;
    }
  }
  if (event->button() == Qt::LeftButton && event->position().x() >= header_width_ &&
      event->position().y() <= ruler_height_) {
    dragging_playhead_ = true;
    movePlayheadFromPointer(static_cast<int>(event->position().x()), true);
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    const auto clipIndex = clipAt(position);
    if (clipIndex >= 0) {
      selectClip(clipIndex, event->modifiers());
      emit clipActivated(active_clip_id_);
      const auto hitRegion = clipHitRegionAt(position);
      const auto trackIndex = clips_.at(clipIndex).trackIndex;
      const auto requiresEdge = tool_mode_ == ToolMode::RippleTrim ||
                                tool_mode_ == ToolMode::OverwriteTrim ||
                                tool_mode_ == ToolMode::Roll;
      const auto requiresBody = tool_mode_ == ToolMode::Slip || tool_mode_ == ToolMode::Slide;
      if (trackIndex >= 0 && trackIndex < tracks_.size() && !tracks_.at(trackIndex).locked &&
          (!requiresEdge ||
           (hitRegion == ClipHitRegion::TrimIn || hitRegion == ClipHitRegion::TrimOut)) &&
          (!requiresBody || hitRegion == ClipHitRegion::Body)) {
        beginClipGesture(clipIndex, position, hitRegion);
      }
      viewport()->update();
      event->accept();
      return;
    }
  }
  QAbstractScrollArea::mousePressEvent(event);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_playhead_) {
    movePlayheadFromPointer(static_cast<int>(event->position().x()), true);
    event->accept();
    return;
  }
  if (marker_gesture_.pointerDown) {
    if (!marker_gesture_.dragging &&
        (event->position().toPoint() - marker_gesture_.pressPosition).manhattanLength() >=
            QApplication::startDragDistance()) {
      marker_gesture_.dragging = true;
    }
    if (marker_gesture_.dragging && marker_gesture_.markerIndex >= 0 &&
        marker_gesture_.markerIndex < markers_.size()) {
      auto target = timeForX(static_cast<int>(event->position().x()));
      marker_gesture_.snap = resolveSnap(target, {}, true, event->modifiers(),
                                         markers_.at(marker_gesture_.markerIndex).id);
      marker_gesture_.targetStart = marker_gesture_.snap.time;
      emit markerMovePreview(markers_.at(marker_gesture_.markerIndex).id,
                             marker_gesture_.targetStart, marker_gesture_.snap);
      viewport()->update();
    }
    event->accept();
    return;
  }
  if (transition_gesture_.pointerDown && transition_gesture_.index >= 0 &&
      transition_gesture_.index < transitions_.size()) {
    const auto position = event->position().toPoint();
    if (!transition_gesture_.dragging &&
        (position - transition_gesture_.pressPosition).manhattanLength() >=
            QApplication::startDragDistance()) {
      transition_gesture_.dragging = true;
    }
    if (!transition_gesture_.dragging) {
      event->accept();
      return;
    }

    const auto currentPointerTime = timeForX(position.x());
    const auto delta = currentPointerTime - transition_gesture_.pressPointerTime;
    const auto& original = transitions_.at(transition_gesture_.index);
    qint64 maximumDuration = std::numeric_limits<qint64>::max();
    for (const auto& clip : clips_) {
      if (clip.trackIndex != original.trackIndex) {
        continue;
      }
      if (clip.id == original.outgoingClipId || clip.id == original.incomingClipId) {
        maximumDuration = std::min(maximumDuration, std::max<qint64>(1, clip.duration));
      }
    }
    if (maximumDuration == std::numeric_limits<qint64>::max()) {
      maximumDuration = std::max<qint64>(1, original.duration);
    }

    auto& transition = transitions_[transition_gesture_.index];
    const auto originalEnd = transition_gesture_.originalStart +
                             transition_gesture_.originalDuration;
    qint64 newStart = transition_gesture_.originalStart;
    qint64 newDuration = transition_gesture_.originalDuration;
    if (transition_gesture_.draggingLeft) {
      const auto minimumDuration = std::max<qint64>(1, std::min(maximumDuration,
                                                                transition_gesture_.originalDuration));
      const auto maximumStart = originalEnd - minimumDuration;
      const auto minimumStart = std::max<qint64>(0, originalEnd - maximumDuration);
      newStart = std::clamp(transition_gesture_.originalStart + delta, minimumStart,
                            maximumStart);
      newDuration = originalEnd - newStart;
    } else {
      newDuration = std::clamp(transition_gesture_.originalDuration + delta, qint64{1},
                               maximumDuration);
    }

    transition.start = newStart;
    transition.duration = newDuration;
    viewport()->update();
    event->accept();
    return;
  }
  if (clip_gesture_.pointerDown) {
    updateClipGesture(event->position().toPoint(), event->modifiers(), true);
    event->accept();
    return;
  }
  updateHoverCursor(event->position().toPoint());
  QAbstractScrollArea::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (dragging_playhead_ && event->button() == Qt::LeftButton) {
    dragging_playhead_ = false;
    movePlayheadFromPointer(static_cast<int>(event->position().x()), true);
    event->accept();
    return;
  }
  if (marker_gesture_.pointerDown && event->button() == Qt::LeftButton) {
    const auto gesture = marker_gesture_;
    marker_gesture_ = MarkerGesture{};
    if (gesture.dragging && gesture.markerIndex >= 0 && gesture.markerIndex < markers_.size()) {
      emit markerMoveCommitted(markers_.at(gesture.markerIndex).id, gesture.targetStart,
                               gesture.snap);
    }
    viewport()->update();
    event->accept();
    return;
  }
  if (clip_gesture_.pointerDown && event->button() == Qt::LeftButton) {
    finishClipGesture(event->position().toPoint(), event->modifiers());
    updateHoverCursor(event->position().toPoint());
    event->accept();
    return;
  }
  if (transition_gesture_.pointerDown && event->button() == Qt::LeftButton) {
    const auto gesture = transition_gesture_;
    if (gesture.dragging && gesture.index >= 0 && gesture.index < transitions_.size()) {
      const auto& transition = transitions_.at(gesture.index);
      emit transitionDurationEdited(transition.id, transition.duration);
    }
    transition_gesture_ = TransitionGesture{};
    if (QWidget::mouseGrabber() == viewport()) {
      viewport()->releaseMouse();
    }
    updateHoverCursor(event->position().toPoint());
    event->accept();
    return;
  }
  QAbstractScrollArea::mouseReleaseEvent(event);
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && event->position().y() <= ruler_height_ &&
      event->position().x() >= header_width_) {
    const auto snap = resolveSnap(timeForX(static_cast<int>(event->position().x())), {}, true,
                                  event->modifiers());
    emit markerAddRequested(snap.time);
    event->accept();
    return;
  }
  const auto index = clipAt(event->position().toPoint());
  if (event->button() == Qt::LeftButton && index >= 0) {
    emit clipActivated(clips_.at(index).id);
    event->accept();
    return;
  }
  QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
  const auto addTrackMenuItems = [this](QMenu& menu) {
    auto* addVideo = menu.addAction(tr("Add Video Track"));
    auto* addAudio = menu.addAction(tr("Add Audio Track"));
    return std::make_pair(addVideo, addAudio);
  };
  const auto addInsertAtPlayhead = [this](QMenu& menu) {
    auto* insertAtPlayhead = menu.addAction(tr("Insert at playhead"));
    insertAtPlayhead->setToolTip(tr("Insert the loaded or selected source at the playhead"));
    return insertAtPlayhead;
  };
  const auto wireTrackMenuActions = [this](const QAction* addVideo, const QAction* addAudio,
                                            const QAction* insertAtPlayhead) {
    connect(addVideo, &QAction::triggered, this,
            [this] { emit trackAddRequested(TrackKind::Video); });
    connect(addAudio, &QAction::triggered, this,
            [this] { emit trackAddRequested(TrackKind::Audio); });
    connect(insertAtPlayhead, &QAction::triggered, this, [this] { emit insertAtPlayheadRequested(); });
  };
  const auto gapHasLaterClip = [this](const TimelineGapView& gap) {
    const qint64 gapEnd = gap.start + gap.duration;
    return std::any_of(clips_.cbegin(), clips_.cend(), [&gap, gapEnd](const TimelineClipView& clip) {
      return clip.trackIndex == gap.trackIndex && clip.start >= gapEnd;
    });
  };

  const auto transitionIndex = transitionAt(event->pos());
  if (transitionIndex >= 0) {
    const auto transition = transitions_.at(transitionIndex);
    QMenu menu(this);
    auto* remove = menu.addAction(tr("Remove Transition"));
    auto* crossDissolve = menu.addAction(tr("Change Preset → Cross Dissolve"));
    auto* dipToBlack = menu.addAction(tr("Change Preset → Dip to Black"));
    remove->setToolTip(tr("Remove the selected transition"));
    crossDissolve->setToolTip(tr("Set transition preset to cross dissolve"));
    dipToBlack->setToolTip(tr("Set transition preset to dip to black"));
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == remove) {
      emit transitionRemoved(transition.id);
    } else if (chosen == crossDissolve) {
      emit transitionPresetChanged(transition.id, QStringLiteral("cross_dissolve"));
    } else if (chosen == dipToBlack) {
      emit transitionPresetChanged(transition.id, QStringLiteral("dip_to_black"));
    }
    event->accept();
    return;
  }
  const auto markerIndex = markerAt(event->pos());
  if (markerIndex >= 0) {
    selectMarker(markerIndex);
    const auto marker = markers_.at(markerIndex);
    QMenu menu(this);
    auto* rename = menu.addAction(tr("Rename Marker…"));
    auto* remove = menu.addAction(tr("Remove Marker"));
    rename->setToolTip(tr("Rename the selected marker"));
    remove->setToolTip(tr("Remove the selected marker"));
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == rename) {
      bool accepted = false;
      const auto name = QInputDialog::getText(this, tr("Rename Marker"), tr("Name:"),
                                              QLineEdit::Normal, marker.displayName, &accepted);
      if (accepted && !name.trimmed().isEmpty()) {
        emit markerRenameRequested(marker.id, name.trimmed());
      }
    } else if (chosen == remove) {
      emit markerRemoveRequested(marker.id);
    }
    event->accept();
    return;
  }
  const auto gapIndex = gapAt(event->pos());
  if (gapIndex >= 0) {
    selectGap(gapIndex);
    const auto& gap = gaps_.at(gapIndex);
    QMenu menu(this);
    auto [addVideo, addAudio] = addTrackMenuItems(menu);
    menu.addSeparator();
    auto* insertAtPlayhead = addInsertAtPlayhead(menu);
    auto* close = menu.addAction(tr("Close Gap"));
    close->setToolTip(tr("Ripple material after this gap left"));
    if (!gapHasLaterClip(gap)) {
      close->setEnabled(false);
      close->setToolTip(tr("Nothing after this gap to ripple."));
    }
    wireTrackMenuActions(addVideo, addAudio, insertAtPlayhead);
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == close && close->isEnabled()) {
      emit closeGapRequested(gap.key);
    }
    event->accept();
    return;
  }
  const auto headerTrack = trackHeaderAt(event->pos());
  if (headerTrack >= 0) {
    const auto track = tracks_.at(headerTrack);
    QMenu menu(this);
    auto [addVideo, addAudio] = addTrackMenuItems(menu);
    auto* insertAtPlayhead = addInsertAtPlayhead(menu);
    menu.addSeparator();
    auto* rename = menu.addAction(tr("Rename Track…"));
    auto* moveUp = menu.addAction(tr("Move Track Up"));
    auto* moveDown = menu.addAction(tr("Move Track Down"));
    auto* lock = menu.addAction(track.locked ? tr("Unlock Track") : tr("Lock Track"));
    auto* visible = menu.addAction(track.visible ? tr("Hide Track") : tr("Show Track"));
    auto* target = menu.addAction(track.targeted ? tr("Untarget Track") : tr("Target Track"));
    menu.addSeparator();
    auto* remove = menu.addAction(tr("Remove Track"));
    wireTrackMenuActions(addVideo, addAudio, insertAtPlayhead);
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == rename) {
      bool accepted = false;
      const auto name = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"),
                                              QLineEdit::Normal, track.displayName, &accepted);
      if (accepted && !name.trimmed().isEmpty()) {
        emit trackRenameRequested(track.id, name.trimmed());
      }
    } else if (chosen == moveUp) {
      emit trackReorderRequested(track.id, std::max(0, headerTrack - 1));
    } else if (chosen == moveDown) {
      emit trackReorderRequested(track.id,
                                 std::min(static_cast<int>(tracks_.size()) - 1, headerTrack + 1));
    } else if (chosen == lock) {
      emit trackLockToggled(track.id, !track.locked);
    } else if (chosen == visible) {
      emit trackVisibilityToggled(track.id, !track.visible);
    } else if (chosen == target) {
      emit trackTargetToggled(track.id, !track.targeted);
    } else if (chosen == remove) {
      emit trackRemoveRequested(track.id);
    }
    event->accept();
    return;
  }
  const auto index = clipAt(event->pos());
  if (index >= 0) {
    emit clipContextMenuRequested(clips_.at(index).id, event->globalPos());
    event->accept();
    return;
  }
  QMenu menu(this);
  auto [addVideo, addAudio] = addTrackMenuItems(menu);
  auto* insertAtPlayhead = addInsertAtPlayhead(menu);
  wireTrackMenuActions(addVideo, addAudio, insertAtPlayhead);
  menu.exec(event->globalPos());
  event->accept();
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && marker_gesture_.pointerDown) {
    cancelMarkerGesture();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape && clip_gesture_.pointerDown) {
    cancelClipGesture();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape && transition_gesture_.pointerDown) {
    transition_gesture_ = TransitionGesture{};
    if (QWidget::mouseGrabber() == viewport()) {
      viewport()->releaseMouse();
    }
    event->accept();
    return;
  }

  if (event->modifiers().testFlag(Qt::AltModifier) &&
      (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
    const auto amount = event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
    const auto direction = event->key() == Qt::Key_Left ? -amount : amount;
    const auto intent =
        event->modifiers().testFlag(Qt::ControlModifier) ? EditIntent::Ripple : EditIntent::Normal;
    nudgeActiveClipByFrames(direction, intent);
    event->accept();
    return;
  }

  if (event->key() == Qt::Key_M) {
    emit markerAddRequested(playhead_);
    event->accept();
    return;
  }
  if ((event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) &&
      !active_gap_key_.isEmpty()) {
    emit closeGapRequested(active_gap_key_);
    event->accept();
    return;
  }
  if ((event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) &&
      selected_transition_index_ >= 0 &&
      selected_transition_index_ < transitions_.size()) {
    emit transitionRemoved(transitions_.at(selected_transition_index_).id);
    selected_transition_index_ = -1;
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Insert && !event->modifiers().testFlag(Qt::ControlModifier)) {
    const auto kind =
        event->modifiers().testFlag(Qt::ShiftModifier) ? TrackKind::Audio : TrackKind::Video;
    emit trackAddRequested(kind);
    event->accept();
    return;
  }
  if (!active_track_id_.isEmpty()) {
    int activeTrack = -1;
    for (int index = 0; index < tracks_.size(); ++index) {
      if (tracks_.at(index).id == active_track_id_) {
        activeTrack = index;
        break;
      }
    }
    if (activeTrack >= 0 && event->modifiers().testFlag(Qt::ControlModifier) &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
      const auto destination = std::clamp(activeTrack + (event->key() == Qt::Key_Up ? -1 : 1), 0,
                                          static_cast<int>(tracks_.size()) - 1);
      if (destination != activeTrack) {
        emit trackReorderRequested(active_track_id_, destination);
      }
      event->accept();
      return;
    }
    if (activeTrack >= 0 && event->key() == Qt::Key_L) {
      emit trackLockToggled(active_track_id_, !tracks_.at(activeTrack).locked);
      event->accept();
      return;
    }
    if (activeTrack >= 0 && event->key() == Qt::Key_V) {
      emit trackVisibilityToggled(active_track_id_, !tracks_.at(activeTrack).visible);
      event->accept();
      return;
    }
    if (activeTrack >= 0 && event->key() == Qt::Key_T) {
      emit trackTargetToggled(active_track_id_, !tracks_.at(activeTrack).targeted);
      event->accept();
      return;
    }
  }

  const auto frame = roundedFrameDuration();
  qint64 target = playhead_;
  switch (event->key()) {
  case Qt::Key_Left:
    target -= event->modifiers().testFlag(Qt::ShiftModifier) ? frame * 10 : frame;
    break;
  case Qt::Key_Right:
    target += event->modifiers().testFlag(Qt::ShiftModifier) ? frame * 10 : frame;
    break;
  case Qt::Key_Home:
    target = 0;
    break;
  case Qt::Key_End:
    target = duration_;
    break;
  default:
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }
  setPlayhead(target);
  emit seekRequested(playhead_);
  event->accept();
}

int TimelineWidget::contentWidth() const {
  const auto seconds = static_cast<double>(duration_) / static_cast<double>(time_scale_);
  return header_width_ + static_cast<int>(std::ceil(seconds * pixels_per_second_)) + 24;
}

int TimelineWidget::contentHeight() const {
  return ruler_height_ + static_cast<int>(tracks_.size()) * track_height_;
}

int TimelineWidget::xForTime(qint64 time) const {
  const auto pixels =
      static_cast<double>(time) / static_cast<double>(time_scale_) * pixels_per_second_;
  return header_width_ + static_cast<int>(std::llround(pixels)) - horizontalScrollBar()->value();
}

qint64 TimelineWidget::timeForX(int viewportX) const {
  const auto pixels = std::max(0, viewportX - header_width_ + horizontalScrollBar()->value());
  const auto result = static_cast<qint64>(std::llround(
      static_cast<double>(pixels) / pixels_per_second_ * static_cast<double>(time_scale_)));
  return std::clamp(result, qint64{0}, duration_);
}

QRect TimelineWidget::clipRect(const TimelineClipView& clip) const {
  const auto x = xForTime(clip.start);
  const auto endX = xForTime(clip.start + std::max<qint64>(0, clip.duration));
  const auto y = ruler_height_ + clip.trackIndex * track_height_ - verticalScrollBar()->value() + 5;
  return QRect{x, y, std::max(3, endX - x), track_height_ - 10};
}

QRect TimelineWidget::transitionRect(const TransitionView& transition) const {
  const auto x = xForTime(transition.start);
  const auto width = std::max(8, xForTime(transition.start + transition.duration) - x);
  const auto y = ruler_height_ + transition.trackIndex * track_height_ -
                 verticalScrollBar()->value() + 5;
  return QRect{x, y, width, track_height_ - 10};
}

int TimelineWidget::clipAt(const QPoint& position) const {
  for (int index = static_cast<int>(clips_.size()) - 1; index >= 0; --index) {
    if (clipRect(clips_.at(index)).contains(position)) {
      return index;
    }
  }
  return -1;
}

int TimelineWidget::transitionAt(const QPoint& position) const {
  for (int index = static_cast<int>(transitions_.size()) - 1; index >= 0; --index) {
    if (transitionRect(transitions_.at(index)).contains(position)) {
      return index;
    }
  }
  return -1;
}

int TimelineWidget::activeClipIndex() const {
  if (!active_clip_id_.isEmpty()) {
    for (int index = 0; index < clips_.size(); ++index) {
      if (clips_.at(index).id == active_clip_id_) {
        return index;
      }
    }
  }
  for (int index = 0; index < clips_.size(); ++index) {
    if (clips_.at(index).selected) {
      return index;
    }
  }
  return -1;
}

int TimelineWidget::markerAt(const QPoint& position) const {
  if (position.y() > ruler_height_ + 5) {
    return -1;
  }
  for (int index = static_cast<int>(markers_.size()) - 1; index >= 0; --index) {
    if (std::abs(xForTime(markers_.at(index).start) - position.x()) <= 8) {
      return index;
    }
  }
  return -1;
}

int TimelineWidget::gapAt(const QPoint& position) const {
  for (int index = static_cast<int>(gaps_.size()) - 1; index >= 0; --index) {
    const auto& gap = gaps_.at(index);
    if (gap.trackIndex >= 0 && gap.trackIndex < tracks_.size()) {
      const QRect rect{xForTime(gap.start),
                       ruler_height_ + gap.trackIndex * track_height_ -
                           verticalScrollBar()->value() + 5,
                       std::max(3, xForTime(gap.start + gap.duration) - xForTime(gap.start)),
                       track_height_ - 10};
      if (rect.contains(position)) {
        return index;
      }
    }
  }
  return -1;
}

int TimelineWidget::trackHeaderAt(const QPoint& position) const {
  if (position.x() >= header_width_ || position.y() < ruler_height_) {
    return -1;
  }
  return trackAtY(position.y());
}

TimelineSnapResult TimelineWidget::resolveSnap(qint64 proposedTime, const QStringList& exclusions,
                                               bool forMarker, Qt::KeyboardModifiers modifiers,
                                               const QString& excludedMarkerId) const {
  if (modifiers.testFlag(Qt::ShiftModifier) || snap_threshold_pixels_ <= 0 || !snap_resolver_) {
    return {.time = proposedTime, .kind = TimelineSnapKind::None, .label = {}};
  }
  auto result = snap_resolver_({.proposedTime = proposedTime,
                                .thresholdPixels = snap_threshold_pixels_,
                                .excludedClipIds = exclusions,
                                .excludedMarkerId = excludedMarkerId,
                                .forMarker = forMarker});
  if (result.time < 0 || result.time > duration_) {
    return {.time = proposedTime, .kind = TimelineSnapKind::None, .label = {}};
  }
  return result;
}

TimelineWidget::EditMode TimelineWidget::gestureMode(ClipHitRegion hit) const noexcept {
  if (tool_mode_ == ToolMode::Roll) {
    return EditMode::Roll;
  }
  if (tool_mode_ == ToolMode::Slip) {
    return EditMode::Slip;
  }
  if (tool_mode_ == ToolMode::Slide) {
    return EditMode::Slide;
  }
  if (tool_mode_ == ToolMode::RippleTrim || tool_mode_ == ToolMode::OverwriteTrim) {
    return hit == ClipHitRegion::TrimIn ? EditMode::TrimIn : EditMode::TrimOut;
  }
  return hit == ClipHitRegion::TrimIn    ? EditMode::TrimIn
         : hit == ClipHitRegion::TrimOut ? EditMode::TrimOut
                                         : EditMode::Move;
}

void TimelineWidget::clearClipSelection() {
  for (auto& clip : clips_) {
    clip.selected = false;
  }
}

void TimelineWidget::selectClip(int clipIndex, Qt::KeyboardModifiers modifiers) {
  if (clipIndex < 0 || clipIndex >= clips_.size()) {
    return;
  }
  clearTransitionSelection();
  const auto id = clips_.at(clipIndex).id;
  if (modifiers.testFlag(Qt::ShiftModifier) && !selection_anchor_id_.isEmpty()) {
    int anchor = -1;
    for (int index = 0; index < clips_.size(); ++index) {
      if (clips_.at(index).id == selection_anchor_id_) {
        anchor = index;
        break;
      }
    }
    if (anchor >= 0) {
      const auto first = std::min(anchor, clipIndex);
      const auto last = std::max(anchor, clipIndex);
      clearClipSelection();
      for (int index = first; index <= last; ++index) {
        clips_[index].selected = true;
      }
    } else {
      clearClipSelection();
      clips_[clipIndex].selected = true;
      selection_anchor_id_ = id;
    }
  } else if (modifiers.testFlag(Qt::ControlModifier)) {
    clips_[clipIndex].selected = !clips_.at(clipIndex).selected;
    selection_anchor_id_ = id;
  } else {
    clearClipSelection();
    clips_[clipIndex].selected = true;
    selection_anchor_id_ = id;
  }
  active_clip_id_ = clips_.at(clipIndex).selected ? id : selectedClipIds().value(0);
  for (auto& marker : markers_) {
    marker.selected = false;
  }
  for (auto& gap : gaps_) {
    gap.selected = false;
  }
  active_marker_id_.clear();
  active_gap_key_.clear();
  emit clipSelectionChanged(selectedClipIds(), active_clip_id_);
}

void TimelineWidget::selectMarker(int markerIndex) {
  if (markerIndex < 0 || markerIndex >= markers_.size()) {
    return;
  }
  clearTransitionSelection();
  clearClipSelection();
  for (auto& marker : markers_) {
    marker.selected = false;
  }
  markers_[markerIndex].selected = true;
  for (auto& gap : gaps_) {
    gap.selected = false;
  }
  active_marker_id_ = markers_.at(markerIndex).id;
  active_gap_key_.clear();
  active_clip_id_.clear();
  emit markerSelectionChanged(active_marker_id_);
  emit clipSelectionChanged({}, {});
}

void TimelineWidget::selectGap(int gapIndex) {
  if (gapIndex < 0 || gapIndex >= gaps_.size()) {
    return;
  }
  clearTransitionSelection();
  clearClipSelection();
  for (auto& gap : gaps_) {
    gap.selected = false;
  }
  gaps_[gapIndex].selected = true;
  for (auto& marker : markers_) {
    marker.selected = false;
  }
  active_gap_key_ = gaps_.at(gapIndex).key;
  active_marker_id_.clear();
  active_clip_id_.clear();
  emit gapSelectionChanged(active_gap_key_);
  emit clipSelectionChanged({}, {});
}

int TimelineWidget::trackAtY(int viewportY) const {
  if (tracks_.isEmpty()) {
    return -1;
  }
  const auto contentY = viewportY - ruler_height_ + verticalScrollBar()->value();
  const auto index = contentY < 0 ? 0 : contentY / track_height_;
  return std::clamp(index, 0, static_cast<int>(tracks_.size()) - 1);
}

TimelineWidget::EditIntent
TimelineWidget::editIntent(Qt::KeyboardModifiers modifiers) const noexcept {
  if (tool_mode_ == ToolMode::RippleTrim) {
    return EditIntent::Ripple;
  }
  if (tool_mode_ == ToolMode::OverwriteTrim) {
    return EditIntent::Overwrite;
  }
  if (modifiers.testFlag(Qt::ControlModifier)) {
    return EditIntent::Ripple;
  }
  if (modifiers.testFlag(Qt::AltModifier)) {
    return EditIntent::Overwrite;
  }
  return EditIntent::Normal;
}

qint64 TimelineWidget::roundedFrameDuration() const noexcept {
  const auto numerator = static_cast<quint64>(std::max<quint32>(1, frame_rate_numerator_));
  const auto denominator = static_cast<quint64>(std::max<quint32>(1, frame_rate_denominator_));
  const auto scale = static_cast<quint64>(std::max<qint64>(1, time_scale_));
  const auto whole = scale / numerator;
  const auto remainder = scale % numerator;
  const auto limit = static_cast<quint64>(std::numeric_limits<qint64>::max());
  if (whole > limit / denominator) {
    return std::numeric_limits<qint64>::max();
  }
  const auto integral = whole * denominator;
  const auto fractional = (remainder * denominator + numerator / 2) / numerator;
  if (integral > limit - fractional) {
    return std::numeric_limits<qint64>::max();
  }
  return static_cast<qint64>(std::max<quint64>(1, integral + fractional));
}

void TimelineWidget::beginClipGesture(int clipIndex, const QPoint& position,
                                      ClipHitRegion hitRegion) {
  if (clipIndex < 0 || clipIndex >= clips_.size() || hitRegion == ClipHitRegion::None) {
    return;
  }
  const auto& clip = clips_.at(clipIndex);
  clip_gesture_ = ClipGesture{};
  clip_gesture_.pointerDown = true;
  clip_gesture_.clipIndex = clipIndex;
  clip_gesture_.pressPosition = position;
  clip_gesture_.pressPointerTime = timeForX(position.x());
  clip_gesture_.originalStart = clip.start;
  clip_gesture_.originalDuration = clip.duration;
  clip_gesture_.originalTrackIndex = clip.trackIndex;
  clip_gesture_.destinationTrackIndex = clip.trackIndex;
  clip_gesture_.mode = gestureMode(hitRegion);
  clip_gesture_.clipIds = selectedClipIds();
  if (clip_gesture_.clipIds.isEmpty()) {
    clip_gesture_.clipIds.append(clip.id);
  }
  if (clip_gesture_.mode == EditMode::Roll) {
    const auto minimumDuration = [](qint64 duration, qint64 frameDuration) {
      return std::max<qint64>(1, std::min(duration, frameDuration));
    };
    const auto clipMinimum = minimumDuration(clip.duration, roundedFrameDuration());
    const auto clipEnd = clip.start + clip.duration;
    int following = -1;
    int preceding = -1;
    for (int index = 0; index < clips_.size(); ++index) {
      const auto& candidate = clips_.at(index);
      if (index == clipIndex || candidate.trackIndex != clip.trackIndex) {
        continue;
      }
      if (candidate.start == clipEnd) {
        following = index;
      } else if (candidate.start + candidate.duration == clip.start) {
        preceding = index;
      }
    }
    // Match the model/controller's deterministic partner preference: roll the
    // outgoing edge when both neighbours are contiguous, otherwise use the
    // incoming edge.
    if (following >= 0) {
      const auto followingMinimum =
          minimumDuration(clips_.at(following).duration, roundedFrameDuration());
      clip_gesture_.rollMinimumDelta = -(clip.duration - clipMinimum);
      clip_gesture_.rollMaximumDelta = clips_.at(following).duration - followingMinimum;
    } else if (preceding >= 0) {
      const auto precedingMinimum =
          minimumDuration(clips_.at(preceding).duration, roundedFrameDuration());
      clip_gesture_.rollCutAtStart = true;
      clip_gesture_.rollMinimumDelta = -(clips_.at(preceding).duration - precedingMinimum);
      clip_gesture_.rollMaximumDelta = clip.duration - clipMinimum;
    } else {
      // A roll needs an adjacent cut. Avoid issuing a gesture that the model
      // will necessarily reject.
      clip_gesture_ = ClipGesture{};
      return;
    }
  }
  if (QApplication::platformName() != QStringLiteral("offscreen")) {
    viewport()->grabMouse();
  }
}

void TimelineWidget::updateClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers,
                                       bool allowAutoScroll) {
  if (!clip_gesture_.pointerDown || clip_gesture_.clipIndex < 0 ||
      clip_gesture_.clipIndex >= clips_.size()) {
    return;
  }

  if (!clip_gesture_.dragging) {
    if ((position - clip_gesture_.pressPosition).manhattanLength() <
        QApplication::startDragDistance()) {
      return;
    }
    clip_gesture_.dragging = true;
  }

  if (allowAutoScroll) {
    autoScrollForDrag(position);
  }

  const auto currentPointerTime = timeForX(position.x());
  const auto originalEnd = clip_gesture_.originalStart + clip_gesture_.originalDuration;
  const auto minimumDuration =
      std::max<qint64>(1, std::min(clip_gesture_.originalDuration, roundedFrameDuration()));
  qint64 targetStart = clip_gesture_.originalStart;
  qint64 targetDuration = clip_gesture_.originalDuration;

  switch (clip_gesture_.mode) {
  case EditMode::Move:
  case EditMode::Slide: {
    const auto grabOffset = clip_gesture_.pressPointerTime - clip_gesture_.originalStart;
    const auto latestStart = std::max<qint64>(0, duration_ - clip_gesture_.originalDuration);
    targetStart = std::clamp(currentPointerTime - grabOffset, qint64{0}, latestStart);
    clip_gesture_.destinationTrackIndex = clip_gesture_.mode == EditMode::Move
                                              ? trackAtY(position.y())
                                              : clip_gesture_.originalTrackIndex;
    break;
  }
  case EditMode::Slip: {
    // Slip changes source time only. Its timeline placement and track must stay
    // fixed; the model validates the resulting source range.
    targetStart =
        clip_gesture_.originalStart + (currentPointerTime - clip_gesture_.pressPointerTime);
    clip_gesture_.destinationTrackIndex = clip_gesture_.originalTrackIndex;
    break;
  }
  case EditMode::TrimIn: {
    const auto grabOffset = clip_gesture_.pressPointerTime - clip_gesture_.originalStart;
    targetStart =
        std::clamp(currentPointerTime - grabOffset, qint64{0},
                   std::max<qint64>(clip_gesture_.originalStart, originalEnd - minimumDuration));
    targetDuration = originalEnd - targetStart;
    clip_gesture_.destinationTrackIndex = clip_gesture_.originalTrackIndex;
    break;
  }
  case EditMode::TrimOut: {
    const auto grabOffset = clip_gesture_.pressPointerTime - originalEnd;
    const auto targetEnd = std::clamp(currentPointerTime - grabOffset,
                                      clip_gesture_.originalStart + minimumDuration, duration_);
    targetDuration = targetEnd - clip_gesture_.originalStart;
    clip_gesture_.destinationTrackIndex = clip_gesture_.originalTrackIndex;
    break;
  }
  case EditMode::Roll: {
    const auto originalCut =
        clip_gesture_.rollCutAtStart ? clip_gesture_.originalStart : originalEnd;
    const auto grabOffset = clip_gesture_.pressPointerTime - originalCut;
    const auto delta = std::clamp(currentPointerTime - grabOffset - originalCut,
                                  clip_gesture_.rollMinimumDelta, clip_gesture_.rollMaximumDelta);
    if (clip_gesture_.rollCutAtStart) {
      targetStart = clip_gesture_.originalStart + delta;
      targetDuration = clip_gesture_.originalDuration - delta;
    } else {
      targetDuration = clip_gesture_.originalDuration + delta;
    }
    clip_gesture_.destinationTrackIndex = clip_gesture_.originalTrackIndex;
    break;
  }
  }

  const auto anchor =
      clip_gesture_.mode == EditMode::Roll
          ? (clip_gesture_.rollCutAtStart ? targetStart : targetStart + targetDuration)
      : clip_gesture_.mode == EditMode::TrimOut ? targetStart + targetDuration
                                                : targetStart;
  clip_gesture_.snap =
      clip_gesture_.mode == EditMode::Slip
          ? TimelineSnapResult{.time = anchor, .kind = TimelineSnapKind::None, .label = {}}
          : resolveSnap(anchor, clip_gesture_.clipIds, false, modifiers);
  clip_gesture_.snapped = clip_gesture_.snap.snapped();
  clip_gesture_.snapTime = clip_gesture_.snap.time;
  if (clip_gesture_.snapped) {
    if (clip_gesture_.mode == EditMode::Roll) {
      const auto originalCut =
          clip_gesture_.rollCutAtStart ? clip_gesture_.originalStart : originalEnd;
      const auto delta = std::clamp(clip_gesture_.snap.time - originalCut,
                                    clip_gesture_.rollMinimumDelta, clip_gesture_.rollMaximumDelta);
      if (originalCut + delta != clip_gesture_.snap.time) {
        clip_gesture_.snap = {.time = anchor, .kind = TimelineSnapKind::None, .label = {}};
        clip_gesture_.snapped = false;
      } else if (clip_gesture_.rollCutAtStart) {
        targetStart = clip_gesture_.originalStart + delta;
        targetDuration = clip_gesture_.originalDuration - delta;
      } else {
        targetDuration = clip_gesture_.originalDuration + delta;
      }
    } else if (clip_gesture_.mode == EditMode::TrimOut) {
      targetDuration = std::clamp(clip_gesture_.snap.time - targetStart, minimumDuration,
                                  duration_ - targetStart);
    } else {
      targetStart = std::clamp(clip_gesture_.snap.time, qint64{0},
                               std::max<qint64>(0, duration_ - targetDuration));
      if (clip_gesture_.mode == EditMode::TrimIn) {
        targetStart = std::min(targetStart, originalEnd - minimumDuration);
        targetDuration = originalEnd - targetStart;
      }
    }
  }

  clip_gesture_.startDelta = targetStart - clip_gesture_.originalStart;
  if (clip_gesture_.mode == EditMode::Roll) {
    clip_gesture_.startDelta = clip_gesture_.rollCutAtStart
                                   ? targetStart - clip_gesture_.originalStart
                                   : targetStart + targetDuration - originalEnd;
  }
  clip_gesture_.durationDelta = targetDuration - clip_gesture_.originalDuration;
  clip_gesture_.intent = editIntent(modifiers);
  const auto& clip = clips_.at(clip_gesture_.clipIndex);
  emit clipEditPreview(clip.id, clip_gesture_.destinationTrackIndex, clip_gesture_.startDelta,
                       clip_gesture_.durationDelta, clip_gesture_.mode, clip_gesture_.intent,
                       clip_gesture_.snapped);
  emit clipBatchEditPreview(clip_gesture_.clipIds, clip_gesture_.destinationTrackIndex,
                            clip_gesture_.startDelta, clip_gesture_.durationDelta,
                            clip_gesture_.mode, clip_gesture_.intent, clip_gesture_.snap);
  viewport()->setCursor((clip_gesture_.mode == EditMode::Move ||
                         clip_gesture_.mode == EditMode::Slip ||
                         clip_gesture_.mode == EditMode::Slide)
                            ? Qt::ClosedHandCursor
                            : Qt::SizeHorCursor);
  viewport()->update();
}

void TimelineWidget::finishClipGesture(const QPoint& position, Qt::KeyboardModifiers modifiers) {
  if (!clip_gesture_.pointerDown) {
    return;
  }
  updateClipGesture(position, modifiers, false);
  if (!clip_gesture_.dragging || clip_gesture_.clipIndex < 0 ||
      clip_gesture_.clipIndex >= clips_.size()) {
    clip_gesture_ = ClipGesture{};
    if (QWidget::mouseGrabber() == viewport()) {
      viewport()->releaseMouse();
    }
    viewport()->update();
    return;
  }

  const auto clipId = clips_.at(clip_gesture_.clipIndex).id;
  const auto completed = clip_gesture_;
  clip_gesture_ = ClipGesture{};
  if (QWidget::mouseGrabber() == viewport()) {
    viewport()->releaseMouse();
  }
  viewport()->update();
  emit clipEditCommitted(clipId, completed.destinationTrackIndex, completed.startDelta,
                         completed.durationDelta, completed.mode, completed.intent,
                         completed.snapped);
  emit clipBatchEditCommitted(completed.clipIds, completed.destinationTrackIndex,
                              completed.startDelta, completed.durationDelta, completed.mode,
                              completed.intent, completed.snap);
}

void TimelineWidget::cancelClipGesture() {
  if (!clip_gesture_.pointerDown) {
    return;
  }
  QString clipId;
  const auto clipIds = clip_gesture_.clipIds;
  if (clip_gesture_.clipIndex >= 0 && clip_gesture_.clipIndex < clips_.size()) {
    clipId = clips_.at(clip_gesture_.clipIndex).id;
  }
  clip_gesture_ = ClipGesture{};
  if (QWidget::mouseGrabber() == viewport()) {
    viewport()->releaseMouse();
  }
  viewport()->setCursor(Qt::ArrowCursor);
  viewport()->update();
  if (!clipId.isEmpty()) {
    emit clipEditCanceled(clipId);
    emit clipBatchEditCanceled(clipIds);
  }
}

void TimelineWidget::cancelMarkerGesture() {
  if (!marker_gesture_.pointerDown) {
    return;
  }
  QString markerId;
  if (marker_gesture_.markerIndex >= 0 && marker_gesture_.markerIndex < markers_.size()) {
    markerId = markers_.at(marker_gesture_.markerIndex).id;
  }
  marker_gesture_ = MarkerGesture{};
  viewport()->update();
  if (!markerId.isEmpty()) {
    emit markerMoveCanceled(markerId);
  }
}

void TimelineWidget::updateHoverCursor(const QPoint& position) {
  if (transition_gesture_.pointerDown && transition_gesture_.index >= 0) {
    viewport()->setCursor(Qt::SizeHorCursor);
    return;
  }
  const auto transitionIndex = transitionAt(position);
  if (transitionIndex >= 0) {
    const auto& transition = transitions_.at(transitionIndex);
    const auto rect = transitionRect(transition);
    const int handleWidth = transition_handle_pixels_;
    const bool onLeftEdge = position.x() <= rect.left() + handleWidth;
    const bool onRightEdge = position.x() >= rect.right() - handleWidth + 1;
    if (onLeftEdge || onRightEdge) {
      viewport()->setCursor(Qt::SizeHorCursor);
      return;
    }
  }
  const auto hit = clipHitRegionAt(position);
  const auto isEdge = hit == ClipHitRegion::TrimIn || hit == ClipHitRegion::TrimOut;
  if ((tool_mode_ == ToolMode::RippleTrim || tool_mode_ == ToolMode::OverwriteTrim ||
       tool_mode_ == ToolMode::Roll) &&
      isEdge) {
    viewport()->setCursor(Qt::SizeHorCursor);
    return;
  }
  if ((tool_mode_ == ToolMode::RippleTrim || tool_mode_ == ToolMode::OverwriteTrim ||
       tool_mode_ == ToolMode::Roll) &&
      hit != ClipHitRegion::None) {
    viewport()->setCursor(Qt::ArrowCursor);
    return;
  }
  if ((tool_mode_ == ToolMode::Slip || tool_mode_ == ToolMode::Slide) &&
      hit == ClipHitRegion::Body) {
    viewport()->setCursor(Qt::OpenHandCursor);
    return;
  }
  if ((tool_mode_ == ToolMode::Slip || tool_mode_ == ToolMode::Slide) &&
      hit != ClipHitRegion::None) {
    viewport()->setCursor(Qt::ArrowCursor);
    return;
  }
  switch (hit) {
  case ClipHitRegion::TrimIn:
  case ClipHitRegion::TrimOut:
    viewport()->setCursor(Qt::SizeHorCursor);
    break;
  case ClipHitRegion::Body:
    viewport()->setCursor(Qt::OpenHandCursor);
    break;
  case ClipHitRegion::Transition:
    viewport()->setCursor(Qt::ArrowCursor);
    break;
  case ClipHitRegion::None:
    viewport()->setCursor(Qt::ArrowCursor);
    break;
  }
}

void TimelineWidget::autoScrollForDrag(const QPoint& position) {
  const auto horizontalStep = std::max(8, horizontalScrollBar()->singleStep() / 4);
  if (position.x() < header_width_ + auto_scroll_margin_) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - horizontalStep);
  } else if (position.x() > viewport()->width() - auto_scroll_margin_) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + horizontalStep);
  }

  const auto verticalStep = std::max(8, track_height_ / 2);
  if (position.y() < ruler_height_ + auto_scroll_margin_) {
    verticalScrollBar()->setValue(verticalScrollBar()->value() - verticalStep);
  } else if (position.y() > viewport()->height() - auto_scroll_margin_) {
    verticalScrollBar()->setValue(verticalScrollBar()->value() + verticalStep);
  }
}

void TimelineWidget::updateScrollBars() {
  const auto visibleTimelineWidth = std::max(0, viewport()->width() - header_width_);
  const auto timelineContentWidth = std::max(0, contentWidth() - header_width_);
  horizontalScrollBar()->setRange(0, std::max(0, timelineContentWidth - visibleTimelineWidth));
  horizontalScrollBar()->setPageStep(visibleTimelineWidth);
  horizontalScrollBar()->setSingleStep(std::max(12, visibleTimelineWidth / 12));

  const auto visibleTrackHeight = std::max(0, viewport()->height() - ruler_height_);
  const auto trackContentHeight = std::max(0, contentHeight() - ruler_height_);
  verticalScrollBar()->setRange(0, std::max(0, trackContentHeight - visibleTrackHeight));
  verticalScrollBar()->setPageStep(visibleTrackHeight);
  verticalScrollBar()->setSingleStep(track_height_);
}

void TimelineWidget::revealPlayhead() {
  const auto x = xForTime(playhead_);
  const auto left = header_width_ + 20;
  const auto right = viewport()->width() - 20;
  if (x < left) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - (left - x));
  } else if (x > right) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + (x - right));
  }
}

void TimelineWidget::movePlayheadFromPointer(int viewportX, bool emitRequest) {
  const auto position = timeForX(viewportX);
  setPlayhead(position);
  if (emitRequest) {
    emit seekRequested(position);
  }
}

} // namespace video_editor::desktop_ui
