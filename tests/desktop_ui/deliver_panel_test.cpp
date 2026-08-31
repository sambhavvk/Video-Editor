// SPDX-License-Identifier: MPL-2.0
#include "video_editor/desktop_ui/panel_widgets.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QToolButton>
#include <QTest>

using video_editor::desktop_ui::DeliverPanelWidget;

class DeliverPanelWidgetTest final : public QObject {
  Q_OBJECT

private slots:
  void loadsAllPlatformPresets();
  void updatesPresetNotesWhenPresetChanges();
  void usesCreatorReadyDefaults();
  void exposesRunningStateAndSummaries();
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

QTEST_MAIN(DeliverPanelWidgetTest)

#include "deliver_panel_test.moc"
