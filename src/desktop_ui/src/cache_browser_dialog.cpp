// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/cache_browser_dialog.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace video_editor::desktop_ui {
namespace {

constexpr qint64 kGigabyte = 1024LL * 1024 * 1024;
constexpr int kNameColumn = 0;
constexpr int kKindRole = Qt::UserRole + 1;

QString formatCacheBytes(const qint64 bytes) {
  constexpr qint64 kKiB = 1024;
  constexpr qint64 kMiB = 1024 * kKiB;
  constexpr qint64 kGiB = 1024 * kMiB;
  if (bytes >= kGiB) {
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / static_cast<double>(kGiB), 0,
                                       'f', 1);
  }
  if (bytes >= kMiB) {
    return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / static_cast<double>(kMiB), 0,
                                       'f', 1);
  }
  if (bytes >= kKiB) {
    return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / static_cast<double>(kKiB), 0,
                                       'f', 1);
  }
  return QObject::tr("%1 bytes").arg(bytes);
}

QString formatLastAccess(const qint64 last_access_utc_ms) {
  if (last_access_utc_ms <= 0) {
    return QStringLiteral("—");
  }
  const auto accessed =
      QDateTime::fromMSecsSinceEpoch(last_access_utc_ms, QTimeZone::UTC).toLocalTime();
  return QLocale().toString(accessed, QLocale::ShortFormat);
}

} // namespace

CacheBrowserDialog::CacheBrowserDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("cacheBrowserDialog"));
  setAccessibleName(tr("Media cache"));
  setWindowTitle(tr("Media cache"));
  setModal(true);
  resize(720, 480);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  usage_label_ = new QLabel(this);
  usage_label_->setObjectName(QStringLiteral("cacheUsageLabel"));
  usage_label_->setAccessibleName(tr("Cache usage"));
  layout->addWidget(usage_label_);

  usage_bar_ = new QProgressBar(this);
  usage_bar_->setObjectName(QStringLiteral("cacheUsageBar"));
  usage_bar_->setAccessibleName(tr("Cache usage"));
  usage_bar_->setTextVisible(false);
  usage_bar_->setRange(0, 1000);
  layout->addWidget(usage_bar_);

  auto* budget_row = new QHBoxLayout;
  auto* budget_label = new QLabel(tr("Budget"), this);
  budget_label->setObjectName(QStringLiteral("cacheBudgetLabel"));
  budget_label->setAccessibleName(tr("Cache budget"));
  budget_ = new QComboBox(this);
  budget_->setObjectName(QStringLiteral("cacheBudgetCombo"));
  budget_->setAccessibleName(tr("Cache budget"));
  budget_label->setBuddy(budget_);
  const std::array budget_options{
      std::pair{10, 10 * kGigabyte},   std::pair{25, 25 * kGigabyte},
      std::pair{50, 50 * kGigabyte},   std::pair{100, 100 * kGigabyte},
      std::pair{200, 200 * kGigabyte},
  };
  for (const auto& [gigabytes, bytes] : budget_options) {
    budget_->addItem(tr("%1 GB").arg(gigabytes), QVariant::fromValue(bytes));
  }
  budget_->setCurrentIndex(3);
  budget_row->addWidget(budget_label);
  budget_row->addWidget(budget_, 1);
  layout->addLayout(budget_row);

  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("cacheEntryTable"));
  table_->setAccessibleName(tr("Cached media artifacts"));
  table_->setColumnCount(4);
  table_->setHorizontalHeaderLabels({tr("Name"), tr("Kind"), tr("Size"), tr("Last accessed")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setShowGrid(false);
  table_->setSortingEnabled(true);
  table_->setTabKeyNavigation(true);
  layout->addWidget(table_, 1);

  auto* buttons = new QHBoxLayout;
  remove_selected_ = new QPushButton(tr("Remove selected"), this);
  remove_selected_->setObjectName(QStringLiteral("cacheRemoveSelected"));
  remove_selected_->setAccessibleName(tr("Remove selected"));
  remove_selected_->setAutoDefault(false);
  remove_asset_ = new QPushButton(tr("Remove asset"), this);
  remove_asset_->setObjectName(QStringLiteral("cacheRemoveAsset"));
  remove_asset_->setAccessibleName(tr("Remove asset"));
  remove_asset_->setAutoDefault(false);
  evict_unused_ = new QPushButton(tr("Evict unused"), this);
  evict_unused_->setObjectName(QStringLiteral("cacheEvictUnused"));
  evict_unused_->setAccessibleName(tr("Evict unused"));
  evict_unused_->setAutoDefault(false);
  clear_all_ = new QPushButton(tr("Clear all…"), this);
  clear_all_->setObjectName(QStringLiteral("cacheClearAll"));
  clear_all_->setAccessibleName(tr("Clear all"));
  clear_all_->setAutoDefault(false);
  auto* close = new QPushButton(tr("Close"), this);
  close->setObjectName(QStringLiteral("cacheClose"));
  close->setAccessibleName(tr("Close"));
  close->setAutoDefault(false);
  buttons->addWidget(remove_selected_);
  buttons->addWidget(remove_asset_);
  buttons->addWidget(evict_unused_);
  buttons->addWidget(clear_all_);
  buttons->addStretch();
  buttons->addWidget(close);
  layout->addLayout(buttons);

  setTabOrder(budget_, table_);
  setTabOrder(table_, remove_selected_);
  setTabOrder(remove_selected_, remove_asset_);
  setTabOrder(remove_asset_, evict_unused_);
  setTabOrder(evict_unused_, clear_all_);
  setTabOrder(clear_all_, close);

  connect(budget_, &QComboBox::currentIndexChanged, this, [this] {
    inventory_.budgetBytes = selectedBudgetBytes();
    updateUsage();
    emit budgetChanged(inventory_.budgetBytes);
  });
  connect(table_, &QTableWidget::itemSelectionChanged, this,
          &CacheBrowserDialog::updateActionEnabled);
  connect(remove_selected_, &QPushButton::clicked, this, [this] {
    const auto asset_id = selectedAssetId();
    if (!asset_id.isEmpty()) {
      emit removeEntryRequested(asset_id, selectedKindText());
    }
  });
  connect(remove_asset_, &QPushButton::clicked, this, [this] {
    const auto asset_id = selectedAssetId();
    if (!asset_id.isEmpty()) {
      emit removeAssetRequested(asset_id);
    }
  });
  connect(evict_unused_, &QPushButton::clicked, this, &CacheBrowserDialog::evictToBudgetRequested);
  connect(clear_all_, &QPushButton::clicked, this, &CacheBrowserDialog::confirmClearAll);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);

  inventory_.budgetBytes = selectedBudgetBytes();
  updateUsage();
  updateActionEnabled();
}

