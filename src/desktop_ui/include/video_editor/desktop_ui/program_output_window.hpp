// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/program_viewer.hpp"

#include <QWidget>

class QScreen;

namespace video_editor::desktop_ui {

class ProgramOutputWindow final : public QWidget {
  Q_OBJECT

public:
  explicit ProgramOutputWindow(QWidget* parent = nullptr);

  [[nodiscard]] ProgramViewer* viewer() const noexcept {
    return viewer_;
  }

  void showOnScreen(QScreen* screen);

signals:
  void outputClosed();
  void nativePresentationReady(NativePresentationHandles handles);
  void nativePresentationResized(int width, int height);
  void nativePresentationLost();

protected:
  void closeEvent(QCloseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  ProgramViewer* viewer_{nullptr};
};

} // namespace video_editor::desktop_ui
