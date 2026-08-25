// SPDX-License-Identifier: MPL-2.0

#include "editor_controller.hpp"
#include "path_utils.hpp"

#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/edit_model/edit_model.h"
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/gpu_backend.h"
#include "video_editor/render_engine/gpu_timeline_renderer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <map>
#include <memory>
#include <string>
#include <variant>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

void writePpmFrame(const QString& path, const int width, const int height) {
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray header = QByteArrayLiteral("P6\n") + QByteArray::number(width) + ' ' +
                            QByteArray::number(height) + QByteArrayLiteral("\n255\n");
  QCOMPARE(file.write(header), static_cast<qint64>(header.size()));
  QByteArray pixels(width * height * 3, '\0');
  for (qsizetype index = 0; index < pixels.size(); index += 3) {
    pixels[index] = static_cast<char>(0x40);
    pixels[index + 1] = static_cast<char>(0x80);
    pixels[index + 2] = static_cast<char>(0xc0);
  }
  QCOMPARE(file.write(pixels), static_cast<qint64>(pixels.size()));
  file.close();
}

[[nodiscard]] QByteArray readFileBytes(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

class DirectoryWriteGuard final {
public:
  explicit DirectoryWriteGuard(QString path) : path_(std::move(path)) {
    previous_ = QFile(path_).permissions();
  }

  ~DirectoryWriteGuard() {
    QFile(path_).setPermissions(previous_);
  }

  DirectoryWriteGuard(const DirectoryWriteGuard&) = delete;
  DirectoryWriteGuard& operator=(const DirectoryWriteGuard&) = delete;

  [[nodiscard]] bool make_read_only() {
    return QFile(path_).setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner |
                                       QFileDevice::ReadUser | QFileDevice::ExeUser |
                                       QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                       QFileDevice::ReadOther | QFileDevice::ExeOther);
  }

  [[nodiscard]] bool rejects_create() const {
    QFile probe(QDir(path_).filePath(QStringLiteral(".write-probe")));
    if (probe.open(QIODevice::WriteOnly)) {
      probe.close();
      probe.remove();
      return false;
    }
    return true;
  }

private:
  QString path_;
  QFileDevice::Permissions previous_;
};

class ModalDialogDismisser final {
public:
  ModalDialogDismisser() {
    timer_.setInterval(0);
    QObject::connect(&timer_, &QTimer::timeout, [] {
      const auto widgets = QApplication::topLevelWidgets();
      for (QWidget* widget : widgets) {
        if (auto* box = qobject_cast<QMessageBox*>(widget); box != nullptr && box->isVisible()) {
          box->accept();
        }
      }
    });
    timer_.start();
  }

private:
  QTimer timer_;
};

class RecordingProvider final : public video_editor::render::FrameProvider {
public:
  std::map<video_editor::edit::EntityId, std::shared_ptr<const video_editor::render::CpuFrame>>
      frames;

  video_editor::render::RenderResult<std::shared_ptr<const video_editor::render::CpuFrame>>
  request(const video_editor::render::AssetFrameRequest& request) override {
    const auto found = frames.find(request.asset_id);
    if (found == frames.end()) {
      return video_editor::render::RenderResult<
          std::shared_ptr<const video_editor::render::CpuFrame>>::
          failure({.code = video_editor::render::RenderErrorCode::AssetUnavailable,
                   .message = "missing GPU fault-injection fixture"});
    }
    return video_editor::render::RenderResult<
        std::shared_ptr<const video_editor::render::CpuFrame>>::success(found->second);
  }
};

[[nodiscard]] std::shared_ptr<video_editor::render::CpuFrame> solidFrame() {
  auto frame = std::make_shared<video_editor::render::CpuFrame>(4, 4);
  for (int y = 0; y < frame->height(); ++y) {
    for (int x = 0; x < frame->width(); ++x) {
      auto pixel = frame->pixel(x, y);
      pixel[0] = 0.2F;
      pixel[1] = 0.4F;
      pixel[2] = 0.6F;
      pixel[3] = 1.0F;
    }
  }
  return frame;
}

} // namespace

