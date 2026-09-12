// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QDialog>
#include <QVector>

class QLabel;
class QPushButton;
class QTableWidget;

namespace video_editor::desktop_ui {

class RestorePointsDialog final : public QDialog {
  Q_OBJECT

public:
  explicit RestorePointsDialog(QWidget* parent = nullptr);

  void setRestorePoints(const QVector<RestorePointView>& points);
  [[nodiscard]] QString selectedRestorePointId() const;

signals:
  void createRestorePointRequested(const QString& name);
  void restorePointRequested(const QString& id);

private:
  void rebuildTable();
  void updateActions();
  void promptCreate();

  QVector<RestorePointView> points_;
  QLabel* preview_label_{nullptr};
  QTableWidget* table_{nullptr};
  QPushButton* create_button_{nullptr};
  QPushButton* restore_button_{nullptr};
};

} // namespace video_editor::desktop_ui
