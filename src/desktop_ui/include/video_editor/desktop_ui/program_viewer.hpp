// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QImage>
#include <QWidget>

namespace video_editor::desktop_ui {

class ProgramViewer final : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString timecode READ timecode WRITE setTimecode)
  Q_PROPERTY(QString title READ title WRITE setTitle)
  Q_PROPERTY(bool safeGuidesVisible READ safeGuidesVisible WRITE setSafeGuidesVisible)

public:
  explicit ProgramViewer(QWidget* parent = nullptr);

  [[nodiscard]] QString timecode() const {
    return timecode_;
  }
  [[nodiscard]] QString title() const {
    return title_;
  }
  [[nodiscard]] bool safeGuidesVisible() const noexcept {
    return safe_guides_visible_;
  }
  [[nodiscard]] bool hasFrame() const noexcept {
    return !frame_.isNull();
  }

public slots:
  void setFrame(const QImage& frame);
  void clearFrame();
  void setTimecode(const QString& timecode);
  void setTitle(const QString& title);
  void setSafeGuidesVisible(bool visible);

signals:
  void filesDropped(const QStringList& localFiles);
  void togglePlaybackRequested();

protected:
  void paintEvent(QPaintEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  [[nodiscard]] QRect targetFrameRect() const;

  QImage frame_;
  QString timecode_{QStringLiteral("00:00:00:00")};
  QString title_{QStringLiteral("Program")};
  bool safe_guides_visible_{false};
};

} // namespace video_editor::desktop_ui
