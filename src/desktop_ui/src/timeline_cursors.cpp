// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/timeline_cursors.hpp"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace video_editor::desktop_ui {
namespace {

constexpr int kCursorSize = 32;

struct CursorArt {
  QPixmap pixmap;
  QPoint hotspot{1, 1};
};

void strokePath(QPainter& painter, const QPainterPath& path, const QColor& fill,
                const QColor& outline, qreal width = 1.4) {
  painter.setPen(QPen{outline, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(fill);
  painter.drawPath(path);
}

CursorArt paintArrow() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  QPainterPath path;
  path.moveTo(2, 2);
  path.lineTo(2, 22);
  path.lineTo(8, 16);
  path.lineTo(13, 27);
  path.lineTo(16, 25);
  path.lineTo(11, 15);
  path.lineTo(20, 15);
  path.closeSubpath();
  strokePath(painter, path, QColor{244, 246, 250}, QColor{18, 20, 24}, 1.6);
  art.hotspot = {2, 2};
  return art;
}

CursorArt paintHand(const bool closed) {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(QColor{244, 236, 214});
  if (closed) {
    painter.drawRoundedRect(QRectF{8, 10, 16, 14}, 4, 4);
    painter.drawEllipse(QRectF{10, 6, 12, 10});
  } else {
    painter.drawRoundedRect(QRectF{10, 14, 12, 12}, 3, 3);
    painter.drawRoundedRect(QRectF{8, 7, 4, 12}, 1.5, 1.5);
    painter.drawRoundedRect(QRectF{13, 4, 4, 14}, 1.5, 1.5);
    painter.drawRoundedRect(QRectF{18, 7, 4, 12}, 1.5, 1.5);
    painter.drawRoundedRect(QRectF{22, 10, 4, 10}, 1.5, 1.5);
  }
  art.hotspot = {15, 15};
  return art;
}

CursorArt paintTrim(const QColor& accent) {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(accent);
  QPainterPath left;
  left.moveTo(4, 16);
  left.lineTo(13, 8);
  left.lineTo(13, 24);
  left.closeSubpath();
  QPainterPath right;
  right.moveTo(28, 16);
  right.lineTo(19, 8);
  right.lineTo(19, 24);
  right.closeSubpath();
  painter.drawPath(left);
  painter.drawPath(right);
  painter.setPen(QPen{QColor{236, 240, 247}, 2.0});
  painter.drawLine(16, 6, 16, 26);
  art.hotspot = {16, 16};
  return art;
}

CursorArt paintRipple() {
  auto art = paintTrim(QColor{94, 196, 164});
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.5});
  painter.setBrush(Qt::NoBrush);
  QPainterPath wave;
  wave.moveTo(20, 26);
  wave.cubicTo(23, 22, 25, 30, 28, 26);
  painter.drawPath(wave);
  return art;
}

CursorArt paintOverwrite() {
  auto art = paintTrim(QColor{214, 132, 72});
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.4});
  painter.setBrush(QColor{214, 132, 72});
  painter.drawRect(QRectF{20, 22, 8, 6});
  return art;
}

CursorArt paintRoll() {
  auto art = paintTrim(QColor{120, 168, 224});
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{236, 240, 247}, 1.6, Qt::SolidLine, Qt::RoundCap});
  painter.drawLine(10, 4, 22, 4);
  painter.drawLine(10, 28, 22, 28);
  return art;
}

CursorArt paintSlip() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.6});
  painter.setBrush(QColor{72, 118, 168, 180});
  painter.drawRoundedRect(QRectF{4, 8, 24, 16}, 3, 3);
  painter.setBrush(QColor{214, 222, 236});
  painter.drawRoundedRect(QRectF{8, 11, 16, 10}, 2, 2);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.drawLine(12, 16, 7, 16);
  painter.drawLine(7, 16, 10, 13);
  painter.drawLine(7, 16, 10, 19);
  painter.drawLine(20, 16, 25, 16);
  painter.drawLine(25, 16, 22, 13);
  painter.drawLine(25, 16, 22, 19);
  art.hotspot = {16, 16};
  return art;
}

