// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <memory>
#include <optional>

class QVulkanInstance;
class QWindow;

namespace video_editor::desktop_ui {

struct NativePresentationHandles final {
  quintptr instance{0};
  quintptr surface{0};
  quintptr get_proc_addr{0};
  int width{0};
  int height{0};
};

struct ViewerOverlay final {
  bool visible{false};
  bool crop_handles{false};
  QRectF bounds;
};

class ProgramViewer final : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString timecode READ timecode WRITE setTimecode)
  Q_PROPERTY(QString title READ title WRITE setTitle)
  Q_PROPERTY(bool safeGuidesVisible READ safeGuidesVisible WRITE setSafeGuidesVisible)

public:
  explicit ProgramViewer(QWidget* parent = nullptr);
  ~ProgramViewer() override;

  [[nodiscard]] QString timecode() const {
    return timecode_;
  }
  [[nodiscard]] QString title() const {
    return title_;
  }
  [[nodiscard]] QString timecodeCaption() const {
    return timecode_caption_;
  }
  [[nodiscard]] bool safeGuidesVisible() const noexcept {
    return safe_guides_visible_;
  }
  [[nodiscard]] bool frameSamplingEnabled() const noexcept {
    return frame_sampling_enabled_;
  }
  [[nodiscard]] bool nativePresentationEligible() const noexcept;
  [[nodiscard]] bool hasFrame() const noexcept {
    return native_presented_ || !frame_.isNull();
  }

public slots:
  void setFrame(const QImage& frame);
  void setSamplingFrameSize(const QSize& size);
  void clearFrame();
  void setTrimCompareFrames(const QImage& outgoing, const QImage& incoming);
  void clearTrimCompareFrames();
  void setTimecode(const QString& timecode);
  void setTimecodeCaption(const QString& caption);
  void setTitle(const QString& title);
  void setSafeGuidesVisible(bool visible);
  void setSourceEditKeysEnabled(bool enabled);
  void setFrameSamplingEnabled(bool enabled);
  void setNativePresentationEnabled(bool enabled);
  void setNativePresented(bool presented);
  void setCanvasSize(int width, int height);
  void setViewerOverlay(const ViewerOverlay& overlay);
  void setClipInfoOverlay(bool visible, const QString& clipName, const QString& sourceTimecode);
  void setPeakMeters(float leftDbfs, float rightDbfs, bool active);
  [[nodiscard]] QImage currentDisplayImage() const;

signals:
  void filesDropped(const QStringList& localFiles);
  void togglePlaybackRequested();
  void markInRequested();
  void markOutRequested();
  void nativePresentationReady(NativePresentationHandles handles);
  void nativePresentationUnavailable();
  void nativePresentationResized(int width, int height);
  void nativePresentationLost();
  void frameSampleRequested(int frameX, int frameY);
  void viewerTransformPressed(const QString& handle, QPointF sequencePos);
  void viewerTransformMoved(QPointF sequencePos);
  void viewerTransformReleased();

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;

private:
  [[nodiscard]] QRect targetFrameRect() const;
  [[nodiscard]] std::optional<QPoint> mapWidgetToFramePixel(const QPoint& widget_pos) const;
  [[nodiscard]] std::optional<QPointF> mapWidgetToSequence(const QPoint& widget_pos) const;
  [[nodiscard]] QRectF sequenceRectToWidget(const QRectF& sequence_rect) const;
  [[nodiscard]] QString hitTestOverlay(const QPoint& widget_pos) const;
  void updateTransformHud();
  void tryInitializeNativePresentation();
  void updateVulkanContainerGeometry();
  void paintSafeGuides(QPainter& painter, const QRect& frameRect) const;
  void teardownNativePresentation();

  QImage frame_;
  QImage compare_frame_;
  bool trim_compare_active_{false};
  QSize sampling_frame_size_{16, 9};
  QString timecode_{QStringLiteral("00:00:00:00")};
  QString timecode_caption_;
  QString title_{QStringLiteral("Program")};
  bool safe_guides_visible_{false};
  bool frame_sampling_enabled_{false};
  bool source_edit_keys_{false};
  bool native_presentation_enabled_{false};
  bool native_presented_{false};
  bool native_presentation_ready_{false};
  bool native_presentation_attempted_{false};
  bool native_presentation_outcome_reported_{false};
  int canvas_width_{1920};
  int canvas_height_{1080};
  ViewerOverlay overlay_{};
  bool clip_info_visible_{false};
  QString clip_info_name_;
  QString clip_info_source_timecode_;
  float meter_left_dbfs_{0.0F};
  float meter_right_dbfs_{0.0F};
  bool meters_active_{false};
  bool viewer_drag_active_{false};
  QWidget* transform_hud_{nullptr};

#if defined(__linux__)
  std::unique_ptr<QVulkanInstance> vulkan_instance_;
  std::unique_ptr<QWindow> vulkan_window_;
  QWidget* vulkan_container_{nullptr};
#endif
};

} // namespace video_editor::desktop_ui
