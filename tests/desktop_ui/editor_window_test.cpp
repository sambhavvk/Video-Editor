// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "video_editor/desktop_ui/editor_window.hpp"

#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QWheelEvent>

using video_editor::desktop_ui::EditorWindow;
using video_editor::desktop_ui::TimelineClipView;
using video_editor::desktop_ui::TimelineTrackView;
using video_editor::desktop_ui::TimelineWidget;
using video_editor::desktop_ui::TrackKind;
using video_editor::desktop_ui::Workspace;

class EditorWindowTest final : public QObject {
  Q_OBJECT

private slots:
  void constructsCompleteShell();
  void switchesWorkspacesAndPanelSets();
  void exposesAccessibleNames();
  void persistsWorkspaceAndProgressiveControls();
  void exposesTransportControllerSignals();
  void mediaBinShowsProxyLifecycle();
  void audioMixerReflectsTrackState();
  void audioMixerDisplaysStableTrackMeters();
  void audioMixerSetTracksKeepsStripsAliveForSameIds();
  void captionsPanelEmitsEditableCueActions();
  void captionsPanelExposesTranscriptionAndWordNavigation();
  void timelineVirtualizesAndSeeks();
  void timelineEmitsTypedMovePreviewAndCommit();
  void timelineDistinguishesTrimRegions();
  void timelineSnapsAndShiftDisablesSnap();
  void timelineCancelsAndDoesNotCommitClicks();
  void timelineHandlesScrolledCoordinatesAndFrameNudges();
  void timelineSupportsMultiSelectionAndToolModes();
  void timelineUsesCanonicalSnapResolver();
  void timelineExposesMarkerGapAndTrackCommands();
  void timelineRequiresClipEdgesForTrimTools();
  void timelineKeepsSlipAndSlideOnClipBodies();
  void timelineRollUsesTheControllerBoundaryConvention();
  void timelineMarkerSnappingExcludesTheDraggedMarker();
  void timelineRefreshCancelsMarkerGesturesAndUsesAuthoritativeSelection();
  void timelineCanCreateTracksWithoutAnExistingTrack();
  void inspectorExposesKeyframeAuthoring();
  void deliverPanelShowsCancelableProgress();
  void audioMixerShowsSystemDefaultAndAuthoritativeLufsStates();
};

namespace {

std::unique_ptr<QSettings> temporarySettings(const QTemporaryDir& directory) {
  return std::make_unique<QSettings>(directory.filePath(QStringLiteral("ui-test.ini")),
                                     QSettings::IniFormat);
}

QDockWidget* requireDock(EditorWindow& window, const char* name) {
  return window.findChild<QDockWidget*>(QString::fromLatin1(name));
}

void configureInteractiveTimeline(
    TimelineWidget& timeline,
    QVector<TimelineClipView> clips = {
        {QStringLiteral("clip-a"), QStringLiteral("Clip A"), 0, 1'000, 2'000},
        {QStringLiteral("clip-b"), QStringLiteral("Clip B"), 0, 4'000, 1'000},
    }) {
  timeline.resize(900, 260);
  timeline.setTimeline(10'000, 1'000,
                       {{QStringLiteral("video-1"), QStringLiteral("Video 1"), TrackKind::Video},
                        {QStringLiteral("video-2"), QStringLiteral("Video 2"), TrackKind::Video}},
                       std::move(clips));
  timeline.setPixelsPerSecond(100.0);
  timeline.horizontalScrollBar()->setValue(0);
  timeline.show();
  QCoreApplication::processEvents();
}

void sendPointer(TimelineWidget& timeline, QEvent::Type type, const QPoint& position,
                 Qt::MouseButton button, Qt::MouseButtons buttons,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  const auto global = timeline.viewport()->mapToGlobal(position);
  QMouseEvent event{type, QPointF{position}, QPointF{global}, button, buttons, modifiers};
  QCoreApplication::sendEvent(timeline.viewport(), &event);
}

TimelineWidget::EditMode editMode(const QList<QVariant>& payload) {
  return payload.at(4).value<TimelineWidget::EditMode>();
}

TimelineWidget::EditIntent editIntent(const QList<QVariant>& payload) {
  return payload.at(5).value<TimelineWidget::EditIntent>();
}

} // namespace

void EditorWindowTest::constructsCompleteShell() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  window.show();
  QCoreApplication::processEvents();

  QCOMPARE(window.workspace(), Workspace::Edit);
  QVERIFY(window.programViewer() != nullptr);
  QVERIFY(window.timeline() != nullptr);
  QVERIFY(window.mediaBin() != nullptr);
  QVERIFY(window.inspector() != nullptr);
  QVERIFY(window.effectsPanel() != nullptr);
  QVERIFY(window.audioMixer() != nullptr);
  QVERIFY(window.captionsPanel() != nullptr);
  QVERIFY(window.deliverPanel() != nullptr);

  for (const auto* dockName :
       {"mediaDock", "inspectorDock", "effectsDock", "mixerDock", "captionsDock", "deliverDock"}) {
    QVERIFY(requireDock(window, dockName) != nullptr);
  }
  QVERIFY(window.findChild<QToolBar*>(QStringLiteral("workspaceToolBar")) != nullptr);
  QVERIFY(window.findChild<QWidget*>(QStringLiteral("precisionTrimPanel")) != nullptr);
  QVERIFY(window.findChild<QWidget*>(QStringLiteral("sourceMonitorContainer")) != nullptr);

  QCOMPARE(window.action(QStringLiteral("reverse"))->shortcut(), QKeySequence{Qt::Key_J});
  QCOMPARE(window.action(QStringLiteral("stop"))->shortcut(), QKeySequence{Qt::Key_K});
  QCOMPARE(window.action(QStringLiteral("forward"))->shortcut(), QKeySequence{Qt::Key_L});
  QCOMPARE(window.action(QStringLiteral("workspace.0"))->shortcut(),
           QKeySequence{QStringLiteral("Ctrl+1")});
  QCOMPARE(window.action(QStringLiteral("workspace.3"))->shortcut(),
           QKeySequence{QStringLiteral("Ctrl+4")});
}

