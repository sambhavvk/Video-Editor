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
#include <QResizeEvent>
#include <QShowEvent>
#include <QUrl>
#if defined(__linux__)
#include <QVulkanInstance>
#include <QWindow>
#endif

#include <algorithm>
#include <memory>

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

ProgramViewer::~ProgramViewer() {
  if (native_presentation_ready_) {
    emit nativePresentationLost();
  }
  teardownNativePresentation();
}

void ProgramViewer::setFrame(const QImage& frame) {
  frame_ = frame;
  if (!frame_.isNull()) {
    sampling_frame_size_ = frame_.size();
  }
  update();
}

void ProgramViewer::setSamplingFrameSize(const QSize& size) {
  if (size.isEmpty()) {
    return;
  }
  sampling_frame_size_ = size;
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

void ProgramViewer::setSourceEditKeysEnabled(bool enabled) {
  source_edit_keys_ = enabled;
}

void ProgramViewer::setFrameSamplingEnabled(bool enabled) {
  if (frame_sampling_enabled_ == enabled) {
    return;
  }
  frame_sampling_enabled_ = enabled;
  setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
  update();
}

void ProgramViewer::setNativePresentationEnabled(bool enabled) {
  native_presentation_enabled_ = enabled;
}

void ProgramViewer::setNativePresented(bool presented) {
  if (native_presented_ == presented) {
    return;
  }
  native_presented_ = presented;
#if defined(__linux__)
  if (vulkan_container_ != nullptr) {
    vulkan_container_->setVisible(native_presentation_ready_ && native_presented_ &&
                                  !safe_guides_visible_);
  }
#endif
  update();
}

bool ProgramViewer::nativePresentationEligible() const noexcept {
  return native_presentation_enabled_ && !source_edit_keys_;
}

void ProgramViewer::tryInitializeNativePresentation() {
#if defined(__linux__)
  if (!nativePresentationEligible() || native_presentation_attempted_) {
    return;
  }
  native_presentation_attempted_ = true;

  vulkan_instance_ = std::make_unique<QVulkanInstance>();
  if (!vulkan_instance_->create()) {
    vulkan_instance_.reset();
    if (!native_presentation_outcome_reported_) {
      native_presentation_outcome_reported_ = true;
      emit nativePresentationUnavailable();
    }
    return;
  }

  vulkan_window_ = std::make_unique<QWindow>();
  vulkan_window_->setSurfaceType(QSurface::VulkanSurface);
  vulkan_window_->setVulkanInstance(vulkan_instance_.get());
  vulkan_window_->create();
  if (!vulkan_window_->isVisible()) {
    vulkan_window_->setVisible(true);
  }

  const auto surface = vulkan_instance_->surfaceForWindow(vulkan_window_.get());
  if (reinterpret_cast<quintptr>(surface) == 0U) {
    teardownNativePresentation();
    if (!native_presentation_outcome_reported_) {
      native_presentation_outcome_reported_ = true;
      emit nativePresentationUnavailable();
    }
    return;
  }

  if (vulkan_container_ == nullptr) {
    vulkan_container_ = QWidget::createWindowContainer(vulkan_window_.get(), this);
    vulkan_container_->setFocusPolicy(Qt::NoFocus);
    vulkan_container_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    vulkan_container_->hide();
  }

  native_presentation_ready_ = true;
  updateVulkanContainerGeometry();
  if (!native_presentation_outcome_reported_) {
    native_presentation_outcome_reported_ = true;
    const auto frameRect = targetFrameRect();
    emit nativePresentationReady(NativePresentationHandles{
        .instance = reinterpret_cast<quintptr>(vulkan_instance_->vkInstance()),
        .surface = reinterpret_cast<quintptr>(surface),
        .width = std::max(1, frameRect.width()),
        .height = std::max(1, frameRect.height()),
    });
  }
#else
  Q_UNUSED(this)
#endif
}

void ProgramViewer::updateVulkanContainerGeometry() {
#if defined(__linux__)
  if (vulkan_container_ == nullptr) {
    return;
  }
  const auto frameRect = targetFrameRect();
  if (frameRect.isEmpty()) {
    vulkan_container_->hide();
    return;
  }
  vulkan_container_->setGeometry(frameRect);
  vulkan_container_->setVisible(native_presentation_ready_ && native_presented_ &&
                                !safe_guides_visible_);
  if (native_presentation_ready_) {
    emit nativePresentationResized(std::max(1, frameRect.width()),
                                   std::max(1, frameRect.height()));
  }
#endif
}

void ProgramViewer::teardownNativePresentation() {
#if defined(__linux__)
  if (vulkan_container_ != nullptr) {
    vulkan_container_->hide();
    vulkan_container_->deleteLater();
    vulkan_container_ = nullptr;
  }
  vulkan_window_.reset();
  vulkan_instance_.reset();
  native_presentation_ready_ = false;
#endif
}

void ProgramViewer::paintSafeGuides(QPainter& painter, const QRect& frameRect) const {
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

void ProgramViewer::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event)
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.fillRect(rect(), QColor{20, 22, 26});

  const auto frameRect = targetFrameRect();
  const bool draw_frame_image = !native_presented_ || safe_guides_visible_;
  if (draw_frame_image) {
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
    } else if (!native_presented_) {
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
  }

  if (safe_guides_visible_) {
    paintSafeGuides(painter, frameRect);
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

void ProgramViewer::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateVulkanContainerGeometry();
}

void ProgramViewer::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  tryInitializeNativePresentation();
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
  if (frame_sampling_enabled_) {
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    emit togglePlaybackRequested();
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void ProgramViewer::keyPressEvent(QKeyEvent* event) {
  if (source_edit_keys_) {
    if (event->key() == Qt::Key_I) {
      emit markInRequested();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_O) {
      emit markOutRequested();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_L) {
      emit rippleInsertRequested();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Comma) {
      emit overwriteInsertRequested();
      event->accept();
      return;
    }
  }
  if (event->key() == Qt::Key_Space) {
    emit togglePlaybackRequested();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void ProgramViewer::mousePressEvent(QMouseEvent* event) {
  if (frame_sampling_enabled_ && event->button() == Qt::LeftButton) {
    if (const auto frame_pixel = mapWidgetToFramePixel(event->pos())) {
      emit frameSampleRequested(frame_pixel->x(), frame_pixel->y());
      event->accept();
      return;
    }
  }
  QWidget::mousePressEvent(event);
}

QRect ProgramViewer::targetFrameRect() const {
  const auto available =
      rect().adjusted(kOuterMargin, kTopBarHeight, -kOuterMargin, -kBottomBarHeight);
  if (available.isEmpty()) {
    return {};
  }

  const auto sourceSize =
      frame_.isNull() ? sampling_frame_size_ : frame_.size();
  auto size = sourceSize;
  size.scale(available.size(), Qt::KeepAspectRatio);
  return QRect{
      QPoint{available.center().x() - size.width() / 2, available.center().y() - size.height() / 2},
      size};
}

std::optional<QPoint> ProgramViewer::mapWidgetToFramePixel(const QPoint& widget_pos) const {
  const QSize source_size = frame_.isNull() ? sampling_frame_size_ : frame_.size();
  if (source_size.isEmpty()) {
    return std::nullopt;
  }
  const auto frame_rect = targetFrameRect();
  if (!frame_rect.contains(widget_pos)) {
    return std::nullopt;
  }
  const double relative_x =
      static_cast<double>(widget_pos.x() - frame_rect.left()) / static_cast<double>(frame_rect.width());
  const double relative_y =
      static_cast<double>(widget_pos.y() - frame_rect.top()) / static_cast<double>(frame_rect.height());
  const int frame_x = std::clamp(static_cast<int>(std::lround(relative_x * (source_size.width() - 1))), 0,
                                 std::max(0, source_size.width() - 1));
  const int frame_y = std::clamp(static_cast<int>(std::lround(relative_y * (source_size.height() - 1))), 0,
                                 std::max(0, source_size.height() - 1));
  return QPoint{frame_x, frame_y};
}

} // namespace video_editor::desktop_ui
