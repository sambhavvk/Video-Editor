// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/keyframe_curve_widget.hpp"

#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace video_editor::desktop_ui {
namespace {

constexpr int kPadding = 16;
constexpr int kHandleRadius = 6;

QPointF clampPoint(const QPointF point, const QRectF& bounds) {
  return {std::clamp(point.x(), bounds.left(), bounds.right()),
          std::clamp(point.y(), bounds.top(), bounds.bottom())};
}

} // namespace

KeyframeCurveWidget::KeyframeCurveWidget(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("keyframeCurve"));
  setAccessibleName(tr("Keyframe curve editor"));
  setAccessibleDescription(
      tr("Select a keyframe, then use the arrow keys or drag its point to edit the value."));
  setFocusPolicy(Qt::StrongFocus);
  setMinimumHeight(140);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void KeyframeCurveWidget::setKeyframes(const QVector<KeyframeView>& keyframes,
                                       const qint64 duration, const double minimum,
                                       const double maximum) {
  keyframes_ = keyframes;
  duration_ = std::max<qint64>(1, duration);
  minimum_ = std::min(minimum, maximum);
  maximum_ = std::max(minimum, maximum);
  if (std::none_of(keyframes_.cbegin(), keyframes_.cend(), [this](const KeyframeView& keyframe) {
        return keyframe.id == selected_keyframe_id_;
      })) {
    selected_keyframe_id_.clear();
  }
  update();
}

void KeyframeCurveWidget::setSelectedKeyframe(const QString& keyframeId) {
  if (selected_keyframe_id_ == keyframeId) {
    return;
  }
  selected_keyframe_id_ = keyframeId;
  update();
}

QRectF KeyframeCurveWidget::plotRect() const {
  return QRectF{static_cast<qreal>(kPadding), static_cast<qreal>(kPadding),
                static_cast<qreal>(std::max(1, width() - 2 * kPadding)),
                static_cast<qreal>(std::max(1, height() - 2 * kPadding))};
}

QPointF KeyframeCurveWidget::pointFor(const KeyframeView& keyframe) const {
  const QRectF bounds = plotRect();
  const double time_ratio = std::clamp(static_cast<double>(keyframe.time) /
                                           static_cast<double>(std::max<qint64>(1, duration_)),
                                       0.0, 1.0);
  const double value_ratio =
      std::clamp((keyframe.value - minimum_) / std::max(1.0e-12, maximum_ - minimum_), 0.0, 1.0);
  return {bounds.left() + time_ratio * bounds.width(),
          bounds.bottom() - value_ratio * bounds.height()};
}

QPointF KeyframeCurveWidget::controlPointFor(const KeyframeView& keyframe,
                                             const bool incoming) const {
  const QPointF point = pointFor(keyframe);
  const QPointF control = incoming ? keyframe.incomingControl : keyframe.outgoingControl;
  return {point.x() + control.x() * plotRect().width(),
          point.y() - control.y() * plotRect().height()};
}

int KeyframeCurveWidget::nearestKeyframe(const QPointF& point) const {
  int index = -1;
  double distance = 14.0;
  for (int candidate = 0; candidate < keyframes_.size(); ++candidate) {
    const double current = QLineF(point, pointFor(keyframes_.at(candidate))).length();
    if (current <= distance) {
      distance = current;
      index = candidate;
    }
  }
  return index;
}

qint64 KeyframeCurveWidget::timeFor(const QPointF& point) const {
  const QRectF bounds = plotRect();
  return static_cast<qint64>(std::llround(
      std::clamp((point.x() - bounds.left()) / std::max(1.0, bounds.width()), 0.0, 1.0) *
      static_cast<double>(duration_)));
}

double KeyframeCurveWidget::valueFor(const QPointF& point) const {
  const QRectF bounds = plotRect();
  const double ratio =
      std::clamp((bounds.bottom() - point.y()) / std::max(1.0, bounds.height()), 0.0, 1.0);
  return minimum_ + ratio * (maximum_ - minimum_);
}