void EditorWindowTest::switchesWorkspacesAndPanelSets() {
  QTemporaryDir directory;
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  window.show();
  QCoreApplication::processEvents();
  QSignalSpy changed(&window, &EditorWindow::workspaceChanged);

  window.setWorkspace(Workspace::Import);
  QCoreApplication::processEvents();
  QCOMPARE(window.workspace(), Workspace::Import);
  QVERIFY(requireDock(window, "mediaDock")->isVisible());
  QVERIFY(requireDock(window, "inspectorDock")->isVisible());
  QVERIFY(!requireDock(window, "mixerDock")->isVisible());

  window.setWorkspace(Workspace::AudioCaptions);
  QCoreApplication::processEvents();
  QCOMPARE(window.workspace(), Workspace::AudioCaptions);
  QVERIFY(requireDock(window, "mixerDock")->isVisible());
  QVERIFY(requireDock(window, "captionsDock")->isVisible());
  QVERIFY(!requireDock(window, "mediaDock")->isVisible());

  window.action(QStringLiteral("workspace.3"))->trigger();
  QCoreApplication::processEvents();
  QCOMPARE(window.workspace(), Workspace::Deliver);
  QVERIFY(requireDock(window, "deliverDock")->isVisible());
  QVERIFY(!requireDock(window, "captionsDock")->isVisible());
  QCOMPARE(changed.count(), 3);
}

void EditorWindowTest::exposesAccessibleNames() {
  QTemporaryDir directory;
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());

  const QStringList requiredObjects{
      QStringLiteral("editorWindow"),    QStringLiteral("programViewer"),
      QStringLiteral("timelineWidget"),  QStringLiteral("mediaBinPanel"),
      QStringLiteral("inspectorPanel"),  QStringLiteral("effectsPanel"),
      QStringLiteral("audioMixerPanel"), QStringLiteral("captionsPanel"),
      QStringLiteral("deliverPanel"),    QStringLiteral("workspaceToolBar"),
      QStringLiteral("editorStatusBar"),
  };
  for (const auto& objectName : requiredObjects) {
    auto* widget = window.findChild<QWidget*>(objectName, Qt::FindChildrenRecursively);
    if (objectName == QStringLiteral("editorWindow")) {
      widget = &window;
    }
    QVERIFY2(widget != nullptr, qPrintable(objectName));
    QVERIFY2(!widget->accessibleName().trimmed().isEmpty(), qPrintable(objectName));
  }

  auto* mediaSearch = window.findChild<QLineEdit*>(QStringLiteral("mediaSearch"));
  QVERIFY(mediaSearch != nullptr);
  QCOMPARE(mediaSearch->accessibleName(), QStringLiteral("Search media"));
  QVERIFY(!window.timeline()->accessibleDescription().isEmpty());
}

void EditorWindowTest::persistsWorkspaceAndProgressiveControls() {
  QTemporaryDir directory;
  auto settings = temporarySettings(directory);
  {
    EditorWindow first(settings.get());
    first.setWorkspace(Workspace::Deliver);
    first.setSourceMonitorVisible(true);
    first.setPrecisionTrimVisible(true);
    first.saveUiState();
  }

  EditorWindow restored(settings.get());
  QCOMPARE(restored.workspace(), Workspace::Deliver);
  QVERIFY(!restored.findChild<QWidget*>(QStringLiteral("sourceMonitorContainer"))->isHidden());
  QVERIFY(!restored.findChild<QWidget*>(QStringLiteral("precisionTrimPanel"))->isHidden());
  QVERIFY(restored.action(QStringLiteral("sourceMonitor"))->isChecked());
  QVERIFY(restored.action(QStringLiteral("precisionTrim"))->isChecked());
}

void EditorWindowTest::exposesTransportControllerSignals() {
  QTemporaryDir directory;
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  QSignalSpy playback(&window, &EditorWindow::playbackRateRequested);

  window.action(QStringLiteral("reverse"))->trigger();
  window.action(QStringLiteral("reverse"))->trigger();
  window.action(QStringLiteral("stop"))->trigger();

  QCOMPARE(playback.count(), 3);
  QCOMPARE(playback.at(0).at(0).toDouble(), -1.0);
  QCOMPARE(playback.at(1).at(0).toDouble(), -2.0);
  QCOMPARE(playback.at(2).at(0).toDouble(), 0.0);
}

void EditorWindowTest::mediaBinShowsProxyLifecycle() {
  video_editor::desktop_ui::MediaBinWidget media_bin;
  auto* table = media_bin.findChild<QTableWidget*>(QStringLiteral("mediaTable"));
  QVERIFY(table != nullptr);

  video_editor::desktop_ui::MediaItemView item{.id = QStringLiteral("asset"),
                                               .displayName = QStringLiteral("camera.mov"),
                                               .filePath = QStringLiteral("/media/camera.mov"),
                                               .durationText = QStringLiteral("00:00:05"),
                                               .formatText = QStringLiteral("H.264 3840×2160"),
                                               .offline = false,
                                               .proxyAvailable = false,
                                               .proxyRecommended = true,
                                               .proxyGenerating = false};
  media_bin.setItems({item});
  QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Proxy recommended"));
  QVERIFY(!table->item(0, 4)->toolTip().isEmpty());

  item.proxyGenerating = true;
  media_bin.setItems({item});
  QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Creating proxy…"));

  item.proxyGenerating = false;
  item.proxyAvailable = true;
  media_bin.setItems({item});
  QCOMPARE(table->item(0, 4)->text(), QStringLiteral("Proxy ready"));
}

