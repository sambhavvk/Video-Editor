// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/shortcut_bindings.hpp"

#include <QAction>
#include <QMessageBox>
#include <QSettings>

#include <algorithm>

namespace video_editor::desktop_ui {
namespace {

QString strippedActionText(const QAction* action) {
  return action == nullptr ? QString{} : action->text().remove(u'&').trimmed();
}

bool shortcutsMatch(const QKeySequence& lhs, const QKeySequence& rhs) {
  return !lhs.isEmpty() && lhs.matches(rhs);
}

const QList<QKeySequence>& reservedShortcuts() {
  static const QList<QKeySequence> shortcuts{
      QKeySequence{Qt::Key_J},
      QKeySequence{Qt::Key_K},
      QKeySequence{Qt::Key_L},
      QKeySequence{Qt::Key_Space},
      QKeySequence{QStringLiteral("Ctrl+1")},
      QKeySequence{QStringLiteral("Ctrl+2")},
      QKeySequence{QStringLiteral("Ctrl+3")},
      QKeySequence{QStringLiteral("Ctrl+4")},
  };
  return shortcuts;
}

const QStringList& pinnedCommands() {
  static const QStringList commands{
      QStringLiteral("reverse"),
      QStringLiteral("stop"),
      QStringLiteral("forward"),
      QStringLiteral("playPause"),
      QStringLiteral("workspace.0"),
      QStringLiteral("workspace.1"),
      QStringLiteral("workspace.2"),
      QStringLiteral("workspace.3"),
  };
  return commands;
}

void applyShortcutToAction(QAction* action, const QKeySequence& shortcut) {
  if (action == nullptr) {
    return;
  }
  if (shortcut.isEmpty()) {
    action->setShortcut({});
    return;
  }
  action->setShortcut(shortcut);
  action->setShortcutContext(Qt::WindowShortcut);
}

void persistBinding(QSettings* settings, const QString& commandId,
                    const QHash<QString, QKeySequence>& defaults, const QKeySequence& shortcut) {
  if (settings == nullptr) {
    return;
  }
  const auto key = ShortcutBindings::settingsKey(commandId);
  if (shortcut.isEmpty() || shortcut == defaults.value(commandId)) {
    settings->remove(key);
    return;
  }
  settings->setValue(key, shortcut.toString());
}

} // namespace

QString ShortcutBindings::settingsKey(const QString& commandId) {
  return QStringLiteral("shortcuts/v1/%1").arg(commandId);
}

bool ShortcutBindings::isReservedShortcut(const QKeySequence& shortcut) {
  if (shortcut.isEmpty()) {
    return false;
  }
  for (const auto& reserved : reservedShortcuts()) {
    if (shortcutsMatch(shortcut, reserved)) {
      return true;
    }
  }
  return false;
}

bool ShortcutBindings::isPinnedCommand(const QString& commandId) {
  return pinnedCommands().contains(commandId);
}

QAction* ShortcutBindings::findConflictingAction(const QHash<QString, QAction*>& actions,
                                                 const QString& commandId,
                                                 const QKeySequence& shortcut) {
  if (shortcut.isEmpty()) {
    return nullptr;
  }
  for (auto it = actions.cbegin(); it != actions.cend(); ++it) {
    if (it.key() == commandId) {
      continue;
    }
    auto* action = it.value();
    if (action != nullptr && shortcutsMatch(action->shortcut(), shortcut)) {
      return action;
    }
  }
  return nullptr;
}

void ShortcutBindings::loadOverrides(QSettings* settings,
                                     const QHash<QString, QAction*>& actions) {
  if (settings == nullptr) {
    return;
  }
  for (auto it = actions.cbegin(); it != actions.cend(); ++it) {
    if (isPinnedCommand(it.key())) {
      continue;
    }
    const auto stored = settings->value(settingsKey(it.key()));
    if (!stored.isValid()) {
      continue;
    }
    const auto text = stored.toString().trimmed();
    if (text.isEmpty()) {
      applyShortcutToAction(it.value(), {});
      continue;
    }
    const QKeySequence shortcut{text};
    if (shortcut.isEmpty() || isReservedShortcut(shortcut)) {
      continue;
    }
    applyShortcutToAction(it.value(), shortcut);
  }
}

QString ShortcutBindings::applyBinding(QSettings* settings,
                                       const QHash<QString, QAction*>& actions,
                                       const QHash<QString, QKeySequence>& defaults,
                                       const QString& commandId, const QKeySequence& shortcut,
                                       bool replaceConflicts, QWidget* messageParent) {
  auto* action = actions.value(commandId, nullptr);
  if (action == nullptr) {
    return QObject::tr("Unknown command.");
  }
  if (isPinnedCommand(commandId)) {
    return QObject::tr("This command uses a reserved transport or workspace shortcut and cannot "
                       "be remapped.");
  }
  if (!shortcut.isEmpty() && isReservedShortcut(shortcut)) {
    return QObject::tr("That shortcut is reserved for transport or workspace controls.");
  }

  const auto defaultShortcut = defaults.value(commandId);
  if (!shortcut.isEmpty()) {
    if (auto* conflict = findConflictingAction(actions, commandId, shortcut)) {
      if (!replaceConflicts) {
        return QObject::tr("%1 is already assigned to %2.")
            .arg(shortcut.toString(QKeySequence::NativeText), strippedActionText(conflict));
      }
      const auto conflictId = conflict->property("commandId").toString();
      if (!conflictId.isEmpty() && !isPinnedCommand(conflictId)) {
        applyShortcutToAction(conflict, defaults.value(conflictId));
        persistBinding(settings, conflictId, defaults, conflict->shortcut());
      }
    }
  }

  applyShortcutToAction(action, shortcut.isEmpty() ? defaultShortcut : shortcut);
  persistBinding(settings, commandId, defaults,
                 shortcut.isEmpty() ? defaultShortcut : shortcut);
  return {};
}

void ShortcutBindings::resetBinding(QSettings* settings, const QHash<QString, QAction*>& actions,
                                    const QHash<QString, QKeySequence>& defaults,
                                    const QString& commandId) {
  if (isPinnedCommand(commandId)) {
    return;
  }
  applyShortcutToAction(actions.value(commandId, nullptr), defaults.value(commandId));
  if (settings != nullptr) {
    settings->remove(settingsKey(commandId));
  }
}

void ShortcutBindings::resetAllBindings(QSettings* settings,
                                        const QHash<QString, QAction*>& actions,
                                        const QHash<QString, QKeySequence>& defaults) {
  for (auto it = actions.cbegin(); it != actions.cend(); ++it) {
    if (isPinnedCommand(it.key())) {
      continue;
    }
    applyShortcutToAction(it.value(), defaults.value(it.key()));
  }
  if (settings == nullptr) {
    return;
  }
  settings->beginGroup(QStringLiteral("shortcuts/v1"));
  settings->remove(QString{});
  settings->endGroup();
}

QString ShortcutBindings::formatShortcutHelp(const QHash<QString, QAction*>& actions) {
  QList<const QAction*> listed;
  listed.reserve(actions.size());
  for (auto* action : actions) {
    if (action != nullptr && !action->shortcut().isEmpty()) {
      listed.append(action);
    }
  }
  std::sort(listed.begin(), listed.end(), [](const QAction* lhs, const QAction* rhs) {
    return QString::localeAwareCompare(strippedActionText(lhs), strippedActionText(rhs)) < 0;
  });

  QStringList lines;
  lines.reserve(listed.size());
  for (const auto* action : listed) {
    lines.append(QStringLiteral("%1 — %2")
                     .arg(strippedActionText(action),
                          action->shortcut().toString(QKeySequence::NativeText)));
  }
  return lines.join(QLatin1Char('\n'));
}

} // namespace video_editor::desktop_ui
