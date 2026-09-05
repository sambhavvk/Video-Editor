// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/program_output_window.hpp"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QVBoxLayout>

namespace video_editor::desktop_ui {

ProgramOutputWindow::ProgramOutputWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
  setObjectName(QStringLiteral("programOutputWindow"));
  setAccessibleName(tr("Program monitor output window"));
  setAccessibleDescription(tr("Fullscreen program output on an external display."));
  setWindowTitle(tr("Program Output"));
  setAttribute(Qt::WA_QuitOnClose, false);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  viewer_ = new ProgramViewer(this);
  viewer_->setObjectName(QStringLiteral("programOutputViewer"));
  viewer_->setAccessibleName(tr("Program output viewer"));
  viewer_->setTitle(tr("Program"));
  viewer_->setNativePresentationEnabled(true);
  viewer_->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(viewer_);

  connect(viewer_, &ProgramViewer::nativePresentationReady, this,
          &ProgramOutputWindow::nativePresentationReady);
  connect(viewer_, &ProgramViewer::nativePresentationResized, this,
          &ProgramOutputWindow::nativePresentationResized);
  connect(viewer_, &ProgramViewer::nativePresentationLost, this,
          &ProgramOutputWindow::nativePresentationLost);
}

void ProgramOutputWindow::showOnScreen(QScreen* screen) {
  if (screen == nullptr) {
    screen = QGuiApplication::primaryScreen();
  }
  if (screen == nullptr) {
    return;
  }
  setScreen(screen);
  setGeometry(screen->geometry());
  showFullScreen();
  viewer_->show();
  viewer_->updateGeometry();
}

void ProgramOutputWindow::closeEvent(QCloseEvent* event) {
  if (viewer_ != nullptr && viewer_->isVisible()) {
    viewer_->clearFrame();
  }
  emit outputClosed();
  QWidget::closeEvent(event);
}

void ProgramOutputWindow::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    close();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

} // namespace video_editor::desktop_ui