void EditorWindowTest::inspectorExposesKeyframeAuthoring() {
  video_editor::desktop_ui::InspectorWidget inspector;
  QSignalSpy interpolation(
      &inspector, &video_editor::desktop_ui::InspectorWidget::effectKeyframeInterpolationEdited);
  QSignalSpy removed(&inspector, &video_editor::desktop_ui::InspectorWidget::effectKeyframeRemoved);
  video_editor::desktop_ui::EffectParameterView parameter{
      .effectId = QStringLiteral("effect"),
      .effectName = QStringLiteral("Color"),
      .parameterId = QStringLiteral("exposure"),
      .displayName = QStringLiteral("Exposure"),
      .value = 0.0,
      .duration = 48'000,
      .keyframes = {
          {.id = QStringLiteral("kf"),
           .time = 24'000,
           .value = 1.0,
           .interpolation = video_editor::desktop_ui::KeyframeInterpolationView::Linear}}};
  inspector.setEffectParameters({parameter});
  auto* list = inspector.findChild<QListWidget*>(QStringLiteral("keyframeList"));
  auto* interpolationBox = inspector.findChild<QComboBox*>(QStringLiteral("keyframeInterpolation"));
  auto* remove = inspector.findChild<QToolButton*>(QStringLiteral("deleteKeyframe"));
  auto* curve = inspector.findChild<QWidget*>(QStringLiteral("keyframeCurve"));
  QVERIFY(list != nullptr);
  QVERIFY(interpolationBox != nullptr);
  QVERIFY(remove != nullptr);
  QVERIFY(curve != nullptr);
  QCOMPARE(list->count(), 1);
  interpolationBox->setCurrentIndex(2);
  QCOMPARE(interpolation.count(), 1);
  QCOMPARE(interpolation.at(0).at(0).toString(), QStringLiteral("effect"));
  QCOMPARE(interpolation.at(0).at(1).toString(), QStringLiteral("exposure"));
  remove->click();
  QCOMPARE(removed.count(), 1);
  QCOMPARE(removed.at(0).at(2).toString(), QStringLiteral("kf"));
}

void EditorWindowTest::audioMixerReflectsTrackState() {
  video_editor::desktop_ui::AudioMixerWidget mixer;
  QSignalSpy muted(&mixer, &video_editor::desktop_ui::AudioMixerWidget::muteToggled);
  QSignalSpy soloed(&mixer, &video_editor::desktop_ui::AudioMixerWidget::soloToggled);
  mixer.setTracks({{.displayName = QStringLiteral("Dialogue"), .muted = true, .soloed = false},
                   {.displayName = QStringLiteral("Music"), .muted = false, .soloed = true}});
  QCOMPARE(muted.count(), 0);
  QCOMPARE(soloed.count(), 0);

  auto* first_mute = mixer.findChild<QToolButton*>(QStringLiteral("mixerMute.0"));
  auto* second_solo = mixer.findChild<QToolButton*>(QStringLiteral("mixerSolo.1"));
  auto* first_fader = mixer.findChild<QSlider*>(QStringLiteral("audioFader.0"));
  QVERIFY(first_mute != nullptr);
  QVERIFY(second_solo != nullptr);
  QVERIFY(first_fader != nullptr);
  QVERIFY(first_mute->isChecked());
  QVERIFY(second_solo->isChecked());
  QVERIFY(first_fader->isEnabled());
  QCOMPARE(first_fader->minimum(), -96);
  QCOMPARE(first_fader->maximum(), 24);

  first_mute->click();
  QCOMPARE(muted.count(), 1);
  QCOMPARE(muted.at(0).at(0).toInt(), 0);
  QVERIFY(!muted.at(0).at(1).toBool());

  mixer.setMasterMeter(-1.0F, -8.0F, -14.2, true, true, false);
  auto* loudness = mixer.findChild<QLabel*>(QStringLiteral("masterLufsMeter"));
  QVERIFY(loudness != nullptr);
  QVERIFY(loudness->text().contains(QStringLiteral("LUFS-I")));
}

void EditorWindowTest::audioMixerDisplaysStableTrackMeters() {
  video_editor::desktop_ui::AudioMixerWidget mixer;
  mixer.setTracks({{.id = QStringLiteral("track-a"), .displayName = QStringLiteral("Dialogue")},
                   {.id = QStringLiteral("track-b"), .displayName = QStringLiteral("Music")}});
  mixer.setTrackMeters({{.id = QStringLiteral("track-a"),
                         .peak_dbfs = {-3.0F, -6.0F},
                         .rms_dbfs = {-12.0F, -14.0F},
                         .active = true,
                         .stale = false},
                        {.id = QStringLiteral("track-b"),
                         .peak_dbfs = {0.0F, 0.0F},
                         .rms_dbfs = {0.0F, 0.0F},
                         .active = false,
                         .stale = false}});
  auto* dialogue = mixer.findChild<QProgressBar*>(QStringLiteral("audioMeter.0"));
  auto* music = mixer.findChild<QProgressBar*>(QStringLiteral("audioMeter.1"));
  QVERIFY(dialogue != nullptr);
  QVERIFY(music != nullptr);
  QCOMPARE(dialogue->value(), 57);
  QVERIFY(dialogue->isEnabled());
  QCOMPARE(music->value(), 0);
  QVERIFY(!music->isEnabled());
  QVERIFY(dialogue->toolTip().contains(QStringLiteral("Peak: -3.0 dBFS")));
  QVERIFY(music->toolTip().contains(QStringLiteral("inactive")));

  // A reordered view updates by stable IDs, not the prior strip index.
  mixer.setTracks({{.id = QStringLiteral("track-b"), .displayName = QStringLiteral("Music")},
                   {.id = QStringLiteral("track-a"), .displayName = QStringLiteral("Dialogue")}});
  mixer.setTrackMeters({{.id = QStringLiteral("track-a"),
                         .peak_dbfs = {-9.0F, -9.0F},
                         .rms_dbfs = {-18.0F, -18.0F},
                         .active = true,
                         .stale = false}});
  auto* reordered_dialogue = mixer.findChild<QProgressBar*>(QStringLiteral("audioMeter.1"));
  QVERIFY(reordered_dialogue != nullptr);
  QCOMPARE(reordered_dialogue->value(), 51);
  QVERIFY(reordered_dialogue->isEnabled());

  // An empty/stopped snapshot clears removed strips instead of leaving stale
  // telemetry visible.
  mixer.setTrackMeters({});
  QCOMPARE(reordered_dialogue->value(), 0);
  QVERIFY(!reordered_dialogue->isEnabled());
  QVERIFY(reordered_dialogue->toolTip().contains(QStringLiteral("inactive")));
}

