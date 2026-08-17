// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>

#include <QString>

namespace video_editor::desktop_ui::test {

inline QString describeWidget(const QWidget* widget) {
  if (widget == nullptr) {
    return QStringLiteral("<null>");
  }
  const QString objectName =
      widget->objectName().isEmpty() ? QStringLiteral("<unnamed>") : widget->objectName();
  return QStringLiteral("%1 (%2)").arg(objectName,
                                       QString::fromLatin1(widget->metaObject()->className()));
}

inline bool isQtInternalInteractive(const QWidget* widget) {
  if (widget == nullptr) {
    return true;
  }
  const QString name = widget->objectName();
  if (name.startsWith(QLatin1String("qt_"))) {
    return true;
  }
  const QWidget* parent = widget->parentWidget();
  if (parent == nullptr) {
    return false;
  }
  if (qobject_cast<const QAbstractButton*>(widget) == nullptr) {
    return false;
  }
  // Clear/dropdown/spin arrows and tab-bar scroll buttons are Qt-owned.
  return qobject_cast<const QLineEdit*>(parent) != nullptr ||
         qobject_cast<const QComboBox*>(parent) != nullptr ||
         qobject_cast<const QAbstractSpinBox*>(parent) != nullptr ||
         qobject_cast<const QTabBar*>(parent) != nullptr;
}

inline QString firstUnlabeledInteractive(QWidget& root) {
  const auto children = root.findChildren<QWidget*>();
  QList<QWidget*> widgets;
  widgets.reserve(children.size() + 1);
  widgets.append(&root);
  widgets.append(children);

  for (auto* widget : widgets) {
    if (isQtInternalInteractive(widget)) {
      continue;
    }
    const bool interactive = qobject_cast<QPushButton*>(widget) != nullptr ||
                             qobject_cast<QToolButton*>(widget) != nullptr ||
                             qobject_cast<QLineEdit*>(widget) != nullptr ||
                             qobject_cast<QComboBox*>(widget) != nullptr ||
                             qobject_cast<QSlider*>(widget) != nullptr ||
                             qobject_cast<QTabBar*>(widget) != nullptr;
    if (!interactive) {
      continue;
    }
    if (widget->accessibleName().trimmed().isEmpty()) {
      return describeWidget(widget);
    }
  }

  for (auto* menu : root.findChildren<QMenu*>()) {
    for (auto* action : menu->actions()) {
      if (action->isSeparator() || !action->isVisible()) {
        continue;
      }
      if (action->text().remove(u'&').trimmed().isEmpty()) {
        const QString objectName =
            action->objectName().isEmpty() ? QStringLiteral("<unnamed>") : action->objectName();
        return QStringLiteral("%1 (QAction in %2)")
            .arg(objectName, describeWidget(menu));
      }
    }
  }
  return {};
}

} // namespace video_editor::desktop_ui::test
