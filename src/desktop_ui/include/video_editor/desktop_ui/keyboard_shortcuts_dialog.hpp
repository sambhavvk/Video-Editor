// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>

class QAction;
class QLineEdit;
class QPushButton;
class QSettings;
class QTableWidget;

namespace video_editor::desktop_ui {

class ShortcutKeyCaptureDialog final : public QDialog {
  Q_OBJECT

public:
  explicit ShortcutKeyCaptureDialog(QWidget* parent = nullptr);

  [[nodiscard]] QKeySequence capturedShortcut() const noexcept {
    return captured_;
  }
  [[nodiscard]] bool clearedShortcut() const noexcept {
    return cleared_;
  }

protected:
  void keyPressEvent(QKeyEvent* event) override;

private:
  QKeySequence captured_;
  bool cleared_{false};
};

class KeyboardShortcutsDialog final : public QDialog {
  Q_OBJECT

public:
  KeyboardShortcutsDialog(QSettings* settings, const QHash<QString, QAction*>& actions,
                          const QHash<QString, QKeySequence>& defaults, QWidget* parent = nullptr);

signals:
  void shortcutsChanged();

private slots:
  void filterRows(const QString& query);
  void changeShortcutForCurrentRow();
  void resetShortcutForCurrentRow();
  void resetAllShortcuts();

private:
  void rebuildTable(const QString& query);
  [[nodiscard]] QString commandIdForRow(int row) const;
  void refreshRow(int row);
  void notifyBindingError(const QString& message);

  QSettings* settings_{nullptr};
  QHash<QString, QAction*> actions_;
  QHash<QString, QKeySequence> defaults_;
  QLineEdit* search_{nullptr};
  QTableWidget* table_{nullptr};
  QPushButton* change_{nullptr};
  QPushButton* reset_{nullptr};
  QPushButton* reset_all_{nullptr};
};

} // namespace video_editor::desktop_ui