void EditorWindowTest::audioMixerSetTracksKeepsStripsAliveForSameIds() {
  video_editor::desktop_ui::AudioMixerWidget mixer;
  QSignalSpy gainEdited(&mixer, &video_editor::desktop_ui::AudioMixerWidget::gainEdited);
  QSignalSpy panEdited(&mixer, &video_editor::desktop_ui::AudioMixerWidget::panEdited);
  QSignalSpy muteToggled(&mixer, &video_editor::desktop_ui::AudioMixerWidget::muteToggled);
  QSignalSpy soloToggled(&mixer, &video_editor::desktop_ui::AudioMixerWidget::soloToggled);

  mixer.setTracks({{.id = QStringLiteral("track-a"),
                    .displayName = QStringLiteral("Dialogue"),
                    .muted = false,
                    .soloed = false,
                    .gain_db = 0.0,
                    .pan = 0.0},
                   {.id = QStringLiteral("track-b"),
                    .displayName = QStringLiteral("Music"),
                    .muted = false,
                    .soloed = false,
                    .gain_db = 0.0,
                    .pan = 0.0}});

  auto* fader = mixer.findChild<QSlider*>(QStringLiteral("audioFader.0"));
  auto* pan = mixer.findChild<QSlider*>(QStringLiteral("audioPan.0"));
  auto* mute = mixer.findChild<QToolButton*>(QStringLiteral("mixerMute.0"));
  auto* solo = mixer.findChild<QToolButton*>(QStringLiteral("mixerSolo.1"));
  QVERIFY(fader != nullptr);
  QVERIFY(pan != nullptr);
  QVERIFY(mute != nullptr);
  QVERIFY(solo != nullptr);
  QPointer<QSlider> faderPtr(fader);
  QPointer<QSlider> panPtr(pan);
  QPointer<QToolButton> mutePtr(mute);
  QPointer<QToolButton> soloPtr(solo);

  mixer.setTracks({{.id = QStringLiteral("track-a"),
                    .displayName = QStringLiteral("Dialogue"),
                    .muted = true,
                    .soloed = false,
                    .gain_db = -6.0,
                    .pan = -0.5},
                   {.id = QStringLiteral("track-b"),
                    .displayName = QStringLiteral("Music"),
                    .muted = false,
                    .soloed = true,
                    .gain_db = 3.0,
                    .pan = 0.25}});

  QVERIFY(!faderPtr.isNull());
  QVERIFY(!panPtr.isNull());
  QVERIFY(!mutePtr.isNull());
  QVERIFY(!soloPtr.isNull());
  QCOMPARE(faderPtr->value(), -6);
  QCOMPARE(panPtr->value(), -50);
  QVERIFY(mutePtr->isChecked());
  QVERIFY(soloPtr->isChecked());
  QCOMPARE(gainEdited.count(), 0);
  QCOMPARE(panEdited.count(), 0);
  QCOMPARE(muteToggled.count(), 0);
  QCOMPARE(soloToggled.count(), 0);

  const QPointF localPos(faderPtr->width() / 2.0, faderPtr->height() / 2.0);
  const QPointF globalPos = faderPtr->mapToGlobal(localPos.toPoint());
  QWheelEvent wheel(localPos, globalPos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                    Qt::ScrollUpdate, false);
  QCoreApplication::sendEvent(faderPtr.data(), &wheel);

  mixer.setTracks({{.id = QStringLiteral("track-a"),
                    .displayName = QStringLiteral("Dialogue"),
                    .muted = true,
                    .soloed = false,
                    .gain_db = -3.0,
                    .pan = -0.25},
                   {.id = QStringLiteral("track-b"),
                    .displayName = QStringLiteral("Music"),
                    .muted = false,
                    .soloed = true,
                    .gain_db = 6.0,
                    .pan = 0.5}});
  QVERIFY(!faderPtr.isNull());
  QCOMPARE(faderPtr->value(), -3);
  QCOMPARE(gainEdited.count(), 1);
  QCOMPARE(panEdited.count(), 0);
  QCOMPARE(muteToggled.count(), 0);
  QCOMPARE(soloToggled.count(), 0);
}

void EditorWindowTest::audioMixerShowsSystemDefaultAndAuthoritativeLufsStates() {
  video_editor::desktop_ui::AudioMixerWidget mixer;
  mixer.setOutputDevices({QStringLiteral("speaker")}, {QStringLiteral("Speaker")}, {}, true,
                         QStringLiteral("Ready"));
  auto* selector = mixer.findChild<QComboBox*>(QStringLiteral("audioOutputDevice"));
  QVERIFY(selector != nullptr);
  QCOMPARE(selector->itemData(0).toString(), QString{});
  QCOMPARE(selector->itemText(0), QStringLiteral("System default"));
  mixer.setMasterMeter(-1.0F, -8.0F, -14.2, true, true, false);
  auto* lufs = mixer.findChild<QLabel*>(QStringLiteral("masterLufsMeter"));
  QVERIFY(lufs != nullptr);
  QVERIFY(lufs->text().contains(QStringLiteral("LUFS-I")));
  QVERIFY(lufs->text().contains(QStringLiteral("-14.2")));
  mixer.setMasterMeter(-1.0F, -8.0F, -14.2, true, false, true);
  QVERIFY(lufs->text().contains(QStringLiteral("stale")));
}

void EditorWindowTest::captionsPanelEmitsEditableCueActions() {
  video_editor::desktop_ui::CaptionsPanelWidget captions;
  auto* table = captions.findChild<QTableWidget*>(QStringLiteral("captionsTable"));
  auto* add = captions.findChild<QPushButton*>(QStringLiteral("addCaptionButton"));
  auto* remove = captions.findChild<QPushButton*>(QStringLiteral("removeCaptionButton"));
  QVERIFY(table != nullptr);
  QVERIFY(add != nullptr);
  QVERIFY(remove != nullptr);
  QSignalSpy edited(&captions, &video_editor::desktop_ui::CaptionsPanelWidget::captionTextEdited);
  QSignalSpy added(&captions, &video_editor::desktop_ui::CaptionsPanelWidget::addCaptionRequested);
  QSignalSpy removed(&captions,
                     &video_editor::desktop_ui::CaptionsPanelWidget::removeCaptionRequested);

  captions.setCaptionRows({QStringLiteral("00:00:00:00 → 00:00:01:00")},
                          {QStringLiteral("Original")});
  QCOMPARE(edited.count(), 0);
  QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEditable));
  QVERIFY(table->item(0, 1)->flags() & Qt::ItemIsEditable);
  table->item(0, 1)->setText(QStringLiteral("Updated caption"));
  QCOMPARE(edited.count(), 1);
  QCOMPARE(edited.at(0).at(0).toInt(), 0);
  QCOMPARE(edited.at(0).at(1).toString(), QStringLiteral("Updated caption"));

  add->click();
  QCOMPARE(added.count(), 1);
  table->selectRow(0);
  remove->click();
  QCOMPARE(removed.count(), 1);
  QCOMPARE(removed.at(0).at(0).toInt(), 0);
}

