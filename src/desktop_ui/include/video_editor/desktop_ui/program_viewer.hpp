// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QImage>
#include <QWidget>

#include <memory>
#include <optional>

class QVulkanInstance;
class QWindow;

namespace video_editor::desktop_ui {

struct NativePresentationHandles final {
  quintptr instance{0};
  quintptr surface{0};
  int width{0};
  int height{0};
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
  [[nodiscard]] bool safeGuidesVisible() const noexcept {
    return safe_guides_visible_;
  }
  [[nodiscard]] bool frameSamplingEnabled() const noexcept {
    return frame_sampling_enabled_;
  }
  [[nodiscard]] bool hasFrame() const noexcept {
    return native_presented_ || !frame_.isNull();
  }

public slots:
  void setFrame(const QImage& frame);
  void setSamplingFrameSize(const QSize& size);
  void clearFrame();
  void setTimecode(const QString& timecode);
  void setTitle(const QString& title);
  void setSafeGuidesVisible(bool visible);
  void setSourceEditKeysEnabled(bool enabled);
  void setFrameSamplingEnabled(bool enabled);
  void setNativePresentationEnabled(bool enabled);
  void setNativePresented(bool presented);

signals:
  void filesDropped(const QStringList& localFiles);
  void togglePlaybackRequested();
  void markInRequested();
  void markOutRequested();
  void rippleInsertRequested();
  void overwriteInsertRequested();
  void nativePresentationReady(NativePresentationHandles handles);
  void nativePresentationUnavailable();
  void nativePresentationResized(int width, int height);
  void nativePresentationLost();
  void frameSampleRequested(int frameX, int frameY);

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  [[nodiscard]] QRect targetFrameRect() const;
  [[nodiscard]] std::optional<QPoint> mapWidgetToFramePixel(const QPoint& widget_pos) const;
  [[nodiscard]] bool nativePresentationEligible() const noexcept;
  void tryInitializeNativePresentation();
  void updateVulkanContainerGeometry();
  void paintSafeGuides(QPainter& painter, const QRect& frameRect) const;
  void teardownNativePresentation();

  QImage frame_;
  QSize sampling_frame_size_{16, 9};
  QString timecode_{QStringLiteral("00:00:00:00")};
  QString title_{QStringLiteral("Program")};
  bool safe_guides_visible_{false};
  bool frame_sampling_enabled_{false};
  bool source_edit_keys_{false};
  bool native_presentation_enabled_{false};
  bool native_presented_{false};
  bool native_presentation_ready_{false};
  bool native_presentation_attempted_{false};
  bool native_presentation_outcome_reported_{false};

#if defined(__linux__)
  std::unique_ptr<QVulkanInstance> vulkan_instance_;
  std::unique_ptr<QWindow> vulkan_window_;
  QWidget* vulkan_container_{nullptr};
#endif
};

} // namespace video_editor::desktop_ui
