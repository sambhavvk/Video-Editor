// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
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
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  duration_ = std::max<qint64>(0, duration);
  time_scale_ = std::max<qint64>(1, timeScale);
  tracks_ = std::move(tracks);
  clips_ = std::move(clips);
  playhead_ = std::clamp(playhead_, qint64{0}, duration_);
  updateScrollBars();
  viewport()->update();
}

void TimelineWidget::setTracks(QVector<TimelineTrackView> tracks) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  tracks_ = std::move(tracks);
  updateScrollBars();
  viewport()->update();
}

void TimelineWidget::setClips(QVector<TimelineClipView> clips) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
  }
  clips_ = std::move(clips);
  viewport()->update();
}

void TimelineWidget::setDuration(qint64 duration, qint64 timeScale) {
  if (clip_gesture_.pointerDown) {
    cancelClipGesture();
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
  const auto index = activeClipIndex();
  if (index < 0 || frameCount == 0) {
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

  if (clip_gesture_.dragging && clip_gesture_.clipIndex >= 0 &&
      clip_gesture_.clipIndex < clips_.size()) {
    auto previewClip = clips_.at(clip_gesture_.clipIndex);
    previewClip.start += clip_gesture_.startDelta;
    previewClip.duration += clip_gesture_.durationDelta;
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
    painter.setPen(QColor{173, 183, 202});
    painter.drawText(header.adjusted(0, 0, -12, 0), Qt::AlignRight | Qt::AlignVCenter,
                     states.trimmed());
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
  if (event->button() == Qt::LeftButton && event->position().x() >= header_width_ &&
      event->position().y() <= ruler_height_) {
    dragging_playhead_ = true;
    movePlayheadFromPointer(static_cast<int>(event->position().x()), true);
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    const auto position = event->position().toPoint();
    const auto clipIndex = clipAt(position);
    if (clipIndex >= 0) {
      active_clip_id_ = clips_.at(clipIndex).id;
      emit clipActivated(active_clip_id_);
      const auto hitRegion = clipHitRegionAt(position);
      const auto trackIndex = clips_.at(clipIndex).trackIndex;
      if (trackIndex >= 0 && trackIndex < tracks_.size() && !tracks_.at(trackIndex).locked) {
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
  if (clip_gesture_.pointerDown && event->button() == Qt::LeftButton) {
    finishClipGesture(event->position().toPoint(), event->modifiers());
    updateHoverCursor(event->position().toPoint());
    event->accept();
    return;
  }
  QAbstractScrollArea::mouseReleaseEvent(event);
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  const auto index = clipAt(event->position().toPoint());
  if (event->button() == Qt::LeftButton && index >= 0) {
    emit clipActivated(clips_.at(index).id);
    event->accept();
    return;
  }
  QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event) {
  const auto index = clipAt(event->pos());
  if (index >= 0) {
    emit clipContextMenuRequested(clips_.at(index).id, event->globalPos());
    event->accept();
    return;
  }
  QAbstractScrollArea::contextMenuEvent(event);
}

void TimelineWidget::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && clip_gesture_.pointerDown) {
    cancelClipGesture();
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
  return ruler_height_ + tracks_.size() * track_height_;
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

int TimelineWidget::clipAt(const QPoint& position) const {
  for (int index = clips_.size() - 1; index >= 0; --index) {
    if (clipRect(clips_.at(index)).contains(position)) {
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
  clip_gesture_.mode = hitRegion == ClipHitRegion::TrimIn    ? EditMode::TrimIn
                       : hitRegion == ClipHitRegion::TrimOut ? EditMode::TrimOut
                                                             : EditMode::Move;
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
  case EditMode::Move: {
    const auto grabOffset = clip_gesture_.pressPointerTime - clip_gesture_.originalStart;
    const auto latestStart = std::max<qint64>(0, duration_ - clip_gesture_.originalDuration);
    targetStart = std::clamp(currentPointerTime - grabOffset, qint64{0}, latestStart);
    clip_gesture_.destinationTrackIndex = trackAtY(position.y());
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
  }

  clip_gesture_.snapped = false;
  clip_gesture_.snapTime = 0;
  if (!modifiers.testFlag(Qt::ShiftModifier) && snap_threshold_pixels_ > 0) {
    qint64 bestOffset = 0;
    qint64 bestTarget = 0;
    auto bestDistance = snap_threshold_pixels_ + 1;

    const auto considerTarget = [&](qint64 anchor, qint64 target) {
      if (target < 0 || target > duration_) {
        return;
      }
      const auto pixelDistance = std::abs(xForTime(anchor) - xForTime(target));
      if (pixelDistance <= snap_threshold_pixels_ && pixelDistance < bestDistance) {
        bestDistance = pixelDistance;
        bestOffset = target - anchor;
        bestTarget = target;
      }
    };

    const auto considerAnchor = [&](qint64 anchor) {
      considerTarget(anchor, playhead_);
      for (int index = 0; index < clips_.size(); ++index) {
        if (index == clip_gesture_.clipIndex) {
          continue;
        }
        const auto& candidate = clips_.at(index);
        considerTarget(anchor, candidate.start);
        considerTarget(anchor, candidate.start + candidate.duration);
      }
      auto secondIndex = anchor / time_scale_;
      const auto remainder = anchor % time_scale_;
      if (remainder >= time_scale_ / 2 + time_scale_ % 2 &&
          secondIndex < std::numeric_limits<qint64>::max() / time_scale_) {
        ++secondIndex;
      }
      const auto roundedSecond =
          secondIndex > duration_ / time_scale_ ? duration_ : secondIndex * time_scale_;
      considerTarget(anchor, roundedSecond);
    };

    switch (clip_gesture_.mode) {
    case EditMode::Move:
      considerAnchor(targetStart);
      considerAnchor(targetStart + targetDuration);
      if (bestDistance <= snap_threshold_pixels_) {
        targetStart += bestOffset;
        const auto latestStart = std::max<qint64>(0, duration_ - targetDuration);
        targetStart = std::clamp(targetStart, qint64{0}, latestStart);
      }
      break;
    case EditMode::TrimIn:
      considerAnchor(targetStart);
      if (bestDistance <= snap_threshold_pixels_) {
        targetStart =
            std::clamp(targetStart + bestOffset, qint64{0}, originalEnd - minimumDuration);
        targetDuration = originalEnd - targetStart;
      }
      break;
    case EditMode::TrimOut: {
      auto targetEnd = clip_gesture_.originalStart + targetDuration;
      considerAnchor(targetEnd);
      if (bestDistance <= snap_threshold_pixels_) {
        targetEnd = std::clamp(targetEnd + bestOffset,
                               clip_gesture_.originalStart + minimumDuration, duration_);
        targetDuration = targetEnd - clip_gesture_.originalStart;
      }
      break;
    }
    }

    if (bestDistance <= snap_threshold_pixels_) {
      const auto startAligned = targetStart == bestTarget;
      const auto endAligned = targetStart + targetDuration == bestTarget;
      clip_gesture_.snapped =
          clip_gesture_.mode == EditMode::Move
              ? startAligned || endAligned
              : (clip_gesture_.mode == EditMode::TrimIn ? startAligned : endAligned);
      if (clip_gesture_.snapped) {
        clip_gesture_.snapTime = bestTarget;
      }
    }
  }

  clip_gesture_.startDelta = targetStart - clip_gesture_.originalStart;
  clip_gesture_.durationDelta = targetDuration - clip_gesture_.originalDuration;
  clip_gesture_.intent = editIntent(modifiers);
  const auto& clip = clips_.at(clip_gesture_.clipIndex);
  emit clipEditPreview(clip.id, clip_gesture_.destinationTrackIndex, clip_gesture_.startDelta,
                       clip_gesture_.durationDelta, clip_gesture_.mode, clip_gesture_.intent,
                       clip_gesture_.snapped);
  viewport()->setCursor(clip_gesture_.mode == EditMode::Move ? Qt::ClosedHandCursor
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
}

void TimelineWidget::cancelClipGesture() {
  if (!clip_gesture_.pointerDown) {
    return;
  }
  QString clipId;
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
  }
}

void TimelineWidget::updateHoverCursor(const QPoint& position) {
  switch (clipHitRegionAt(position)) {
  case ClipHitRegion::TrimIn:
  case ClipHitRegion::TrimOut:
    viewport()->setCursor(Qt::SizeHorCursor);
    break;
  case ClipHitRegion::Body:
    viewport()->setCursor(Qt::OpenHandCursor);
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