void EditorWindowTest::captionsPanelExposesTranscriptionAndWordNavigation() {
  video_editor::desktop_ui::CaptionsPanelWidget captions;
  const video_editor::desktop_ui::CaptionRowView row{
      .id = QStringLiteral("caption-id"),
      .timecode = QStringLiteral("00:00:00:00 → 00:00:01:00"),
      .text = QStringLiteral("Hello"),
      .start = 0,
      .end = 48'000,
      .words = {{QStringLiteral("word-id"), QStringLiteral("Hello"), 240, 720, 0.91}}};
  captions.setCaptionRows({row});
  auto* languages = captions.findChild<QComboBox*>(QStringLiteral("transcriptionLanguage"));
  QVERIFY(languages != nullptr);
  QCOMPARE(languages->findData(QStringLiteral("en")) >= 0, true);
  auto* words = captions.findChild<QListWidget*>(QStringLiteral("captionWordsList"));
  QVERIFY(words != nullptr);
  auto* timings = captions.findChild<QCheckBox*>(QStringLiteral("transcriptionWordTimings"));
  QVERIFY(timings != nullptr);
  QVERIFY(timings->isChecked());
  QVERIFY(!timings->isEnabled());
  QCOMPARE(words->count(), 1);
  QSignalSpy wordActivated(&captions,
                           &video_editor::desktop_ui::CaptionsPanelWidget::wordActivated);
  words->setCurrentRow(0);
  QTest::keyClick(words, Qt::Key_Return);
  QCOMPARE(wordActivated.size(), 1);
  QCOMPARE(wordActivated.at(0).at(0).toString(), QStringLiteral("word-id"));
  QCOMPARE(wordActivated.at(0).at(1).toLongLong(), 240LL);

  captions.setTranscriptionState(video_editor::desktop_ui::TranscriptionState::Downloading,
                                 QStringLiteral("Downloading"), 30);
  auto* progress = captions.findChild<QProgressBar*>(QStringLiteral("transcriptionProgress"));
  QVERIFY(progress != nullptr);
  QCOMPARE(progress->value(), 30);
  captions.setTranscriptionState(video_editor::desktop_ui::TranscriptionState::Ready,
                                 QStringLiteral("Ready"), 100);
  auto* transcribe = captions.findChild<QPushButton*>(QStringLiteral("transcribeButton"));
  QVERIFY(transcribe != nullptr);
  QVERIFY(transcribe->isEnabled());

  captions.setReviewProposals({
      {QStringLiteral("silence-1"), QStringLiteral("Measured silence"),
       QStringLiteral("Remove silent range"), QStringLiteral("00:00:01 → 00:00:02"),
       QStringLiteral("Measured from rendered audio"), true, false, false},
      {QStringLiteral("filler-1"), QStringLiteral("Transcript filler"),
       QStringLiteral("Remove filler um"), QStringLiteral("00:00:02 → 00:00:02.2"),
       QStringLiteral("Off by default"), false, false, false},
  });
  auto* review = captions.findChild<QListWidget*>(QStringLiteral("captionReviewList"));
  QVERIFY(review != nullptr);
  QCOMPARE(review->count(), 2);
  QCOMPARE(review->item(0)->checkState(), Qt::Checked);
  QCOMPARE(review->item(1)->checkState(), Qt::Unchecked);
  QVERIFY(review->item(0)->text().contains(QStringLiteral("Measured silence")));
  QVERIFY(review->item(1)->text().contains(QStringLiteral("Transcript filler")));
}

void EditorWindowTest::timelineVirtualizesAndSeeks() {
  TimelineWidget timeline;
  timeline.resize(900, 420);

  QVector<TimelineTrackView> tracks;
  QVector<TimelineClipView> clips;
  for (int index = 0; index < 100; ++index) {
    tracks.append({QStringLiteral("track-%1").arg(index), QStringLiteral("Track %1").arg(index),
                   index < 30 ? TrackKind::Video : TrackKind::Audio});
    for (int clip = 0; clip < 10; ++clip) {
      clips.append({QStringLiteral("clip-%1-%2").arg(index).arg(clip),
                    QStringLiteral("Clip %1").arg(clip), index, clip * 48'000LL, 40'000LL});
    }
  }
  timeline.setTimeline(10 * 48'000LL, 48'000, tracks, clips);
  timeline.show();
  QCoreApplication::processEvents();
  timeline.viewport()->update();
  QCoreApplication::processEvents();

  QVERIFY(timeline.visibleClipCount() > 0);
  QVERIFY(timeline.visibleClipCount() < clips.size());
  QSignalSpy seek(&timeline, &TimelineWidget::seekRequested);
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint{420, 12});
  QVERIFY(seek.count() >= 1);
  QVERIFY(timeline.playhead() > 0);

  timeline.setPixelsPerSecond(1.0);
  QCOMPARE(timeline.pixelsPerSecond(), 8.0);
  timeline.setPixelsPerSecond(10'000.0);
  QCOMPARE(timeline.pixelsPerSecond(), 2'400.0);
}

void EditorWindowTest::timelineEmitsTypedMovePreviewAndCommit() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  QSignalSpy previews(&timeline, &TimelineWidget::clipEditPreview);
  QSignalSpy commits(&timeline, &TimelineWidget::clipEditCommitted);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {430, 118}, Qt::NoButton, Qt::LeftButton,
              Qt::ControlModifier | Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {430, 118}, Qt::LeftButton, Qt::NoButton,
              Qt::ControlModifier | Qt::ShiftModifier);

  QVERIFY(previews.count() >= 1);
  QCOMPARE(commits.count(), 1);
  const auto payload = commits.takeFirst();
  QCOMPARE(payload.at(0).toString(), QStringLiteral("clip-a"));
  QCOMPARE(payload.at(1).toInt(), 1);
  QCOMPARE(payload.at(2).toLongLong(), 800);
  QCOMPARE(payload.at(3).toLongLong(), 0);
  QCOMPARE(editMode(payload), TimelineWidget::EditMode::Move);
  QCOMPARE(editIntent(payload), TimelineWidget::EditIntent::Ripple);
  QVERIFY(!payload.at(6).toBool());
  QCOMPARE(timeline.clips().at(0).start, 1'000); // The widget never mutates the edit model.
}

