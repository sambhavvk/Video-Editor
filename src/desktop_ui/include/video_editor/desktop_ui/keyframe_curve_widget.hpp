// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QPointF>
#include <QVector>
#include <QWidget>

namespace video_editor::desktop_ui {

// A compact, keyboard-accessible curve editor for one effect parameter.  It
// intentionally works with presentation values so the widget cannot mutate
// an edit-model snapshot while a drag is still in progress.
class KeyframeCurveWidget final : public QWidget {
  Q_OBJECT

public:
  explicit KeyframeCurveWidget(QWidget* parent = nullptr);

  void setKeyframes(const QVector<KeyframeView>& keyframes, qint64 duration, double minimum,
                    double maximum);
  void setSelectedKeyframe(const QString& keyframeId);
  [[nodiscard]] QString selectedKeyframeId() const noexcept {
    return selected_keyframe_id_;
  }

signals:
  void keyframeSelected(const QString& keyframeId);
  void keyframeValuePreview(const QString& keyframeId, qint64 time, double value);
  void keyframeValueCommitted(const QString& keyframeId, qint64 time, double value);
  void keyframeControlPointsCommitted(const QString& keyframeId, const QPointF& incoming,
                                      const QPointF& outgoing);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;

private:
  enum class DragHandle { None, Keyframe, IncomingControl, OutgoingControl };

  [[nodiscard]] QRectF plotRect() const;
  [[nodiscard]] QPointF pointFor(const KeyframeView& keyframe) const;
  [[nodiscard]] QPointF controlPointFor(const KeyframeView& keyframe, bool incoming) const;
  [[nodiscard]] int nearestKeyframe(const QPointF& point) const;
  [[nodiscard]] qint64 timeFor(const QPointF& point) const;
  [[nodiscard]] double valueFor(const QPointF& point) const;
  void selectAt(const QPointF& point);
  void emitPreview();
  void emitCommit();

  QVector<KeyframeView> keyframes_;
  qint64 duration_{1};
  double minimum_{0.0};
  double maximum_{1.0};
  QString selected_keyframe_id_;
  DragHandle drag_handle_{DragHandle::None};
  QPointF drag_position_;
  qint64 original_time_{0};
  double original_value_{0.0};
  QPointF original_incoming_;
  QPointF original_outgoing_;
  bool dragging_{false};
};

} // namespace video_editor::desktop_ui
