// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/program_viewer.hpp"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

#include <algorithm>

namespace video_editor::desktop_ui {
namespace {

constexpr int kOuterMargin = 22;
constexpr int kTopBarHeight = 28;
constexpr int kBottomBarHeight = 34;

} // namespace

ProgramViewer::ProgramViewer(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("programViewer"));
  setAccessibleName(tr("Program viewer"));
  setAccessibleDescription(tr("Preview of the current sequence. Press Space to play or pause. "
                              "Media files can be dropped here."));
  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMinimumSize(320, 180);
}

void ProgramViewer::setFrame(const QImage& frame) {
  frame_ = frame;
  update();
}

void ProgramViewer::clearFrame() {
  frame_ = {};
  update();
}

void ProgramViewer::setTimecode(const QString& timecode) {
  if (timecode_ == timecode) {
    return;
  }
  timecode_ = timecode;
  update();
}

void ProgramViewer::setTitle(const QString& title) {
  if (title_ == title) {
    return;
  }
  title_ = title;
  update();
}

void ProgramViewer::setSafeGuidesVisible(bool visible) {
  if (safe_guides_visible_ == visible) {
    return;
  }
  safe_guides_visible_ = visible;
  update();
}

void ProgramViewer::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.fillRect(rect(), QColor{20, 22, 26});

  const auto frameRect = targetFrameRect();
  painter.fillRect(frameRect, Qt::black);

  if (!frame_.isNull()) {
    // A subtle checkerboard remains visible behind frames with transparency.
    constexpr int cell = 12;
    for (int y = frameRect.top(); y <= frameRect.bottom(); y += cell) {
      for (int x = frameRect.left(); x <= frameRect.right(); x += cell) {
        painter.fillRect(QRect{x, y, cell, cell}, ((x / cell) + (y / cell)) % 2 == 0
                                                      ? QColor{39, 41, 46}
                                                      : QColor{29, 31, 35});
      }
    }
    painter.drawImage(frameRect, frame_, frame_.rect());
  } else {
    const auto center = frameRect.center();
    QPainterPath play;
    play.moveTo(center.x() - 12, center.y() - 19);
    play.lineTo(center.x() + 21, center.y());
    play.lineTo(center.x() - 12, center.y() + 19);
    play.closeSubpath();
    painter.fillPath(play, QColor{129, 149, 190});

    QFont headline = font();
    headline.setPointSize(std::max(11, headline.pointSize() + 2));
    headline.setWeight(QFont::DemiBold);
    painter.setFont(headline);
    painter.setPen(QColor{226, 230, 237});
    painter.drawText(frameRect.adjusted(20, 48, -20, -20), Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("Your story starts here"));
    painter.setFont(font());
    painter.setPen(QColor{151, 157, 169});
    painter.drawText(frameRect.adjusted(20, 94, -20, -20), Qt::AlignHCenter | Qt::AlignVCenter,
                     tr("Drop video, audio, or images to import"));
  }

  if (safe_guides_visible_) {
    painter.setPen(QPen{QColor{255, 255, 255, 130}, 1, Qt::DashLine});
    const auto actionSafe = frameRect.adjusted(frameRect.width() / 20, frameRect.height() / 20,
                                               -frameRect.width() / 20, -frameRect.height() / 20);
    const auto titleSafe = frameRect.adjusted(frameRect.width() / 10, frameRect.height() / 10,
                                              -frameRect.width() / 10, -frameRect.height() / 10);
    painter.drawRect(actionSafe);
    painter.drawRect(titleSafe);
    painter.drawLine(frameRect.center().x(), frameRect.top(), frameRect.center().x(),
                     frameRect.bottom());
    painter.drawLine(frameRect.left(), frameRect.center().y(), frameRect.right(),
                     frameRect.center().y());
  }

  painter.setPen(QColor{75, 79, 89});
  painter.drawRect(frameRect.adjusted(0, 0, -1, -1));

  QFont labelFont = font();
  labelFont.setWeight(QFont::DemiBold);
  painter.setFont(labelFont);
  painter.setPen(QColor{184, 190, 202});
  painter.drawText(QRect{kOuterMargin, 0, width() - 2 * kOuterMargin, kTopBarHeight},
                   Qt::AlignLeft | Qt::AlignVCenter, title_);

  QFont timecodeFont{QStringLiteral("monospace")};
  timecodeFont.setStyleHint(QFont::Monospace);
  timecodeFont.setPointSize(std::max(9, font().pointSize()));
  painter.setFont(timecodeFont);
  painter.setPen(QColor{216, 220, 228});
  painter.drawText(QRect{kOuterMargin, height() - kBottomBarHeight, width() - 2 * kOuterMargin,
                         kBottomBarHeight},
                   Qt::AlignHCenter | Qt::AlignVCenter, timecode_);

  if (hasFocus()) {
    painter.setPen(QPen{QColor{100, 139, 212}, 2});
    painter.drawRect(rect().adjusted(1, 1, -2, -2));
  }
}

void ProgramViewer::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData()->hasUrls()) {
    const auto urls = event->mimeData()->urls();
    if (std::any_of(urls.cbegin(), urls.cend(),
                    [](const QUrl& url) { return url.isLocalFile(); })) {
      event->acceptProposedAction();
      return;
    }
  }
  event->ignore();
}

void ProgramViewer::dropEvent(QDropEvent* event) {
  QStringList files;
  for (const auto& url : event->mimeData()->urls()) {
    if (url.isLocalFile() && QFileInfo{url.toLocalFile()}.isFile()) {
      files.append(url.toLocalFile());
    }
  }
  if (!files.isEmpty()) {
    emit filesDropped(files);
    event->acceptProposedAction();
    return;
  }
  event->ignore();
}

void ProgramViewer::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit togglePlaybackRequested();
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void ProgramViewer::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Space) {
    emit togglePlaybackRequested();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

QRect ProgramViewer::targetFrameRect() const {
  const auto available =
      rect().adjusted(kOuterMargin, kTopBarHeight, -kOuterMargin, -kBottomBarHeight);
  if (available.isEmpty()) {
    return {};
  }

  const auto sourceSize = frame_.isNull() ? QSize{16, 9} : frame_.size();
  auto size = sourceSize;
  size.scale(available.size(), Qt::KeepAspectRatio);
  return QRect{
      QPoint{available.center().x() - size.width() / 2, available.center().y() - size.height() / 2},
      size};
}

} // namespace video_editor::desktop_ui
