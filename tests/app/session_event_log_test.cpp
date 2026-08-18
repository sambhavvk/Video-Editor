// SPDX-License-Identifier: MPL-2.0

#include "session_event_log.hpp"

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <fstream>
#include <string>

namespace {

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool fileContains(const std::filesystem::path& path, const std::string& needle) {
  return readFile(path).find(needle) != std::string::npos;
}

} // namespace

class SessionEventLogTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void startWritesSessionLog();
  void logUiAndBackendLinesAppear();
  void cleanShutdownConsumesCrashMarker();
  void simulatedCrashFooterIsReportedOnce();

private:
  std::unique_ptr<QTemporaryDir> application_data_;
};

void SessionEventLogTest::initTestCase() {
  application_data_ = std::make_unique<QTemporaryDir>();
  QVERIFY(application_data_->isValid());
  qputenv("XDG_DATA_HOME", application_data_->path().toUtf8());
  QStandardPaths::setTestModeEnabled(true);
  QCoreApplication::setOrganizationName(QStringLiteral("VideoEditor"));
  QCoreApplication::setApplicationName(QStringLiteral("VideoEditor"));
}

void SessionEventLogTest::startWritesSessionLog() {
  auto& log = video_editor::app::SessionEventLog::instance();
  log.start();
  const auto session_path = log.session_path();
  QVERIFY(!session_path.empty());
  QVERIFY(std::filesystem::exists(session_path));
  QVERIFY(fileContains(session_path, "SESSION start"));
  log.shutdown();
}

void SessionEventLogTest::logUiAndBackendLinesAppear() {
  auto& log = video_editor::app::SessionEventLog::instance();
  log.start();
  const auto session_path = log.session_path();
  log.log_ui("click", "widget=playButton class=QPushButton button=left");
  log.log_backend("apply", "success op=Add asset");
  log.shutdown();

  QVERIFY(fileContains(session_path, "UI click widget=playButton"));
  QVERIFY(fileContains(session_path, "BACKEND apply success op=Add asset"));
}

void SessionEventLogTest::cleanShutdownConsumesCrashMarker() {
  auto& log = video_editor::app::SessionEventLog::instance();
  log.start();
  log.shutdown();
  QVERIFY(!video_editor::app::SessionEventLog::take_previous_crash_log().has_value());
}

void SessionEventLogTest::simulatedCrashFooterIsReportedOnce() {
  auto& log = video_editor::app::SessionEventLog::instance();
  log.start();
  const auto session_path = log.session_path();
  log.debug_simulate_crash_footer(11);

  const auto first = video_editor::app::SessionEventLog::take_previous_crash_log();
  QVERIFY(first.has_value());
  QCOMPARE(*first, session_path);
  QVERIFY(fileContains(session_path, "CRASH signal=11"));
  QVERIFY(!video_editor::app::SessionEventLog::take_previous_crash_log().has_value());

  log.shutdown();
}

QTEST_MAIN(SessionEventLogTest)
#include "session_event_log_test.moc"