CursorArt paintSlide() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.5});
  painter.setBrush(QColor{86, 92, 104, 160});
  painter.drawRoundedRect(QRectF{3, 10, 8, 12}, 2, 2);
  painter.drawRoundedRect(QRectF{21, 10, 8, 12}, 2, 2);
  painter.setBrush(QColor{214, 222, 236});
  painter.drawRoundedRect(QRectF{11, 8, 10, 16}, 2, 2);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.drawLine(6, 6, 1, 6);
  painter.drawLine(1, 6, 4, 3);
  painter.drawLine(26, 6, 31, 6);
  painter.drawLine(31, 6, 28, 3);
  art.hotspot = {16, 16};
  return art;
}

CursorArt paintRazor() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(QColor{46, 50, 58});
  QPainterPath handle;
  handle.moveTo(6, 24);
  handle.lineTo(12, 18);
  handle.lineTo(16, 22);
  handle.lineTo(10, 28);
  handle.closeSubpath();
  painter.drawPath(handle);
  painter.setBrush(QColor{228, 232, 240});
  QPainterPath blade;
  blade.moveTo(12, 18);
  blade.lineTo(26, 4);
  blade.lineTo(28, 6);
  blade.lineTo(16, 22);
  blade.closeSubpath();
  painter.drawPath(blade);
  painter.setPen(QPen{QColor{196, 64, 72}, 1.6});
  painter.drawLine(24, 2, 30, 8);
  art.hotspot = {26, 5};
  return art;
}

CursorArt paintZoom(const bool zoom_out) {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.8});
  painter.setBrush(QColor{232, 236, 244, 210});
  painter.drawEllipse(QRectF{4, 4, 18, 18});
  painter.setPen(QPen{QColor{18, 20, 24}, 2.4, Qt::SolidLine, Qt::RoundCap});
  painter.drawLine(20, 20, 28, 28);
  painter.setPen(QPen{QColor{32, 36, 44}, 2.0, Qt::SolidLine, Qt::RoundCap});
  painter.drawLine(8, 13, 18, 13);
  if (!zoom_out) {
    painter.drawLine(13, 8, 13, 18);
  }
  art.hotspot = {13, 13};
  return art;
}

CursorArt paintTrackSelect() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.5});
  painter.setBrush(QColor{214, 222, 236});
  painter.drawRoundedRect(QRectF{3, 12, 10, 10}, 2, 2);
  QPainterPath arrow;
  arrow.moveTo(14, 17);
  arrow.lineTo(24, 17);
  arrow.lineTo(24, 12);
  arrow.lineTo(30, 18);
  arrow.lineTo(24, 24);
  arrow.lineTo(24, 19);
  arrow.lineTo(14, 19);
  arrow.closeSubpath();
  strokePath(painter, arrow, QColor{94, 168, 214}, QColor{18, 20, 24}, 1.4);
  art.hotspot = {8, 17};
  return art;
}

CursorArt paintPen() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(QColor{244, 186, 92});
  QPainterPath nib;
  nib.moveTo(6, 26);
  nib.lineTo(10, 18);
  nib.lineTo(14, 22);
  nib.closeSubpath();
  painter.drawPath(nib);
  painter.setBrush(QColor{236, 240, 247});
  QPainterPath barrel;
  barrel.moveTo(10, 18);
  barrel.lineTo(24, 4);
  barrel.lineTo(28, 8);
  barrel.lineTo(14, 22);
  barrel.closeSubpath();
  painter.drawPath(barrel);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.3});
  painter.drawLine(22, 6, 26, 10);
  art.hotspot = {7, 25};
  return art;
}

CursorArt paintEnvelope() {
  CursorArt art;
  art.pixmap = QPixmap(kCursorSize, kCursorSize);
  art.pixmap.fill(Qt::transparent);
  QPainter painter(&art.pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen{QColor{18, 20, 24}, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin});
  painter.setBrush(QColor{94, 214, 164});
  QPainterPath up;
  up.moveTo(16, 3);
  up.lineTo(22, 12);
  up.lineTo(10, 12);
  up.closeSubpath();
  QPainterPath down;
  down.moveTo(16, 29);
  down.lineTo(22, 20);
  down.lineTo(10, 20);
  down.closeSubpath();
  painter.drawPath(up);
  painter.drawPath(down);
  painter.setPen(QPen{QColor{236, 240, 247}, 2.0});
  painter.drawLine(16, 12, 16, 20);
  painter.setBrush(QColor{244, 186, 92});
  painter.setPen(QPen{QColor{18, 20, 24}, 1.2});
  painter.drawEllipse(QRectF{12.5, 12.5, 7, 7});
  art.hotspot = {16, 16};
  return art;
}

