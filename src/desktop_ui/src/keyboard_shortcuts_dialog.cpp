// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/keyboard_shortcuts_dialog.hpp"

#include "video_editor/desktop_ui/shortcut_bindings.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace video_editor::desktop_ui {
namespace {

QString strippedActionText(const QAction* action) {
  return action == nullptr ? QString{} : action->text().remove(u'&').trimmed();
}

} // namespace

ShortcutKeyCaptureDialog::ShortcutKeyCaptureDialog(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("shortcutKeyCaptureDialog"));
  setAccessibleName(tr("Capture shortcut"));
  setWindowTitle(tr("Change Shortcut"));
  setModal(true);
  setFocusPolicy(Qt::StrongFocus);
  resize(420, 140);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  auto* prompt = new QLabel(tr("Press the new shortcut, Backspace to clear, or Escape to cancel."),
                            this);
  prompt->setObjectName(QStringLiteral("shortcutCapturePrompt"));
  prompt->setWordWrap(true);
  layout->addWidget(prompt, 1);

  auto* buttons = new QHBoxLayout();
  buttons->addStretch();
  auto* cancel = new QPushButton(tr("Cancel"), this);
  cancel->setObjectName(QStringLiteral("shortcutCaptureCancel"));
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  buttons->addWidget(cancel);
  layout->addLayout(buttons);

  grabKeyboard();
  setFocus(Qt::PopupFocusReason);
}

void ShortcutKeyCaptureDialog::keyPressEvent(QKeyEvent* event) {
  if (event == nullptr) {
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    releaseKeyboard();
    reject();
    return;
  }
  if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
    cleared_ = true;
    captured_ = {};
    releaseKeyboard();
    accept();
    return;
  }
  if (event->key() == Qt::Key_unknown || event->key() == Qt::Key_Shift ||
      event->key() == Qt::Key_Control || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Meta) {
    return;
  }
  captured_ = QKeySequence(static_cast<int>(event->modifiers()) | event->key());
  if (captured_.isEmpty()) {
    return;
  }
  releaseKeyboard();
  accept();
}

KeyboardShortcutsDialog::KeyboardShortcutsDialog(QSettings* settings,
                                                 const QHash<QString, QAction*>& actions,
                                                 const QHash<QString, QKeySequence>& defaults,
                                                 QWidget* parent)
    : QDialog(parent), settings_(settings), actions_(actions), defaults_(defaults) {
  setObjectName(QStringLiteral("keyboardShortcutsDialog"));
  setAccessibleName(tr("Keyboard shortcuts"));
  setWindowTitle(tr("Keyboard Shortcuts"));
  setModal(true);
  resize(760, 520);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  auto* heading = new QLabel(tr("Search commands and change their shortcuts."), this);
  heading->setObjectName(QStringLiteral("keyboardShortcutsHeading"));
  layout->addWidget(heading);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("keyboardShortcutsSearch"));
  search_->setAccessibleName(tr("Search commands"));
  search_->setPlaceholderText(tr("Filter by command name…"));
  search_->setClearButtonEnabled(true);
  layout->addWidget(search_);

  table_ = new QTableWidget(this);
  table_->setObjectName(QStringLiteral("keyboardShortcutsTable"));
  table_->setAccessibleName(tr("Keyboard shortcut bindings"));
  table_->setColumnCount(2);
  table_->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->verticalHeader()->setVisible(false);
  table_->horizontalHeader()->setStretchLastSection(true);
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  layout->addWidget(table_, 1);

  auto* rowActions = new QHBoxLayout();
  change_ = new QPushButton(tr("Change…"), this);
  change_->setObjectName(QStringLiteral("keyboardShortcutsChange"));
  reset_ = new QPushButton(tr("Reset"), this);
  reset_->setObjectName(QStringLiteral("keyboardShortcutsReset"));
  reset_all_ = new QPushButton(tr("Reset all"), this);
  reset_all_->setObjectName(QStringLiteral("keyboardShortcutsResetAll"));
  rowActions->addWidget(change_);
  rowActions->addWidget(reset_);
  rowActions->addStretch();
  rowActions->addWidget(reset_all_);
  layout->addLayout(rowActions);

  auto* closeRow = new QHBoxLayout();
  closeRow->addStretch();
  auto* close = new QPushButton(tr("Close"), this);
  close->setObjectName(QStringLiteral("keyboardShortcutsClose"));
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
  closeRow->addWidget(close);
  layout->addLayout(closeRow);

  connect(search_, &QLineEdit::textChanged, this, &KeyboardShortcutsDialog::filterRows);
  connect(change_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::changeShortcutForCurrentRow);
  connect(reset_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::resetShortcutForCurrentRow);
  connect(reset_all_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::resetAllShortcuts);
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    const auto hasSelection = table_->currentRow() >= 0;
    change_->setEnabled(hasSelection);
    reset_->setEnabled(hasSelection);
  });
  connect(table_, &QTableWidget::cellDoubleClicked, this,
          [this](int row, int) { table_->setCurrentCell(row, 0); changeShortcutForCurrentRow(); });

  rebuildTable({});
  change_->setEnabled(false);
  reset_->setEnabled(false);
}

