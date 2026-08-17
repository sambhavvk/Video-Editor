// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "accessibility_audit.hpp"

#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QCoreApplication>
#include <QDockWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using video_editor::desktop_ui::AudioTrackView;
using video_editor::desktop_ui::CaptionRowView;
using video_editor::desktop_ui::EditorWindow;
using video_editor::desktop_ui::MediaItemView;
using video_editor::desktop_ui::TimelineClipView;
using video_editor::desktop_ui::TimelineTrackView;
using video_editor::desktop_ui::TrackKind;
using video_editor::desktop_ui::Workspace;
using video_editor::desktop_ui::test::firstUnlabeledInteractive;

class BeginnerWalkthroughTest final : public QObject {
  Q_OBJECT

private slots:
  void firstRunFifteenMinutePathLabelsPrimaryControls();
};

namespace {

std::unique_ptr<QSettings> temporarySettings(const QTemporaryDir& directory) {
  return std::make_unique<QSettings>(directory.filePath(QStringLiteral("beginner-ui.ini")),
                                     QSettings::IniFormat);
}

QDockWidget* requireDock(EditorWindow& window, const char* name) {
  return window.findChild<QDockWidget*>(QString::fromLatin1(name));
}

} // namespace

void BeginnerWalkthroughTest::firstRunFifteenMinutePathLabelsPrimaryControls() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  window.show();
  QCoreApplication::processEvents();

  window.setWorkspace(Workspace::Import);
  QCoreApplication::processEvents();
  QVERIFY(requireDock(window, "mediaDock")->isVisible());
  auto* import = window.findChild<QToolButton*>(QStringLiteral("importMediaButton"));
  QVERIFY(import != nullptr);
  QCOMPARE(import->accessibleName(), QStringLiteral("Import media"));

  const MediaItemView item{.id = QStringLiteral("dialogue"),
                           .displayName = QStringLiteral("dialogue.wav"),
                           .filePath = QStringLiteral("/media/dialogue.wav"),
                           .durationText = QStringLiteral("00:00:01"),
                           .formatText = QStringLiteral("PCM 48 kHz")};
  window.setMediaItems({item});
  QCOMPARE(window.mediaBin()->items().size(), 1);

  window.setWorkspace(Workspace::Edit);
  QCoreApplication::processEvents();
  window.setTimelineView(
      48'000, 48'000,
      {{QStringLiteral("video-1"), QStringLiteral("V1 · Primary"), TrackKind::Video},
       {QStringLiteral("audio-1"), QStringLiteral("A1 · Dialogue"), TrackKind::Audio}},
      {{QStringLiteral("clip-1"), QStringLiteral("dialogue.wav"), 1, 0, 48'000}});
  QVERIFY(window.timeline() != nullptr);
  QCOMPARE(window.timeline()->accessibleName(), QStringLiteral("Timeline"));

  window.setWorkspace(Workspace::AudioCaptions);
  QCoreApplication::processEvents();
  QVERIFY(requireDock(window, "mixerDock")->isVisible());
  QVERIFY(requireDock(window, "captionsDock")->isVisible());

  window.audioMixer()->setTracks(
      {AudioTrackView{.id = QStringLiteral("audio-1"),
                      .displayName = QStringLiteral("Dialogue"),
                      .gain_db = 0.0}});
  auto* fader = window.audioMixer()->findChild<QSlider*>(QStringLiteral("audioFader.0"));
  QVERIFY(fader != nullptr);
  QCOMPARE(fader->accessibleName(), QStringLiteral("Gain for Dialogue"));
  QSignalSpy gain(window.audioMixer(), &video_editor::desktop_ui::AudioMixerWidget::gainEdited);
  fader->setValue(-6);
  QVERIFY(gain.count() >= 1);
  QCOMPARE(gain.last().at(0).toInt(), 0);
  QCOMPARE(gain.last().at(1).toDouble(), -6.0);

  const CaptionRowView caption{.id = QStringLiteral("caption-1"),
                               .timecode = QStringLiteral("00:00:00:00 → 00:00:01:00"),
                               .text = QStringLiteral("Welcome to the edit"),
                               .start = 0,
                               .end = 48'000};
  window.captionsPanel()->setCaptionRows({caption});
  auto* addCaption = window.captionsPanel()->findChild<QPushButton*>(QStringLiteral("addCaptionButton"));
  QVERIFY(addCaption != nullptr);
  QCOMPARE(addCaption->accessibleName(), QStringLiteral("Add a caption at the playhead"));
  QSignalSpy added(window.captionsPanel(),
                   &video_editor::desktop_ui::CaptionsPanelWidget::addCaptionRequested);
  addCaption->click();
  QCOMPARE(added.count(), 1);

  window.setWorkspace(Workspace::Deliver);
  QCoreApplication::processEvents();
  QVERIFY(requireDock(window, "deliverDock")->isVisible());
  auto* exportButton = window.deliverPanel()->findChild<QToolButton*>(QStringLiteral("exportButton"));
  QVERIFY(exportButton != nullptr);
  QCOMPARE(exportButton->accessibleName(), QStringLiteral("Export video master"));
  QVERIFY(!window.deliverPanel()->selectedPresetId().isEmpty());
  window.setMediaItems({});
  QVERIFY(!exportButton->isEnabled());
  window.setMediaItems({item});
  if (exportButton->isEnabled()) {
    QVERIFY(!window.deliverPanel()->selectedPresetId().isEmpty());
  }

  const QString unlabeled = firstUnlabeledInteractive(window);
  QVERIFY2(unlabeled.isEmpty(), qPrintable(unlabeled));
}

QTEST_MAIN(BeginnerWalkthroughTest)
#include "beginner_walkthrough_test.moc"
