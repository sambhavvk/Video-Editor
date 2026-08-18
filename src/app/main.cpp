// SPDX-License-Identifier: MPL-2.0
#include "editor_controller.hpp"

#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/media_codec/runtime.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QIcon>
#include <QPixmap>
#include <QTimer>

int main(int argc, char* argv[]) {
  video_editor::media::install_quiet_ffmpeg_log_filter();
  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("VideoEditor"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("video-editor.local"));
  QCoreApplication::setApplicationName(QStringLiteral("VideoEditor"));
  QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Offline-first professional creator video editor"));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("media"),
                               QStringLiteral("Media files to import after startup."),
                               QStringLiteral("[media…]"));
  const QCommandLineOption screenshot_option(
      QStringLiteral("screenshot"),
      QStringLiteral("Save a desktop-shell screenshot and exit (for UI verification)."),
      QStringLiteral("path"));
  parser.addOption(screenshot_option);
  parser.process(application);

  video_editor::desktop_ui::EditorWindow window;
  video_editor::app::EditorController controller(window);
  window.show();
  if (!parser.isSet(screenshot_option)) {
    (void)controller.offerRecoveryOnStartup();
  }
  const QStringList positional = parser.positionalArguments();
  if (!positional.isEmpty()) {
    QStringList absolute_paths;
    absolute_paths.reserve(positional.size());
    for (const QString& path : positional) {
      absolute_paths.push_back(QDir::current().absoluteFilePath(path));
    }
    QTimer::singleShot(0, &controller,
                       [&controller, absolute_paths] { controller.importPaths(absolute_paths); });
  }
  if (parser.isSet(screenshot_option)) {
    const QString screenshot_path = parser.value(screenshot_option);
    QTimer::singleShot(1'000, &window, [&application, &window, screenshot_path] {
      application.exit(window.grab().save(screenshot_path) ? 0 : 2);
    });
  }
  return application.exec();
}