void KeyboardShortcutsDialog::filterRows(const QString& query) {
  rebuildTable(query);
}

void KeyboardShortcutsDialog::changeShortcutForCurrentRow() {
  const auto row = table_->currentRow();
  if (row < 0) {
    return;
  }
  const auto commandId = commandIdForRow(row);
  if (commandId.isEmpty() || ShortcutBindings::isPinnedCommand(commandId)) {
    notifyBindingError(tr("This command uses a reserved transport or workspace shortcut and cannot "
                          "be remapped."));
    return;
  }

  ShortcutKeyCaptureDialog capture(this);
  if (capture.exec() != QDialog::Accepted) {
    return;
  }

  const QKeySequence shortcut =
      capture.clearedShortcut() ? QKeySequence{} : capture.capturedShortcut();
  if (!shortcut.isEmpty() && ShortcutBindings::isReservedShortcut(shortcut)) {
    notifyBindingError(tr("That shortcut is reserved for transport or workspace controls."));
    return;
  }

  if (auto* conflict = ShortcutBindings::findConflictingAction(actions_, commandId, shortcut)) {
    const auto answer = QMessageBox::question(
        this, tr("Replace Shortcut"),
        tr("%1 is already assigned to %2. Replace it?")
            .arg(shortcut.toString(QKeySequence::NativeText), strippedActionText(conflict)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      return;
    }
    const auto error = ShortcutBindings::applyBinding(settings_, actions_, defaults_, commandId,
                                                    shortcut, true, this);
    if (!error.isEmpty()) {
      notifyBindingError(error);
      return;
    }
    rebuildTable(search_->text());
    emit shortcutsChanged();
    return;
  }

  const auto error = ShortcutBindings::applyBinding(settings_, actions_, defaults_, commandId,
                                                    shortcut, false, this);
  if (!error.isEmpty()) {
    notifyBindingError(error);
    return;
  }
  refreshRow(row);
  emit shortcutsChanged();
}

void KeyboardShortcutsDialog::resetShortcutForCurrentRow() {
  const auto row = table_->currentRow();
  if (row < 0) {
    return;
  }
  const auto commandId = commandIdForRow(row);
  if (commandId.isEmpty()) {
    return;
  }
  ShortcutBindings::resetBinding(settings_, actions_, defaults_, commandId);
  refreshRow(row);
  emit shortcutsChanged();
}

void KeyboardShortcutsDialog::resetAllShortcuts() {
  const auto answer =
      QMessageBox::question(this, tr("Reset All Shortcuts"),
                            tr("Restore every remappable shortcut to its factory default?"),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes) {
    return;
  }
  ShortcutBindings::resetAllBindings(settings_, actions_, defaults_);
  rebuildTable(search_->text());
  emit shortcutsChanged();
}

void KeyboardShortcutsDialog::rebuildTable(const QString& query) {
  const auto words = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
  QStringList commandIds = actions_.keys();
  std::sort(commandIds.begin(), commandIds.end(), [this](const QString& lhs, const QString& rhs) {
    return QString::localeAwareCompare(strippedActionText(actions_.value(lhs)),
                                       strippedActionText(actions_.value(rhs))) < 0;
  });

  table_->setRowCount(0);
  for (const auto& commandId : commandIds) {
    auto* action = actions_.value(commandId, nullptr);
    if (action == nullptr || action->text().isEmpty()) {
      continue;
    }
    const auto haystack = strippedActionText(action);
    const auto matchesAll =
        std::all_of(words.cbegin(), words.cend(), [&haystack](const QString& word) {
          return haystack.contains(word, Qt::CaseInsensitive);
        });
    if (!matchesAll) {
      continue;
    }

    const auto row = table_->rowCount();
    table_->insertRow(row);
    auto* nameItem = new QTableWidgetItem(haystack);
    nameItem->setData(Qt::UserRole, commandId);
    if (ShortcutBindings::isPinnedCommand(commandId)) {
      nameItem->setToolTip(tr("Reserved transport or workspace shortcut"));
    }
    table_->setItem(row, 0, nameItem);
    table_->setItem(row, 1, new QTableWidgetItem(
                                action->shortcut().toString(QKeySequence::NativeText)));
  }
  if (table_->rowCount() > 0) {
    table_->setCurrentCell(0, 0);
  }
}

QString KeyboardShortcutsDialog::commandIdForRow(int row) const {
  if (row < 0 || row >= table_->rowCount()) {
    return {};
  }
  auto* item = table_->item(row, 0);
  return item == nullptr ? QString{} : item->data(Qt::UserRole).toString();
}

void KeyboardShortcutsDialog::refreshRow(int row) {
  const auto commandId = commandIdForRow(row);
  auto* action = actions_.value(commandId, nullptr);
  if (action == nullptr || row < 0 || row >= table_->rowCount()) {
    return;
  }
  if (auto* shortcutItem = table_->item(row, 1)) {
    shortcutItem->setText(action->shortcut().toString(QKeySequence::NativeText));
  }
}

void KeyboardShortcutsDialog::notifyBindingError(const QString& message) {
  QMessageBox::warning(this, tr("Shortcut Not Changed"), message);
}

} // namespace video_editor::desktop_ui
