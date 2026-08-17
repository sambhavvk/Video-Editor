// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "video_editor/desktop_ui/ui_types.hpp"

#include <QDialog>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTableWidget;

namespace video_editor::desktop_ui {

class CacheBrowserDialog final : public QDialog {
  Q_OBJECT

public:
  explicit CacheBrowserDialog(QWidget* parent = nullptr);

  void setInventory(const CacheInventoryView& inventory);
  [[nodiscard]] CacheInventoryView inventory() const;

signals:
  void budgetChanged(qint64 budgetBytes);
  void removeEntryRequested(const QString& assetId, const QString& kindText);
  void removeAssetRequested(const QString& assetId);
  void evictToBudgetRequested();
  void clearAllRequested();

protected:
  void showEvent(QShowEvent* event) override;

private:
  void rebuildTable();
  void updateUsage();
  void updateActionEnabled();
  void confirmClearAll();
  [[nodiscard]] QString selectedAssetId() const;
  [[nodiscard]] QString selectedKindText() const;
  [[nodiscard]] qint64 selectedBudgetBytes() const;

  CacheInventoryView inventory_;
  QLabel* usage_label_{nullptr};
  QProgressBar* usage_bar_{nullptr};
  QComboBox* budget_{nullptr};
  QTableWidget* table_{nullptr};
  QPushButton* remove_selected_{nullptr};
  QPushButton* remove_asset_{nullptr};
  QPushButton* evict_unused_{nullptr};
  QPushButton* clear_all_{nullptr};
};

} // namespace video_editor::desktop_ui
