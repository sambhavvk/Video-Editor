// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/command_palette.hpp"
#include "video_editor/desktop_ui/editor_window.hpp"

#include <QAction>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace video_editor::desktop_ui {

CommandPalette::CommandPalette(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("commandPalette"));
  setAccessibleName(tr("Command palette"));
  setWindowTitle(tr("Commands"));
  setModal(true);
  resize(560, 420);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  auto* hint = new QLabel(tr("Run a command"), this);
  hint->setObjectName(QStringLiteral("commandPaletteHeading"));
  hint->setAccessibleName(tr("Run a command"));
  QFont heading = hint->font();
  heading.setPointSize(heading.pointSize() + 2);
  heading.setWeight(QFont::DemiBold);
  hint->setFont(heading);
  layout->addWidget(hint);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("commandPaletteSearch"));
  search_->setAccessibleName(tr("Search commands"));
  search_->setPlaceholderText(tr("Type a command or shortcut…"));
  search_->setClearButtonEnabled(true);
  layout->addWidget(search_);

  commands_ = new QListWidget(this);
  commands_->setObjectName(QStringLiteral("commandPaletteResults"));
  commands_->setAccessibleName(tr("Matching commands"));
  commands_->setSelectionMode(QAbstractItemView::SingleSelection);
  commands_->setUniformItemSizes(true);
  layout->addWidget(commands_, 1);

  connect(search_, &QLineEdit::textChanged, this, &CommandPalette::filterCommands);
  connect(search_, &QLineEdit::returnPressed, this, &CommandPalette::triggerCurrent);
  connect(commands_, &QListWidget::itemActivated, this, &CommandPalette::triggerItem);
  connect(commands_, &QListWidget::itemDoubleClicked, this, &CommandPalette::triggerItem);
}

void CommandPalette::setActions(const QList<QAction*>& actions) {
  actions_.clear();
  for (auto* action : actions) {
    if (action != nullptr && !action->isSeparator() && !action->text().isEmpty()) {
      actions_.append(action);
    }
  }
  rebuild(search_->text());
}

int CommandPalette::visibleCommandCount() const {
  return commands_->count();
}

void CommandPalette::openPalette() {
  search_->clear();
  rebuild({});
  if (commands_->count() > 0) {
    commands_->setCurrentRow(0);
  }
  show();
  raise();
  activateWindow();
  search_->setFocus(Qt::ShortcutFocusReason);
  search_->selectAll();
}

void CommandPalette::filterCommands(const QString& query) {
  rebuild(query);
}

void CommandPalette::triggerCurrent() {
  auto* item = commands_->currentItem();
  if (item == nullptr && commands_->count() > 0) {
    item = commands_->item(0);
  }
  triggerItem(item);
}

void CommandPalette::triggerItem(QListWidgetItem* item) {
  if (item == nullptr) {
    return;
  }
  auto* action = item->data(Qt::UserRole).value<QAction*>();
  if (action == nullptr) {
    return;
  }
  QString reason;
  if (auto* window = qobject_cast<EditorWindow*>(parentWidget())) {
    reason = window->commandUnavailableReason(action);
  }
  // #region agent log
  {
    std::ofstream out("/home/sambhav/Projects/VideoEditor/.cursor/debug-4d158f.log", std::ios::app);
    if (out) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
      const auto id = action->property("commandId").toString().toStdString();
      out << "{\"sessionId\":\"4d158f\",\"runId\":\"post-fix\",\"hypothesisId\":\"G\","
             "\"location\":\"command_palette.cpp:triggerItem\",\"message\":\"palette trigger\","
             "\"data\":{\"id\":\""
          << id << "\",\"enabled\":" << action->isEnabled() << ",\"unavailable\":"
          << (!reason.isEmpty()) << "},\"timestamp\":" << ms << "}\n";
    }
  }
  // #endregion
  if (!reason.isEmpty()) {
    if (auto* window = qobject_cast<EditorWindow*>(parentWidget())) {
      window->showTransientMessage(reason);
    }
    return;
  }
  if (!action->isEnabled()) {
    if (auto* window = qobject_cast<EditorWindow*>(parentWidget())) {
      window->showTransientMessage(tr("%1 is not available right now").arg(action->text()));
    }
    return;
  }
  accept();
  action->trigger();
}

void CommandPalette::rebuild(const QString& query) {
  commands_->clear();
  const auto words = query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);

  QList<QAction*> matches;
  for (auto* action : actions_) {
    if (!action->isVisible()) {
      continue;
    }
    const auto haystack = QStringLiteral("%1 %2 %3 %4")
                              .arg(action->text(), action->toolTip(), action->objectName(),
                                   action->shortcut().toString(QKeySequence::NativeText));
    const auto matchesAll =
        std::all_of(words.cbegin(), words.cend(), [&haystack](const QString& word) {
          return haystack.contains(word, Qt::CaseInsensitive);
        });
    if (matchesAll) {
      matches.append(action);
    }
  }

  std::sort(matches.begin(), matches.end(), [](const QAction* lhs, const QAction* rhs) {
    return QString::localeAwareCompare(lhs->text(), rhs->text()) < 0;
  });

  int unavailable = 0;
  for (auto* action : matches) {
    auto* item = new QListWidgetItem(commands_);
    item->setText(action->text());
    item->setData(Qt::UserRole, QVariant::fromValue(action));
    item->setToolTip(action->toolTip());
    if (!action->shortcut().isEmpty()) {
      item->setText(QStringLiteral("%1\t%2").arg(
          action->text(), action->shortcut().toString(QKeySequence::NativeText)));
    }
    QString reason;
    if (auto* window = qobject_cast<EditorWindow*>(parentWidget())) {
      reason = window->commandUnavailableReason(action);
    }
    if (!reason.isEmpty()) {
      item->setForeground(commands_->palette().color(QPalette::Disabled, QPalette::Text));
      item->setToolTip(reason);
      ++unavailable;
    }
  }

  // #region agent log
  {
    std::ofstream out("/home/sambhav/Projects/VideoEditor/.cursor/debug-4d158f.log", std::ios::app);
    if (out) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
      out << "{\"sessionId\":\"4d158f\",\"runId\":\"post-fix\",\"hypothesisId\":\"G\","
             "\"location\":\"command_palette.cpp:rebuild\",\"message\":\"palette rebuild\","
             "\"data\":{\"visible\":"
          << commands_->count() << ",\"unavailable\":" << unavailable << "},\"timestamp\":" << ms
          << "}\n";
    }
  }
  // #endregion

  if (commands_->count() > 0) {
    commands_->setCurrentRow(0);
  }
}

} // namespace video_editor::desktop_ui