void CacheBrowserDialog::setInventory(const CacheInventoryView& inventory) {
  inventory_ = inventory;
  {
    const QSignalBlocker blocker(budget_);
    const int index = budget_->findData(QVariant::fromValue(inventory.budgetBytes));
    if (index >= 0) {
      budget_->setCurrentIndex(index);
    } else if (inventory.budgetBytes <= 0) {
      budget_->setCurrentIndex(3);
      inventory_.budgetBytes = selectedBudgetBytes();
    }
  }
  rebuildTable();
  updateUsage();
  updateActionEnabled();
}

CacheInventoryView CacheBrowserDialog::inventory() const {
  return inventory_;
}

void CacheBrowserDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  table_->setFocus(Qt::OtherFocusReason);
}

void CacheBrowserDialog::rebuildTable() {
  table_->setSortingEnabled(false);
  table_->setRowCount(inventory_.entries.size());
  for (int row = 0; row < inventory_.entries.size(); ++row) {
    const auto& entry = inventory_.entries.at(row);
    auto* name = new QTableWidgetItem(entry.displayName);
    name->setData(Qt::UserRole, entry.assetId);
    name->setData(kKindRole, entry.kindText);
    table_->setItem(row, kNameColumn, name);
    table_->setItem(row, 1, new QTableWidgetItem(entry.kindText));
    auto* size = new QTableWidgetItem(formatCacheBytes(entry.bytes));
    size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table_->setItem(row, 2, size);
    table_->setItem(row, 3, new QTableWidgetItem(formatLastAccess(entry.lastAccessUtcMs)));
  }
  table_->setSortingEnabled(true);
}

void CacheBrowserDialog::updateUsage() {
  const qint64 budget =
      inventory_.budgetBytes > 0 ? inventory_.budgetBytes : selectedBudgetBytes();
  const qint64 used = std::max<qint64>(0, inventory_.totalBytes);
  usage_label_->setText(tr("Used %1 of %2").arg(formatCacheBytes(used), formatCacheBytes(budget)));
  const int percent =
      budget > 0 ? static_cast<int>(std::min<qint64>(1000, used * 1000 / budget)) : 0;
  usage_bar_->setValue(percent);
}

void CacheBrowserDialog::updateActionEnabled() {
  const bool has_selection = !selectedAssetId().isEmpty();
  remove_selected_->setEnabled(has_selection);
  remove_asset_->setEnabled(has_selection);
}

void CacheBrowserDialog::confirmClearAll() {
  const auto answer = QMessageBox::question(
      this, tr("Clear media cache"),
      tr("Remove cached thumbnails, waveforms, and proxies? Original media files and projects "
         "are kept."),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    emit clearAllRequested();
  }
}

QString CacheBrowserDialog::selectedAssetId() const {
  const auto row = table_->currentRow();
  if (row < 0 || table_->item(row, kNameColumn) == nullptr) {
    return {};
  }
  return table_->item(row, kNameColumn)->data(Qt::UserRole).toString();
}

QString CacheBrowserDialog::selectedKindText() const {
  const auto row = table_->currentRow();
  if (row < 0 || table_->item(row, kNameColumn) == nullptr) {
    return {};
  }
  return table_->item(row, kNameColumn)->data(kKindRole).toString();
}

qint64 CacheBrowserDialog::selectedBudgetBytes() const {
  return budget_->currentData().toLongLong();
}

} // namespace video_editor::desktop_ui
