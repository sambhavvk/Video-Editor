// SPDX-License-Identifier: MPL-2.0
#include "video_editor/desktop_ui/export_dialog.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

using video_editor::desktop_ui::ExportDialog;

class ExportDialogTest final : public QObject {
  Q_OBJECT

private slots:
  void loadsAllPlatformPresets();
  void disablesOkWithoutDestination();
  void acceptsWithDestination();
};

void ExportDialogTest::loadsAllPlatformPresets() {
  ExportDialog dialog;
  dialog.loadPlatformPresets();

  const auto* preset = dialog.findChild<QComboBox*>(QStringLiteral("exportPreset"));
  QVERIFY(preset != nullptr);
  QCOMPARE(preset->count(), 8);
  QVERIFY(!dialog.selectedPresetId().isEmpty());
}

void ExportDialogTest::disablesOkWithoutDestination() {
  ExportDialog dialog;
  auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("exportOkButton"));
  QVERIFY(ok != nullptr);
  QVERIFY(!ok->isEnabled());

  dialog.setDestinationPath(QStringLiteral("/tmp/export-test.mkv"));
  QVERIFY(ok->isEnabled());
}

void ExportDialogTest::acceptsWithDestination() {
  ExportDialog dialog;
  dialog.setDestinationPath(QStringLiteral("/tmp/export-dialog-test.mkv"));
  QSignalSpy accepted(&dialog, &QDialog::accepted);
  auto* ok = dialog.findChild<QPushButton*>(QStringLiteral("exportOkButton"));
  QVERIFY(ok != nullptr);
  QTest::mouseClick(ok, Qt::LeftButton);
  QCOMPARE(accepted.count(), 1);
  QCOMPARE(dialog.destinationPath(), QStringLiteral("/tmp/export-dialog-test.mkv"));
}

QTEST_MAIN(ExportDialogTest)

#include "export_dialog_test.moc"