class FaultInjectionTest final : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void saveToUnwritableDestinationKeepsDirtyUsableProject();
  void gpuDeviceLostKeepsCpuFrameAndDoesNotMutateRevision();
  void controllerPreviewDoesNotMutateRevision();

private:
  std::unique_ptr<QTemporaryDir> application_data_;
};

void FaultInjectionTest::initTestCase() {
  application_data_ = std::make_unique<QTemporaryDir>();
  QVERIFY(application_data_->isValid());
  qputenv("XDG_DATA_HOME", application_data_->path().toUtf8());
  qputenv("XDG_CACHE_HOME", application_data_->path().toUtf8());
  QStandardPaths::setTestModeEnabled(true);
}

void FaultInjectionTest::saveToUnwritableDestinationKeepsDirtyUsableProject() {
#ifdef _WIN32
  QSKIP("read-only destination injection is Linux-first");
#else
  if (geteuid() == 0) {
    QSKIP("read-only destination injection is skipped when running as root");
  }

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("dest")));
  const QString dest_dir = directory.filePath(QStringLiteral("dest"));
  const QString checkpoint = QDir(dest_dir).filePath(QStringLiteral("project.veproj"));

  QSettings settings(directory.filePath(QStringLiteral("fault-save.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  QVERIFY(!controller.dirty());

  window.addTitleRequested();
  QVERIFY(controller.dirty());
  const auto dirty_revision = controller.editor().revision();

  QVERIFY(controller.saveProjectFile(video_editor::app::pathFromQString(checkpoint)));
  QVERIFY(!controller.dirty());
  QVERIFY(QFileInfo::exists(checkpoint));
  const QByteArray original = readFileBytes(checkpoint);
  QVERIFY(!original.isEmpty());

  // A second title at the same playhead is rejected as an overlap. A caption is a
  // distinct, non-overlapping edit that still dirties the project.
  window.captionsPanel()->addCaptionRequested();
  QVERIFY(controller.dirty());
  const auto after_edit = controller.editor().revision();
  QVERIFY(after_edit != dirty_revision);

  DirectoryWriteGuard guard(dest_dir);
  QVERIFY(guard.make_read_only());
  if (!guard.rejects_create()) {
    QSKIP("environment does not honor directory write permissions");
  }

  ModalDialogDismisser dismisser;
  QVERIFY(!controller.saveProjectFile(video_editor::app::pathFromQString(checkpoint)));
  QVERIFY(controller.dirty());
  QCOMPARE(controller.editor().revision(), after_edit);

  QFile(dest_dir).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner | QFileDevice::ReadUser |
                                 QFileDevice::WriteUser | QFileDevice::ExeUser);
  QCOMPARE(readFileBytes(checkpoint), original);
#endif
}

void FaultInjectionTest::gpuDeviceLostKeepsCpuFrameAndDoesNotMutateRevision() {
  using namespace video_editor;
  auto gpu = std::shared_ptr<render::GpuRenderer>(
      render::GpuRenderer::create({.allow_software = true}));
  if (gpu == nullptr || !gpu->capabilities().available()) {
    QSKIP("software GPU test backend is unavailable");
  }

  edit::Project project;
  edit::Asset asset;
  asset.id = edit::EntityId::generate();
  asset.name = "fault-injection";
  asset.source_uri = "memory://fault-injection";
  asset.duration = edit::Time(4, 1);
  asset.has_video = true;
  asset.width = 4;
  asset.height = 4;
  project.assets.push_back(asset);

  edit::Sequence sequence;
  sequence.width = 4;
  sequence.height = 4;
  sequence.frame_rate = edit::Rate(30, 1);
  edit::Track track;
  track.kind = edit::TrackKind::Video;
  edit::Clip clip;
  clip.asset_id = asset.id;
  clip.kind = edit::ClipKind::Video;
  clip.timeline_range = {edit::Time{}, edit::Time(4, 1)};
  clip.source_range = {edit::Time{}, edit::Time(4, 1)};
  track.clips.push_back(clip);
  sequence.tracks.push_back(track);
  const edit::EntityId sequence_id = sequence.id;
  project.sequences.push_back(sequence);

  edit::TimelineEditor editor(project);
  const auto revision_before = editor.revision();
  auto snapshot = editor.snapshot(sequence_id, revision_before);
  if (!snapshot) {
    const QString message = QString::fromStdString(snapshot.error().message);
    QFAIL(qPrintable(message));
  }

  auto provider = std::make_shared<RecordingProvider>();
  provider->frames[asset.id] = solidFrame();
  render::CpuRenderer cpu(provider);
  render::GpuTimelineRenderer timeline(provider, gpu);
  cpu.begin_epoch(1);
  timeline.begin_epoch(1);

  const auto cpu_before =
      cpu.request_frame(snapshot.value(), edit::Time(1, 30),
                        {.scale = render::PreviewScale::Full}, 1);
  if (!cpu_before) {
    const QString message = QString::fromStdString(cpu_before.error->message);
    QFAIL(qPrintable(message));
  }
  const auto* cpu_storage = std::get_if<std::shared_ptr<const render::CpuFrame>>(
      &cpu_before.value->storage);
  QVERIFY(cpu_storage != nullptr && *cpu_storage);

  gpu->notify_device_lost("injected device loss for controller-level fault test");
  QCOMPARE(gpu->capabilities().state, render::GpuRuntimeState::DeviceLost);

  const auto gpu_after =
      timeline.request_frame(snapshot.value(), edit::Time(1, 30),
                             {.scale = render::PreviewScale::Full}, 1);
  QVERIFY(!gpu_after);
  QCOMPARE(gpu_after.error->code, render::RenderErrorCode::GpuDeviceLost);

  cpu.begin_epoch(2);
  const auto cpu_after =
      cpu.request_frame(snapshot.value(), edit::Time(1, 30),
                        {.scale = render::PreviewScale::Full}, 2);
  if (!cpu_after) {
    const QString message = QString::fromStdString(cpu_after.error->message);
    QFAIL(qPrintable(message));
  }
  const auto* cpu_after_storage = std::get_if<std::shared_ptr<const render::CpuFrame>>(
      &cpu_after.value->storage);
  QVERIFY(cpu_after_storage != nullptr && *cpu_after_storage);
  QCOMPARE((*cpu_after_storage)->width(), (*cpu_storage)->width());
  QCOMPARE((*cpu_after_storage)->height(), (*cpu_storage)->height());
  QCOMPARE(editor.revision(), revision_before);
}

void FaultInjectionTest::controllerPreviewDoesNotMutateRevision() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString frame_path = directory.filePath(QStringLiteral("preview-frame.ppm"));
  writePpmFrame(frame_path, 16, 10);

  QSettings settings(directory.filePath(QStringLiteral("fault-preview.ini")), QSettings::IniFormat);
  video_editor::desktop_ui::EditorWindow window(&settings);
  video_editor::app::EditorController controller(window);
  controller.importPaths({frame_path});
  QTRY_COMPARE_WITH_TIMEOUT(
      controller.editor().projectAt(controller.editor().revision())->assets.size(), 1U, 10'000);
  window.mediaActivated(window.mediaBin()->items().front().id);
  window.rippleInsertFromSource();
  QTRY_VERIFY_WITH_TIMEOUT(window.programViewer()->hasFrame(), 10'000);

  const auto revision = controller.editor().revision();
  const auto presentations = controller.previewPresentationCount();
  window.seekRequested(0);
  QTRY_VERIFY_WITH_TIMEOUT(controller.previewPresentationCount() > presentations, 10'000);
  QVERIFY(window.programViewer()->hasFrame());
  QCOMPARE(controller.editor().revision(), revision);
}

QTEST_MAIN(FaultInjectionTest)
#include "fault_injection_test.moc"