void KeyframeCurveWidget::selectAt(const QPointF& point) {
  const int index = nearestKeyframe(point);
  if (index < 0) {
    return;
  }
  selected_keyframe_id_ = keyframes_.at(index).id;
  emit keyframeSelected(selected_keyframe_id_);
  update();
}

void KeyframeCurveWidget::emitPreview() {
  const auto it =
      std::find_if(keyframes_.cbegin(), keyframes_.cend(),
                   [this](const auto& keyframe) { return keyframe.id == selected_keyframe_id_; });
  if (it == keyframes_.cend()) {
    return;
  }
  emit keyframeValuePreview(it->id, it->time, it->value);
}

void KeyframeCurveWidget::emitCommit() {
  const auto it =
      std::find_if(keyframes_.cbegin(), keyframes_.cend(),
                   [this](const auto& keyframe) { return keyframe.id == selected_keyframe_id_; });
  if (it == keyframes_.cend()) {
    return;
  }
  if (drag_handle_ == DragHandle::Keyframe) {
    emit keyframeValueCommitted(it->id, it->time, it->value);
  } else if (drag_handle_ == DragHandle::IncomingControl ||
             drag_handle_ == DragHandle::OutgoingControl) {
    emit keyframeControlPointsCommitted(it->id, it->incomingControl, it->outgoingControl);
  }
}

void KeyframeCurveWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF bounds = plotRect();
  painter.fillRect(rect(), palette().base());
  painter.setPen(QPen(palette().mid(), 1));
  painter.drawRect(bounds);
  for (int i = 1; i < 4; ++i) {
    const qreal x = bounds.left() + bounds.width() * static_cast<qreal>(i) / 4.0;
    const qreal y = bounds.top() + bounds.height() * static_cast<qreal>(i) / 4.0;
    painter.drawLine(QPointF{x, bounds.top()}, QPointF{x, bounds.bottom()});
    painter.drawLine(QPointF{bounds.left(), y}, QPointF{bounds.right(), y});
  }

  QPainterPath path;
  if (!keyframes_.isEmpty()) {
    path.moveTo(pointFor(keyframes_.front()));
    for (int i = 1; i < keyframes_.size(); ++i) {
      const auto& previous = keyframes_.at(i - 1);
      const auto& current = keyframes_.at(i);
      if (previous.interpolation == KeyframeInterpolationView::Hold) {
        path.lineTo(pointFor(previous).x(), pointFor(current).y());
        path.lineTo(pointFor(current));
      } else if (previous.interpolation == KeyframeInterpolationView::Bezier) {
        path.cubicTo(controlPointFor(previous, false), controlPointFor(current, true),
                     pointFor(current));
      } else {
        path.lineTo(pointFor(current));
      }
    }
  }
  painter.setPen(QPen(palette().highlight(), 2));
  painter.drawPath(path);

  for (const auto& keyframe : keyframes_) {
    const QPointF point = pointFor(keyframe);
    if (keyframe.id == selected_keyframe_id_ &&
        keyframe.interpolation == KeyframeInterpolationView::Bezier) {
      painter.setPen(QPen(palette().mid(), 1));
      painter.drawLine(point, controlPointFor(keyframe, true));
      painter.drawLine(point, controlPointFor(keyframe, false));
      painter.setBrush(palette().mid());
      painter.drawEllipse(controlPointFor(keyframe, true), 4, 4);
      painter.drawEllipse(controlPointFor(keyframe, false), 4, 4);
    }
    painter.setPen(QPen(palette().highlightedText(), 1));
    painter.setBrush(keyframe.id == selected_keyframe_id_ ? palette().highlight()
                                                          : palette().button());
    painter.drawEllipse(point, kHandleRadius, kHandleRadius);
  }
  if (hasFocus()) {
    painter.setPen(QPen(palette().highlight(), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bounds.adjusted(-2, -2, 2, 2));
  }
}

void KeyframeCurveWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  setFocus(Qt::MouseFocusReason);
  selectAt(event->position());
  const auto it =
      std::find_if(keyframes_.cbegin(), keyframes_.cend(),
                   [this](const auto& keyframe) { return keyframe.id == selected_keyframe_id_; });
  if (it == keyframes_.cend()) {
    return;
  }
  drag_position_ = event->position();
  original_time_ = it->time;
  original_value_ = it->value;
  original_incoming_ = it->incomingControl;
  original_outgoing_ = it->outgoingControl;
  drag_handle_ = DragHandle::Keyframe;
  if (it->interpolation == KeyframeInterpolationView::Bezier) {
    if (QLineF(drag_position_, controlPointFor(*it, true)).length() <= 10.0) {
      drag_handle_ = DragHandle::IncomingControl;
    } else if (QLineF(drag_position_, controlPointFor(*it, false)).length() <= 10.0) {
      drag_handle_ = DragHandle::OutgoingControl;
    }
  }
  dragging_ = true;
}

void KeyframeCurveWidget::mouseMoveEvent(QMouseEvent* event) {
  if (!dragging_ || !(event->buttons() & Qt::LeftButton)) {
    QWidget::mouseMoveEvent(event);
    return;
  }
  const auto it = std::find_if(keyframes_.begin(), keyframes_.end(), [this](const auto& keyframe) {
    return keyframe.id == selected_keyframe_id_;
  });
  if (it == keyframes_.end()) {
    return;
  }
  const QRectF bounds = plotRect();
  if (drag_handle_ == DragHandle::Keyframe) {
    const QPointF clamped = clampPoint(event->position(), bounds);
    it->time = timeFor(clamped);
    it->value = valueFor(clamped);
  } else if (drag_handle_ == DragHandle::IncomingControl) {
    const QPointF clamped = clampPoint(event->position(), bounds);
    const QPointF point = pointFor(*it);
    it->incomingControl = {(clamped.x() - point.x()) / bounds.width(),
                           (point.y() - clamped.y()) / bounds.height()};
  } else if (drag_handle_ == DragHandle::OutgoingControl) {
    const QPointF clamped = clampPoint(event->position(), bounds);
    const QPointF point = pointFor(*it);
    it->outgoingControl = {(clamped.x() - point.x()) / bounds.width(),
                           (point.y() - clamped.y()) / bounds.height()};
  }
  emitPreview();
  update();
}

void KeyframeCurveWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && dragging_) {
    dragging_ = false;
    emitCommit();
    drag_handle_ = DragHandle::None;
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void KeyframeCurveWidget::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && dragging_) {
    auto it = std::find_if(keyframes_.begin(), keyframes_.end(), [this](const auto& keyframe) {
      return keyframe.id == selected_keyframe_id_;
    });
    if (it != keyframes_.end()) {
      it->time = original_time_;
      it->value = original_value_;
      it->incomingControl = original_incoming_;
      it->outgoingControl = original_outgoing_;
    }
    dragging_ = false;
    drag_handle_ = DragHandle::None;
    update();
    event->accept();
    return;
  }
  auto it = std::find_if(keyframes_.begin(), keyframes_.end(), [this](const auto& keyframe) {
    return keyframe.id == selected_keyframe_id_;
  });
  if (it == keyframes_.end()) {
    if (event->key() == Qt::Key_Tab && !keyframes_.isEmpty()) {
      selected_keyframe_id_ = keyframes_.front().id;
      emit keyframeSelected(selected_keyframe_id_);
      update();
      return;
    }
    QWidget::keyPressEvent(event);
    return;
  }
  const qint64 frame_step = std::max<qint64>(1, duration_ / 1000);
  if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
    const qint64 delta = event->key() == Qt::Key_Left ? -frame_step : frame_step;
    it->time = std::clamp(it->time + delta, qint64{0}, duration_);
    emitPreview();
    emitCommit();
    update();
    return;
  }
  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
    const double delta = (maximum_ - minimum_) / 100.0;
    it->value =
        std::clamp(it->value + (event->key() == Qt::Key_Up ? delta : -delta), minimum_, maximum_);
    emitPreview();
    emitCommit();
    update();
    return;
  }
  QWidget::keyPressEvent(event);
}

void KeyframeCurveWidget::focusInEvent(QFocusEvent* event) {
  QWidget::focusInEvent(event);
  update();
}

} // namespace video_editor::desktop_ui
