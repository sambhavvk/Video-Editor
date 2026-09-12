// SPDX-License-Identifier: MPL-2.0

#include "video_editor/desktop_ui/restore_points_dialog.hpp"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace video_editor::desktop_ui {

RestorePointsDialog::RestorePointsDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("restorePointsDialog"));
  setAccessibleName(tr("Restore points"));
  setWindowTitle(tr("Restore points"));
  setModal(true);
  resize(760, 420);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  preview_label_ = new QLabel(this);
  preview_label_->setObjectName(QStringLiteral("restorePointPreview"));
  preview_label_->setAccessibleName(tr("Restore preview"));
  preview_label_->setWordWrap(true);
  layout->addWidget(preview_label_);

  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("restorePointsTable"));
  table_->setAccessibleName(tr("Restore points list"));
  table_->setColumnCount(5);
  table_->setHorizontalHeaderLabels(
      {tr("Name"), tr("Created"), tr("Sequence"), tr("Revision"), tr("Source")});
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  layout->addWidget(table_, 1);

  auto* buttons = new QHBoxLayout();
  create_button_ = new QPushButton(tr("Create restore point…"), this);
  create_button_->setObjectName(QStringLiteral("createRestorePoint"));
  restore_button_ = new QPushButton(tr("Restore selected"), this);
  restore_button_->setObjectName(QStringLiteral("restoreSelectedPoint"));
  auto* close = new QPushButton(tr("Close"), this);
  buttons->addWidget(create_button_);
  buttons->addWidget(restore_button_);
  buttons->addStretch(1);
  buttons->addWidget(close);
  layout->addLayout(buttons);

  connect(create_button_, &QPushButton::clicked, this, &RestorePointsDialog::promptCreate);
  connect(restore_button_, &QPushButton::clicked, this, [this] {
    const QString id = selectedRestorePointId();
    if (!id.isEmpty()) {
      emit restorePointRequested(id);
    }
  });
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    updateActions();
    const int row = table_->currentRow();
    if (row >= 0 && row < points_.size()) {
      preview_label_->setText(points_.at(row).previewText);
    } else {
      preview_label_->clear();
    }
  });
  updateActions();
}

void RestorePointsDialog::setRestorePoints(const QVector<RestorePointView>& points) {
  points_ = points;
  rebuildTable();
}

QString RestorePointsDialog::selectedRestorePointId() const {
  const int row = table_->currentRow();
  if (row < 0 || row >= points_.size()) {
    return {};
  }
  return points_.at(row).id;
}

void RestorePointsDialog::rebuildTable() {
  table_->setRowCount(points_.size());
  for (int row = 0; row < points_.size(); ++row) {
    const RestorePointView& point = points_.at(row);
    table_->setItem(row, 0, new QTableWidgetItem(point.name));
    table_->setItem(row, 1, new QTableWidgetItem(point.createdText));
    table_->setItem(row, 2, new QTableWidgetItem(point.sequenceName));
    table_->setItem(row, 3, new QTableWidgetItem(point.revisionText));
    table_->setItem(row, 4, new QTableWidgetItem(point.sourceLabel));
    if (table_->item(row, 0) != nullptr) {
      table_->item(row, 0)->setData(Qt::UserRole, point.id);
    }
  }
  table_->resizeColumnsToContents();
  if (!points_.isEmpty()) {
    table_->selectRow(0);
    preview_label_->setText(points_.front().previewText);
  } else {
    preview_label_->setText(tr("No restore points yet. Create one to capture the current project."));
  }
  updateActions();
}

void RestorePointsDialog::updateActions() {
  const bool has_selection = !selectedRestorePointId().isEmpty();
  restore_button_->setEnabled(has_selection);
}

void RestorePointsDialog::promptCreate() {
  bool accepted = false;
  const QString name = QInputDialog::getText(
      this, tr("Create restore point"), tr("Name this restore point:"), QLineEdit::Normal,
      tr("Before major edit"), &accepted);
  if (accepted && !name.trimmed().isEmpty()) {
    emit createRestorePointRequested(name.trimmed());
  }
}

} // namespace video_editor::desktop_ui
