// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>
#include <QList>

class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace video_editor::desktop_ui {

class CommandPalette final : public QDialog {
  Q_OBJECT

public:
  explicit CommandPalette(QWidget* parent = nullptr);

  void setActions(const QList<QAction*>& actions);
  [[nodiscard]] int visibleCommandCount() const;

public slots:
  void openPalette();

private slots:
  void filterCommands(const QString& query);
  void triggerCurrent();
  void triggerItem(QListWidgetItem* item);

private:
  void rebuild(const QString& query);

  QLineEdit* search_{nullptr};
  QListWidget* commands_{nullptr};
  QList<QAction*> actions_;
};

} // namespace video_editor::desktop_ui
