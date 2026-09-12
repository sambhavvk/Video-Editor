// SPDX-License-Identifier: MPL-2.0
#include "video_editor/desktop_ui/panel_widgets.hpp"

#include "video_editor/export_service/presets.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QTest>

using video_editor::desktop_ui::DeliverPanelWidget;
using video_editor::desktop_ui::ExportJobStateView;
using video_editor::desktop_ui::ExportJobView;

class DeliverPanelWidgetTest final : public QObject {
  Q_OBJECT

private slots:
  void loadsAllPlatformPresets();
  void updatesPresetNotesWhenPresetChanges();
  void usesCreatorReadyDefaults();
  void exposesRunningStateAndSummaries();
  void setExportJobsPopulatesQueueList();
  void deliveryOverviewReportsPresetTargetSize();
  void deliveryRecipeSnapshotRoundTripsAndIgnoresInvalidIndexes();
};

void DeliverPanelWidgetTest::loadsAllPlatformPresets() {
  DeliverPanelWidget panel;
  panel.loadPlatformPresets();

  const auto* preset = panel.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  QVERIFY(preset != nullptr);
  QCOMPARE(preset->count(), 8);
  QVERIFY(!panel.selectedPresetId().isEmpty());
}

void DeliverPanelWidgetTest::updatesPresetNotesWhenPresetChanges() {
  DeliverPanelWidget panel;
  auto* preset = panel.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  auto* notes = panel.findChild<QLabel*>(QStringLiteral("presetNotes"));
  QVERIFY(preset != nullptr);
  QVERIFY(notes != nullptr);

  preset->setCurrentIndex(1);
  QVERIFY(!notes->text().isEmpty());
}

void DeliverPanelWidgetTest::usesCreatorReadyDefaults() {
  DeliverPanelWidget panel;

  QCOMPARE(panel.captionModeKey(), QStringLiteral("none"));
  QCOMPARE(panel.creatorVideoCodecKey(), QStringLiteral("vp9"));
  QCOMPARE(panel.sidecarFormatKey(), QStringLiteral("srt"));
  QCOMPARE(panel.overrideWidth(), 0);
  QCOMPARE(panel.overrideHeight(), 0);
  QCOMPARE(panel.overrideFrameRateNum(), 0u);
  QCOMPARE(panel.overrideFrameRateDen(), 0u);
  QCOMPARE(panel.overrideAudioBitrate(), 0u);
}

void DeliverPanelWidgetTest::exposesRunningStateAndSummaries() {
  DeliverPanelWidget panel;
  panel.show();
  QApplication::processEvents();
  auto* progress = panel.findChild<QProgressBar*>(QStringLiteral("exportProgress"));
  auto* button = panel.findChild<QToolButton*>(QStringLiteral("exportButton"));
  auto* encoder = panel.findChild<QLabel*>(QStringLiteral("encoderSummary"));
  auto* destination = panel.findChild<QLineEdit*>(QStringLiteral("destinationField"));
  QVERIFY(progress != nullptr);
  QVERIFY(button != nullptr);
  QVERIFY(encoder != nullptr);
  QVERIFY(destination != nullptr);

  panel.setEncoderCapabilities(QStringLiteral("test summary"));
  panel.setDestinationPath(QStringLiteral("/tmp/test.mp4"));
  panel.setExportRunning(true, 50);

  QCOMPARE(encoder->text(), QStringLiteral("test summary"));
  QCOMPARE(destination->text(), QStringLiteral("/tmp/test.mp4"));
  QVERIFY(progress->isVisible());
  QCOMPARE(progress->value(), 50);
  QCOMPARE(button->text(), QStringLiteral("Cancel export"));
}