void EditorWindowTest::timelineDistinguishesTrimRegions() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  QCOMPARE(timeline.clipHitRegionAt({278, 60}), TimelineWidget::ClipHitRegion::TrimIn);
  QCOMPARE(timeline.clipHitRegionAt({350, 60}), TimelineWidget::ClipHitRegion::Body);
  QCOMPARE(timeline.clipHitRegionAt({473, 60}), TimelineWidget::ClipHitRegion::TrimOut);
  QCOMPARE(timeline.clipHitRegionAt({530, 60}), TimelineWidget::ClipHitRegion::None);

  QSignalSpy commits(&timeline, &TimelineWidget::clipEditCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {278, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {328, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {328, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
  QCOMPARE(commits.count(), 1);
  auto payload = commits.takeFirst();
  QCOMPARE(payload.at(2).toLongLong(), 500);
  QCOMPARE(payload.at(3).toLongLong(), -500);
  QCOMPARE(editMode(payload), TimelineWidget::EditMode::TrimIn);

  sendPointer(timeline, QEvent::MouseButtonPress, {473, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {523, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::AltModifier | Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {523, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::AltModifier | Qt::ShiftModifier);
  QCOMPARE(commits.count(), 1);
  payload = commits.takeFirst();
  QCOMPARE(payload.at(2).toLongLong(), 0);
  QCOMPARE(payload.at(3).toLongLong(), 500);
  QCOMPARE(editMode(payload), TimelineWidget::EditMode::TrimOut);
  QCOMPARE(editIntent(payload), TimelineWidget::EditIntent::Overwrite);
}

void EditorWindowTest::timelineSnapsAndShiftDisablesSnap() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  timeline.setPlayhead(8'000);
  timeline.setSnapThresholdPixels(8);
  timeline.setSnapResolver([](const video_editor::desktop_ui::TimelineSnapRequest& request) {
    return video_editor::desktop_ui::TimelineSnapResult{
        .time = 2'000,
        .kind = video_editor::desktop_ui::TimelineSnapKind::ClipEdge,
        .label = QStringLiteral("Clip edge")};
  });
  QCOMPARE(timeline.snapThresholdPixels(), 8);
  QSignalSpy commits(&timeline, &TimelineWidget::clipEditCommitted);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {448, 60}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {448, 60}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(commits.count(), 1);
  auto payload = commits.takeFirst();
  QCOMPARE(payload.at(2).toLongLong(), 1'000);
  QVERIFY(payload.at(6).toBool());

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {448, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {448, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
  QCOMPARE(commits.count(), 1);
  payload = commits.takeFirst();
  QCOMPARE(payload.at(2).toLongLong(), 980);
  QVERIFY(!payload.at(6).toBool());
}

void EditorWindowTest::timelineCancelsAndDoesNotCommitClicks() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  QSignalSpy previews(&timeline, &TimelineWidget::clipEditPreview);
  QSignalSpy commits(&timeline, &TimelineWidget::clipEditCommitted);
  QSignalSpy canceled(&timeline, &TimelineWidget::clipEditCanceled);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {350, 60}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(previews.count(), 0);
  QCOMPARE(commits.count(), 0);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {410, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  QVERIFY(previews.count() > 0);
  QTest::keyClick(&timeline, Qt::Key_Escape);
  QCOMPARE(commits.count(), 0);
  QCOMPARE(canceled.count(), 1);
  QCOMPARE(canceled.takeFirst().at(0).toString(), QStringLiteral("clip-a"));
}

void EditorWindowTest::timelineHandlesScrolledCoordinatesAndFrameNudges() {
  TimelineWidget timeline;
  QVector<TimelineClipView> clips{{QStringLiteral("near"), QStringLiteral("Near"), 0, 1'000, 1'000},
                                  {QStringLiteral("scrolled"), QStringLiteral("Scrolled"), 1,
                                   35'000, 2'000, QColor{82, 126, 183}, true}};
  timeline.resize(900, 180);
  timeline.setPixelsPerSecond(100.0);
  timeline.setTimeline(100'000, 1'000,
                       {{QStringLiteral("v1"), QStringLiteral("V1"), TrackKind::Video},
                        {QStringLiteral("v2"), QStringLiteral("V2"), TrackKind::Video},
                        {QStringLiteral("a1"), QStringLiteral("A1"), TrackKind::Audio},
                        {QStringLiteral("a2"), QStringLiteral("A2"), TrackKind::Audio}},
                       clips);
  timeline.show();
  QCoreApplication::processEvents();
  timeline.horizontalScrollBar()->setValue(3'000);
  timeline.verticalScrollBar()->setValue(58);

  QCOMPARE(timeline.clipHitRegionAt({350, 60}), TimelineWidget::ClipHitRegion::None);
  QCOMPARE(timeline.clipHitRegionAt({750, 60}), TimelineWidget::ClipHitRegion::Body);
  QSignalSpy commits(&timeline, &TimelineWidget::clipEditCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {750, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {800, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {800, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
  QCOMPARE(commits.count(), 1);
  QCOMPARE(commits.takeFirst().at(2).toLongLong(), 500);

  TimelineWidget nudgeTimeline;
  QVector<TimelineClipView> selected{{QStringLiteral("selected"), QStringLiteral("Selected"), 0,
                                      48'000, 96'000, QColor{82, 126, 183}, true}};
  nudgeTimeline.resize(700, 180);
  nudgeTimeline.setTimeline(
      480'000, 48'000, {{QStringLiteral("v1"), QStringLiteral("V1"), TrackKind::Video}}, selected);
  nudgeTimeline.setFrameRate(24'000, 1'001);
  QCOMPARE(nudgeTimeline.frameRateNumerator(), quint32{24'000});
  QCOMPARE(nudgeTimeline.frameRateDenominator(), quint32{1'001});
  QCOMPARE(nudgeTimeline.frameStep(), 2'002);
  QSignalSpy nudgeCommits(&nudgeTimeline, &TimelineWidget::clipEditCommitted);
  nudgeTimeline.nudgeActiveClipByFrames(1, TimelineWidget::EditIntent::Overwrite);
  QCOMPARE(nudgeCommits.count(), 1);
  const auto nudge = nudgeCommits.takeFirst();
  QCOMPARE(nudge.at(2).toLongLong(), 2'002);
  QCOMPARE(editIntent(nudge), TimelineWidget::EditIntent::Overwrite);
  QCOMPARE(nudgeTimeline.clips().at(0).start, 48'000);
}

void EditorWindowTest::timelineSupportsMultiSelectionAndToolModes() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline,
                               {{QStringLiteral("clip-a"), QStringLiteral("A"), 0, 1'000, 1'000},
                                {QStringLiteral("clip-b"), QStringLiteral("B"), 0, 3'000, 1'000},
                                {QStringLiteral("clip-c"), QStringLiteral("C"), 0, 5'000, 1'000}});
  QSignalSpy selection(&timeline, &TimelineWidget::clipSelectionChanged);
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {325, 60});
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::ControlModifier, {725, 60});
  QCOMPARE(timeline.selectedClipIds(),
           QStringList({QStringLiteral("clip-a"), QStringLiteral("clip-c")}));
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::ShiftModifier, {325, 60});
  QCOMPARE(
      timeline.selectedClipIds(),
      QStringList({QStringLiteral("clip-a"), QStringLiteral("clip-b"), QStringLiteral("clip-c")}));
  QVERIFY(selection.count() >= 3);

  timeline.setToolMode(TimelineWidget::ToolMode::Slip);
  QCOMPARE(timeline.toolMode(), TimelineWidget::ToolMode::Slip);
  QSignalSpy commits(&timeline, &TimelineWidget::clipBatchEditCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {325, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {365, 60}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {365, 60}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(commits.count(), 1);
  QCOMPARE(commits.at(0).at(4).value<TimelineWidget::EditMode>(), TimelineWidget::EditMode::Slip);
}

void EditorWindowTest::timelineUsesCanonicalSnapResolver() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  int resolverCalls = 0;
  timeline.setSnapResolver(
      [&resolverCalls](const video_editor::desktop_ui::TimelineSnapRequest& request) {
        ++resolverCalls;
        Q_ASSERT(request.excludedClipIds.contains(QStringLiteral("clip-a")));
        return video_editor::desktop_ui::TimelineSnapResult{
            .time = 2'000,
            .kind = video_editor::desktop_ui::TimelineSnapKind::Marker,
            .label = QStringLiteral("Marker")};
      });
  QSignalSpy committed(&timeline, &TimelineWidget::clipBatchEditCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {448, 60}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {448, 60}, Qt::LeftButton, Qt::NoButton);
  QVERIFY(resolverCalls > 0);
  QCOMPARE(committed.count(), 1);
  QVERIFY(committed.at(0).at(6).value<video_editor::desktop_ui::TimelineSnapResult>().snapped());
  resolverCalls = 0;
  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {448, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {448, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);
  QCOMPARE(resolverCalls, 0);
}

void EditorWindowTest::timelineExposesMarkerGapAndTrackCommands() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  timeline.setMarkers({{QStringLiteral("m1"), QStringLiteral("Intro"), 2'000}});
  timeline.setGaps({{QStringLiteral("gap-v1-4000"), QStringLiteral("video-1"), 0, 4'000, 500}});
  QSignalSpy markerAdded(&timeline, &TimelineWidget::markerAddRequested);
  QSignalSpy closeGap(&timeline, &TimelineWidget::closeGapRequested);
  QSignalSpy locked(&timeline, &TimelineWidget::trackLockToggled);
  QTest::mouseDClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {600, 12});
  QCOMPARE(markerAdded.count(), 1);
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {600, 60});
  QTest::keyClick(&timeline, Qt::Key_Delete);
  QCOMPARE(closeGap.count(), 1);
  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {125, 48});
  QCOMPARE(locked.count(), 1);
  QVERIFY(!timeline.accessibleName().isEmpty());
}

void EditorWindowTest::timelineRequiresClipEdgesForTrimTools() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  timeline.setToolMode(TimelineWidget::ToolMode::RippleTrim);
  QSignalSpy committed(&timeline, &TimelineWidget::clipBatchEditCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {420, 60}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {420, 60}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 0);

  sendPointer(timeline, QEvent::MouseButtonPress, {276, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {320, 60}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {320, 60}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 1);
  QCOMPARE(committed.at(0).at(5).value<TimelineWidget::EditIntent>(),
           TimelineWidget::EditIntent::Ripple);
}

void EditorWindowTest::timelineKeepsSlipAndSlideOnClipBodies() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  QSignalSpy committed(&timeline, &TimelineWidget::clipBatchEditCommitted);

  timeline.setToolMode(TimelineWidget::ToolMode::Slip);
  sendPointer(timeline, QEvent::MouseButtonPress, {278, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {328, 118}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {328, 118}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 0);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {400, 118}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {400, 118}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 1);
  auto payload = committed.takeFirst();
  QCOMPARE(payload.at(1).toInt(), 0);
  QCOMPARE(payload.at(4).value<TimelineWidget::EditMode>(), TimelineWidget::EditMode::Slip);

  timeline.setToolMode(TimelineWidget::ToolMode::Slide);
  sendPointer(timeline, QEvent::MouseButtonPress, {473, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {523, 118}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {523, 118}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 0);

  sendPointer(timeline, QEvent::MouseButtonPress, {350, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {400, 118}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {400, 118}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 1);
  payload = committed.takeFirst();
  QCOMPARE(payload.at(1).toInt(), 0);
  QCOMPARE(payload.at(4).value<TimelineWidget::EditMode>(), TimelineWidget::EditMode::Slide);
}

void EditorWindowTest::timelineRollUsesTheControllerBoundaryConvention() {
  TimelineWidget timeline;
  configureInteractiveTimeline(
      timeline, {{QStringLiteral("left"), QStringLiteral("Left"), 0, 1'000, 2'000},
                 {QStringLiteral("right"), QStringLiteral("Right"), 0, 3'000, 2'000}});
  timeline.setToolMode(TimelineWidget::ToolMode::Roll);
  QSignalSpy committed(&timeline, &TimelineWidget::clipBatchEditCommitted);

  // Even though the pointer starts at Left's incoming edge, the controller
  // chooses its outgoing adjacent cut. The UI delta must use that same cut.
  sendPointer(timeline, QEvent::MouseButtonPress, {278, 60}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {378, 60}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  sendPointer(timeline, QEvent::MouseButtonRelease, {378, 60}, Qt::LeftButton, Qt::NoButton,
              Qt::ShiftModifier);

  QCOMPARE(committed.count(), 1);
  const auto payload = committed.takeFirst();
  QCOMPARE(payload.at(2).toLongLong(), 1'000);
  QCOMPARE(payload.at(4).value<TimelineWidget::EditMode>(), TimelineWidget::EditMode::Roll);
}

void EditorWindowTest::timelineMarkerSnappingExcludesTheDraggedMarker() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  timeline.setMarkers({{QStringLiteral("marker-1"), QStringLiteral("Intro"), 2'000}});
  int resolverCalls = 0;
  bool sawDraggedMarkerExclusion = false;
  bool sawEmptyMarkerExclusion = false;
  timeline.setSnapResolver([&resolverCalls, &sawDraggedMarkerExclusion, &sawEmptyMarkerExclusion](
                               const video_editor::desktop_ui::TimelineSnapRequest& request) {
    ++resolverCalls;
    if (!request.excludedMarkerId.isEmpty()) {
      sawDraggedMarkerExclusion =
          request.forMarker && request.excludedMarkerId == QStringLiteral("marker-1");
    } else {
      sawEmptyMarkerExclusion = request.forMarker && request.excludedMarkerId.isEmpty();
    }
    return video_editor::desktop_ui::TimelineSnapResult{
        .time = 2'500,
        .kind = video_editor::desktop_ui::TimelineSnapKind::Frame,
        .label = QStringLiteral("Frame")};
  });
  QSignalSpy committed(&timeline, &TimelineWidget::markerMoveCommitted);

  sendPointer(timeline, QEvent::MouseButtonPress, {376, 5}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {456, 5}, Qt::NoButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseButtonRelease, {456, 5}, Qt::LeftButton, Qt::NoButton);

  QVERIFY(resolverCalls > 0);
  QVERIFY(sawDraggedMarkerExclusion);
  QCOMPARE(committed.count(), 1);
  QCOMPARE(committed.takeFirst().at(1).toLongLong(), 2'500);

  QSignalSpy added(&timeline, &TimelineWidget::markerAddRequested);
  QTest::mouseDClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {576, 12});
  QCOMPARE(added.count(), 1);
  QCOMPARE(added.takeFirst().at(0).toLongLong(), 2'500);
  QVERIFY(sawEmptyMarkerExclusion);
}

void EditorWindowTest::timelineRefreshCancelsMarkerGesturesAndUsesAuthoritativeSelection() {
  TimelineWidget timeline;
  configureInteractiveTimeline(timeline);
  timeline.setMarkers({{QStringLiteral("marker-1"), QStringLiteral("Intro"), 2'000}});
  QSignalSpy canceled(&timeline, &TimelineWidget::markerMoveCanceled);
  QSignalSpy committed(&timeline, &TimelineWidget::markerMoveCommitted);
  sendPointer(timeline, QEvent::MouseButtonPress, {376, 5}, Qt::LeftButton, Qt::LeftButton);
  sendPointer(timeline, QEvent::MouseMove, {426, 5}, Qt::NoButton, Qt::LeftButton,
              Qt::ShiftModifier);
  timeline.setMarkers({{QStringLiteral("marker-2"), QStringLiteral("Outro"), 4'000}});
  QCOMPARE(canceled.count(), 1);
  sendPointer(timeline, QEvent::MouseButtonRelease, {426, 5}, Qt::LeftButton, Qt::NoButton);
  QCOMPARE(committed.count(), 0);

  QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier, {350, 60});
  QVERIFY(!timeline.selectedClipIds().isEmpty());
  timeline.setClips({{QStringLiteral("clip-a"), QStringLiteral("Clip A"), 0, 1'000, 2'000}});
  QVERIFY(timeline.selectedClipIds().isEmpty());
  QVERIFY(timeline.activeClipId().isEmpty());
}

void EditorWindowTest::timelineCanCreateTracksWithoutAnExistingTrack() {
  TimelineWidget timeline;
  timeline.resize(500, 180);
  timeline.setTimeline(10'000, 1'000, {}, {});
  QSignalSpy added(&timeline, &TimelineWidget::trackAddRequested);

  QTest::keyClick(&timeline, Qt::Key_Insert);
  QTest::keyClick(&timeline, Qt::Key_Insert, Qt::ShiftModifier);
  QTest::keyClick(&timeline, Qt::Key_Insert, Qt::ControlModifier);

  QCOMPARE(added.count(), 3);
  QCOMPARE(added.at(0).at(0).value<TrackKind>(), TrackKind::Video);
  QCOMPARE(added.at(1).at(0).value<TrackKind>(), TrackKind::Audio);
  QCOMPARE(added.at(2).at(0).value<TrackKind>(), TrackKind::Caption);
}

void EditorWindowTest::deliverPanelShowsCancelableProgress() {
  QTemporaryDir directory;
  auto settings = temporarySettings(directory);
  EditorWindow window(settings.get());
  auto* panel = window.deliverPanel();
  auto* progress = panel->findChild<QProgressBar*>(QStringLiteral("exportProgress"));
  auto* button = panel->findChild<QToolButton*>(QStringLiteral("exportButton"));
  QVERIFY(progress != nullptr);
  QVERIFY(button != nullptr);

  panel->setExportRunning(true, 37);
  QVERIFY(!progress->isHidden());
  QCOMPARE(progress->value(), 37);
  QCOMPARE(button->text(), QStringLiteral("Cancel export"));

  panel->setExportRunning(false);
  QVERIFY(progress->isHidden());
  QCOMPARE(button->text(), QStringLiteral("Export master"));
}

QTEST_MAIN(EditorWindowTest)
#include "editor_window_test.moc"
