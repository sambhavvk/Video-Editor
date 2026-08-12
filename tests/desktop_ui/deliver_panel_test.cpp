// SPDX-License-Identifier: MPL-2.0
#include "video_editor/desktop_ui/panel_widgets.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QToolButton>

#include <gtest/gtest.h>

namespace {

int argc = 1;
char applicationName[] = "deliver_panel_test";
char* argv[] = {applicationName, nullptr};
QApplication application(argc, argv);

using video_editor::desktop_ui::DeliverPanelWidget;

TEST(DeliverPanelWidgetTest, LoadsAllPlatformPresets) {
  DeliverPanelWidget panel;
  panel.loadPlatformPresets();

  const auto* preset = panel.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  ASSERT_NE(preset, nullptr);
  EXPECT_EQ(preset->count(), 8);
  EXPECT_FALSE(panel.selectedPresetId().isEmpty());
}

TEST(DeliverPanelWidgetTest, UpdatesPresetNotesWhenPresetChanges) {
  DeliverPanelWidget panel;
  auto* preset = panel.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  auto* notes = panel.findChild<QLabel*>(QStringLiteral("presetNotes"));
  ASSERT_NE(preset, nullptr);
  ASSERT_NE(notes, nullptr);

  preset->setCurrentIndex(1);
  EXPECT_FALSE(notes->text().isEmpty());
}

TEST(DeliverPanelWidgetTest, UsesCreatorReadyDefaults) {
  DeliverPanelWidget panel;

  EXPECT_EQ(panel.captionModeKey(), QStringLiteral("none"));
  EXPECT_EQ(panel.sidecarFormatKey(), QStringLiteral("srt"));
  EXPECT_EQ(panel.overrideWidth(), 0);
  EXPECT_EQ(panel.overrideHeight(), 0);
  EXPECT_EQ(panel.overrideFrameRateNum(), 0u);
  EXPECT_EQ(panel.overrideFrameRateDen(), 0u);
  EXPECT_EQ(panel.overrideAudioBitrate(), 0u);
}

TEST(DeliverPanelWidgetTest, ExposesRunningStateAndSummaries) {
  DeliverPanelWidget panel;
  panel.show();
  QApplication::processEvents();
  auto* progress = panel.findChild<QProgressBar*>(QStringLiteral("exportProgress"));
  auto* button = panel.findChild<QToolButton*>(QStringLiteral("exportButton"));
  auto* encoder = panel.findChild<QLabel*>(QStringLiteral("encoderSummary"));
  auto* destination = panel.findChild<QLineEdit*>(QStringLiteral("destinationField"));
  ASSERT_NE(progress, nullptr);
  ASSERT_NE(button, nullptr);
  ASSERT_NE(encoder, nullptr);
  ASSERT_NE(destination, nullptr);

  panel.setEncoderCapabilities(QStringLiteral("test summary"));
  panel.setDestinationPath(QStringLiteral("/tmp/test.mp4"));
  panel.setExportRunning(true, 50);

  EXPECT_EQ(encoder->text(), QStringLiteral("test summary"));
  EXPECT_EQ(destination->text(), QStringLiteral("/tmp/test.mp4"));
  EXPECT_TRUE(progress->isVisible());
  EXPECT_EQ(progress->value(), 50);
  EXPECT_EQ(button->text(), QStringLiteral("Cancel export"));
}

} // namespace