void DeliverPanelWidgetTest::setExportJobsPopulatesQueueList() {
  DeliverPanelWidget panel;
  panel.show();
  QApplication::processEvents();

  auto* list = panel.findChild<QListWidget*>(QStringLiteral("exportJobList"));
  auto* remove_button = panel.findChild<QPushButton*>(QStringLiteral("removeQueuedExportButton"));
  QVERIFY(list != nullptr);
  QVERIFY(remove_button != nullptr);

  QVector<ExportJobView> jobs;
  ExportJobView running;
  running.id = QStringLiteral("running-job");
  running.destinationDisplay = QStringLiteral("/tmp/running.mkv");
  running.presetLabel = QStringLiteral("master.ffv1");
  running.state = ExportJobStateView::Running;
  running.progressPercent = 42;
  jobs.push_back(running);

  ExportJobView queued;
  queued.id = QStringLiteral("queued-job");
  queued.destinationDisplay = QStringLiteral("/tmp/queued.mkv");
  queued.presetLabel = QStringLiteral("master.ffv1");
  queued.state = ExportJobStateView::Queued;
  jobs.push_back(queued);
  panel.setExportJobs(jobs);

  QCOMPARE(list->count(), 2);
  QVERIFY(list->parentWidget()->isVisible());
  list->setCurrentRow(1);
  QApplication::processEvents();
  QVERIFY(remove_button->isEnabled());
  QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), QStringLiteral("queued-job"));
}

void DeliverPanelWidgetTest::deliveryOverviewReportsPresetTargetSize() {
  DeliverPanelWidget panel;
  auto* preset = panel.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  QVERIFY(preset != nullptr);

  QVERIFY(panel.deliveryOverviewText().contains(QStringLiteral("Size: sequence")));

  const int youtube = preset->findData(
      QString::number(static_cast<int>(video_editor::export_service::PlatformPreset::YouTube1080p)));
  QVERIFY(youtube >= 0);
  preset->setCurrentIndex(youtube);
  const QString overview = panel.deliveryOverviewText();
  QVERIFY(overview.contains(QStringLiteral("Size: 1920×1080")));
  QVERIFY(!overview.contains(QStringLiteral("Size: sequence")));
  QVERIFY(overview.contains(QStringLiteral("Video: VP9")));
  QVERIFY(overview.contains(QStringLiteral("Color: Rec.709 SDR limited")));
}

void DeliverPanelWidgetTest::deliveryRecipeSnapshotRoundTripsAndIgnoresInvalidIndexes() {
  DeliverPanelWidget panel;
  auto* resolution = panel.findChild<QComboBox*>(QStringLiteral("resolutionCombo"));
  auto* captions = panel.findChild<QComboBox*>(QStringLiteral("captionModeCombo"));
  auto* sidecar = panel.findChild<QComboBox*>(QStringLiteral("sidecarFormatCombo"));
  QVERIFY(resolution != nullptr);
  QVERIFY(captions != nullptr);
  QVERIFY(sidecar != nullptr);

  panel.setDestinationPath(QStringLiteral("/tmp/recipe.webm"));
  resolution->setCurrentIndex(1);
  captions->setCurrentIndex(captions->findData(QStringLiteral("sidecar")));
  sidecar->setCurrentIndex(sidecar->findData(QStringLiteral("vtt")));

  video_editor::desktop_ui::DeliveryRecipeSnapshot snapshot = panel.captureRecipeSnapshot();
  QCOMPARE(snapshot.destination, QStringLiteral("/tmp/recipe.webm"));
  QCOMPARE(snapshot.resolution_index, 1);
  QCOMPARE(snapshot.sidecar_format, QStringLiteral("vtt"));

  snapshot.resolution_index = 99;
  snapshot.caption_mode_index = -1;
  panel.applyRecipeSnapshot(snapshot);
  QCOMPARE(panel.overrideWidth(), 1920);
  QCOMPARE(panel.overrideHeight(), 1080);
  QCOMPARE(panel.captionModeKey(), QStringLiteral("sidecar"));
  QCOMPARE(panel.sidecarFormatKey(), QStringLiteral("vtt"));
  QCOMPARE(panel.destinationPath(), QStringLiteral("/tmp/recipe.webm"));
}

QTEST_MAIN(DeliverPanelWidgetTest)

#include "deliver_panel_test.moc"
