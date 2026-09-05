// SPDX-License-Identifier: MPL-2.0
#include "video_editor/desktop_ui/panel_widgets.hpp"

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

QTEST_MAIN(DeliverPanelWidgetTest)

#include "deliver_panel_test.moc"