CursorArt paintSelectMove() {
  return paintHand(false);
}

const CursorArt& artFor(const TimelineCursorKind kind) {
  static QHash<int, CursorArt> cache;
  const int key = static_cast<int>(kind);
  if (auto it = cache.find(key); it != cache.end()) {
    return it.value();
  }
  CursorArt art;
  switch (kind) {
  case TimelineCursorKind::SelectMove:
    art = paintSelectMove();
    break;
  case TimelineCursorKind::Trim:
    art = paintTrim(QColor{214, 222, 236});
    break;
  case TimelineCursorKind::RippleTrim:
    art = paintRipple();
    break;
  case TimelineCursorKind::OverwriteTrim:
    art = paintOverwrite();
    break;
  case TimelineCursorKind::Roll:
    art = paintRoll();
    break;
  case TimelineCursorKind::Slip:
    art = paintSlip();
    break;
  case TimelineCursorKind::Slide:
    art = paintSlide();
    break;
  case TimelineCursorKind::Razor:
    art = paintRazor();
    break;
  case TimelineCursorKind::HandOpen:
    art = paintHand(false);
    break;
  case TimelineCursorKind::HandClosed:
    art = paintHand(true);
    break;
  case TimelineCursorKind::ZoomIn:
    art = paintZoom(false);
    break;
  case TimelineCursorKind::ZoomOut:
    art = paintZoom(true);
    break;
  case TimelineCursorKind::TrackSelectForward:
    art = paintTrackSelect();
    break;
  case TimelineCursorKind::Pen:
    art = paintPen();
    break;
  case TimelineCursorKind::Envelope:
    art = paintEnvelope();
    break;
  case TimelineCursorKind::Arrow:
  default:
    art = paintArrow();
    break;
  }
  cache.insert(key, art);
  return cache[key];
}

} // namespace

QCursor timelineCursor(const TimelineCursorKind kind) {
  const auto& art = artFor(kind);
  return QCursor{art.pixmap, art.hotspot.x(), art.hotspot.y()};
}

QIcon timelineToolIcon(const TimelineCursorKind kind) {
  return QIcon{artFor(kind).pixmap};
}

QString timelineCursorName(const TimelineCursorKind kind) {
  switch (kind) {
  case TimelineCursorKind::SelectMove:
    return QStringLiteral("select-move");
  case TimelineCursorKind::Trim:
    return QStringLiteral("trim");
  case TimelineCursorKind::RippleTrim:
    return QStringLiteral("ripple-trim");
  case TimelineCursorKind::OverwriteTrim:
    return QStringLiteral("overwrite-trim");
  case TimelineCursorKind::Roll:
    return QStringLiteral("roll");
  case TimelineCursorKind::Slip:
    return QStringLiteral("slip");
  case TimelineCursorKind::Slide:
    return QStringLiteral("slide");
  case TimelineCursorKind::Razor:
    return QStringLiteral("razor");
  case TimelineCursorKind::HandOpen:
    return QStringLiteral("hand-open");
  case TimelineCursorKind::HandClosed:
    return QStringLiteral("hand-closed");
  case TimelineCursorKind::ZoomIn:
    return QStringLiteral("zoom-in");
  case TimelineCursorKind::ZoomOut:
    return QStringLiteral("zoom-out");
  case TimelineCursorKind::TrackSelectForward:
    return QStringLiteral("track-select");
  case TimelineCursorKind::Pen:
    return QStringLiteral("pen");
  case TimelineCursorKind::Envelope:
    return QStringLiteral("envelope");
  case TimelineCursorKind::Arrow:
    break;
  }
  return QStringLiteral("arrow");
}

} // namespace video_editor::desktop_ui
