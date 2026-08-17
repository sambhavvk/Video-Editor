// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "accessibility_audit.hpp"

#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QKeySequence>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using video_editor::desktop_ui::EditorWindow;
using video_editor::desktop_ui::test::firstUnlabeledInteractive;

class AccessibilityTest final : public QObject {
  Q_OBJECT

private slots:
  void interactiveControlsHaveAccessibleNames();
  void tabFocusesTimelineAndMediaSearch();
  void transportShortcutsRemainBound();
};

namespace {

std::unique_ptr<QSettings> temporarySettings(const QTemporaryDir& directory) {
  return std::make_unique<QSettings>(directory.filePath(QStringLiteral("a11y-ui.ini")),
                                     QSettings::IniFormat);
}

} // namespace

void AccessibilityTest::interactiveControlsHaveAccessibleNames() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  window.show();
  QCoreApplication::processEvents();

  const QString unlabeled = firstUnlabeledInteractive(window);
  QVERIFY2(unlabeled.isEmpty(), qPrintable(unlabeled));

  QCOMPARE(window.accessibleName(), QStringLiteral("Video Editor"));
  QCOMPARE(window.mediaBin()->accessibleName(), QStringLiteral("Media bin"));
  QCOMPARE(window.timeline()->accessibleName(), QStringLiteral("Timeline"));
  QCOMPARE(window.programViewer()->accessibleName(), QStringLiteral("Program viewer"));
  QCOMPARE(window.captionsPanel()->accessibleName(), QStringLiteral("Captions and transcript"));
  QCOMPARE(window.deliverPanel()->accessibleName(), QStringLiteral("Deliver and export"));
  QCOMPARE(window.audioMixer()->accessibleName(), QStringLiteral("Audio mixer"));

  auto* mediaSearch = window.findChild<QLineEdit*>(QStringLiteral("mediaSearch"));
  QVERIFY(mediaSearch != nullptr);
  QCOMPARE(mediaSearch->accessibleName(), QStringLiteral("Search media"));
  QVERIFY(!window.timeline()->accessibleDescription().isEmpty());
}

void AccessibilityTest::tabFocusesTimelineAndMediaSearch() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  window.show();
  window.resize(1440, 900);
  QCoreApplication::processEvents();
  window.activateWindow();
  window.setFocus(Qt::OtherFocusReason);

  auto* mediaSearch = window.findChild<QLineEdit*>(QStringLiteral("mediaSearch"));
  QVERIFY(mediaSearch != nullptr);
  QVERIFY(window.timeline()->focusPolicy() & Qt::TabFocus);
  QVERIFY(mediaSearch->focusPolicy() & Qt::TabFocus);

  bool sawTimeline = false;
  bool sawSearch = false;
  for (int step = 0; step < 96; ++step) {
    QTest::keyClick(&window, Qt::Key_Tab);
    QCoreApplication::processEvents();
    auto* focused = QApplication::focusWidget();
    if (focused == nullptr) {
      continue;
    }
    if (focused == window.timeline() || window.timeline()->isAncestorOf(focused)) {
      sawTimeline = true;
    }
    if (focused == mediaSearch) {
      sawSearch = true;
    }
    if (sawTimeline && sawSearch) {
      break;
    }
  }
  QVERIFY2(sawTimeline, "Tab did not move focus to the timeline");
  QVERIFY2(sawSearch, "Tab did not move focus to Search media");
}

void AccessibilityTest::transportShortcutsRemainBound() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());

  QCOMPARE(window.action(QStringLiteral("reverse"))->shortcut(), QKeySequence{Qt::Key_J});
  QCOMPARE(window.action(QStringLiteral("stop"))->shortcut(), QKeySequence{Qt::Key_K});
  QCOMPARE(window.action(QStringLiteral("forward"))->shortcut(), QKeySequence{Qt::Key_L});
  QCOMPARE(window.action(QStringLiteral("playPause"))->shortcut(), QKeySequence{Qt::Key_Space});
  QCOMPARE(window.action(QStringLiteral("workspace.0"))->shortcut(),
           QKeySequence{QStringLiteral("Ctrl+1")});
  QCOMPARE(window.action(QStringLiteral("commandPalette"))->shortcut(),
           QKeySequence{QStringLiteral("Ctrl+Shift+P")});

  QSignalSpy playback(&window, &EditorWindow::playbackRateRequested);
  window.action(QStringLiteral("forward"))->trigger();
  window.action(QStringLiteral("stop"))->trigger();
  QCOMPARE(playback.count(), 2);
  QCOMPARE(playback.at(0).at(0).toDouble(), 1.0);
  QCOMPARE(playback.at(1).at(0).toDouble(), 0.0);
}

QTEST_MAIN(AccessibilityTest)
#include "accessibility_test.moc"
