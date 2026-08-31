// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QHash>
#include <QKeySequence>
#include <QString>

class QAction;
class QSettings;
class QWidget;

namespace video_editor::desktop_ui {

class ShortcutBindings final {
public:
  [[nodiscard]] static QString settingsKey(const QString& commandId);
  [[nodiscard]] static bool isReservedShortcut(const QKeySequence& shortcut);
  [[nodiscard]] static bool isPinnedCommand(const QString& commandId);
  [[nodiscard]] static QAction* findConflictingAction(const QHash<QString, QAction*>& actions,
                                                      const QString& commandId,
                                                      const QKeySequence& shortcut);
  static void loadOverrides(QSettings* settings, const QHash<QString, QAction*>& actions);
  [[nodiscard]] static QString applyBinding(QSettings* settings,
                                            const QHash<QString, QAction*>& actions,
                                            const QHash<QString, QKeySequence>& defaults,
                                            const QString& commandId, const QKeySequence& shortcut,
                                            bool replaceConflicts, QWidget* messageParent);
  static void resetBinding(QSettings* settings, const QHash<QString, QAction*>& actions,
                           const QHash<QString, QKeySequence>& defaults,
                           const QString& commandId);
  static void resetAllBindings(QSettings* settings, const QHash<QString, QAction*>& actions,
                               const QHash<QString, QKeySequence>& defaults);
  [[nodiscard]] static QString formatShortcutHelp(const QHash<QString, QAction*>& actions);
};

} // namespace video_editor::desktop_ui
