// SPDX-License-Identifier: MPL-2.0
#include "editor_controller.hpp"
#include "path_utils.hpp"

#include "video_editor/audio_engine/async_realtime_playback.h"
#include "video_editor/audio_engine/miniaudio_output_device.h"
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/caption_service/caption_service.h"
#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"
#include "video_editor/export_service/export_service.h"
#include "video_editor/playback/asset_registry.h"
#include "video_editor/playback/ffmpeg_frame_provider.h"
#include "video_editor/project_codec/project_codec.h"
#include "video_editor/project_store/project_store.hpp"
#include "video_editor/proxy_service/proxy_service.h"
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/frame.h"
#include "video_editor/render_engine/gpu_backend.h"
#include "video_editor/render_engine/gpu_timeline_renderer.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace video_editor::app {
namespace {

constexpr qint64 kUiTimescale = 48'000;

using desktop_ui::MediaItemView;
using desktop_ui::TimelineClipView;
using desktop_ui::TimelineTrackView;

QString durationText(const edit::Time duration) {
  const double seconds =
      static_cast<double>(duration.value()) / static_cast<double>(duration.timescale());
  const qint64 total_seconds = std::max<qint64>(0, static_cast<qint64>(std::llround(seconds)));
  const qint64 hours = total_seconds / 3600;
  const qint64 minutes = (total_seconds % 3600) / 60;
  const qint64 remaining = total_seconds % 60;
  return QStringLiteral("%1:%2:%3")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(remaining, 2, 10, QLatin1Char('0'));
}

QString timecodeText(const qint64 position, const edit::Rate& rate) {
  const std::int64_t frame_number = rate.framesAt(
      edit::Time(std::max<qint64>(position, 0), static_cast<std::uint32_t>(kUiTimescale)),
      edit::RoundingMode::Floor);
  const std::int64_t nominal_fps = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(std::llround(static_cast<double>(rate.numerator()) /
                                                static_cast<double>(rate.denominator()))));
  const std::int64_t frames = frame_number % nominal_fps;
  const std::int64_t total_seconds = frame_number / nominal_fps;
  const std::int64_t seconds = total_seconds % 60;
  const std::int64_t minutes = (total_seconds / 60) % 60;
  const std::int64_t hours = total_seconds / 3'600;
  return QStringLiteral("%1:%2:%3:%4")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(frames, 2, 10, QLatin1Char('0'));
}

qint64 toUiTime(const edit::Time time) {
  return static_cast<qint64>(
      time.rescaledTo(static_cast<std::uint32_t>(kUiTimescale), edit::RoundingMode::NearestTiesEven)
          .value());
}

QColor colorForTrack(const edit::TrackKind kind, const std::size_t index) {
  if (kind == edit::TrackKind::Audio) {
    return QColor::fromHsv(static_cast<int>((120U + (index * 23U)) % 360U), 110, 170);
  }
  if (kind == edit::TrackKind::Caption) {
    return QColor(166, 116, 190);
  }
  return QColor::fromHsv(static_cast<int>((205U + (index * 19U)) % 360U), 135, 185);
}

desktop_ui::TrackKind uiTrackKind(const edit::TrackKind kind) {
  switch (kind) {
  case edit::TrackKind::Audio:
    return desktop_ui::TrackKind::Audio;
  case edit::TrackKind::Caption:
    return desktop_ui::TrackKind::Caption;
  case edit::TrackKind::Video:
  default:
    return desktop_ui::TrackKind::Video;
  }
}

std::optional<edit::EntityId> parseId(const QString& text) {
  return edit::EntityId::parse(text.toStdString());
}

QString gpuBackendName(const render::GpuBackendKind backend) {
  switch (backend) {
  case render::GpuBackendKind::D3D11:
    return QStringLiteral("D3D11");
  case render::GpuBackendKind::Vulkan:
    return QStringLiteral("Vulkan");
  case render::GpuBackendKind::Auto:
  default:
    return QStringLiteral("GPU");
  }
}

std::vector<std::byte> binaryPayload(const store::JournalEntry& entry) {
  if (const auto* bytes = std::get_if<store::BinaryPayload>(&entry.payload)) {
    return *bytes;
  }
  const auto& text = std::get<std::string>(entry.payload);
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](const char value) { return static_cast<std::byte>(value); });
  return bytes;
}

QString captionDiagnostics(const std::vector<caption_service::Diagnostic>& diagnostics) {
  QStringList lines;
  for (const auto& diagnostic : diagnostics) {
    lines.push_back(diagnostic.line == 0 ? QString::fromStdString(diagnostic.message)
                                         : QObject::tr("Line %1: %2")
                                               .arg(static_cast<qulonglong>(diagnostic.line))
                                               .arg(QString::fromStdString(diagnostic.message)));
  }
  return lines.join(QLatin1Char('\n'));
}

class EpochSyncFrameProvider final : public render::FrameProvider {
public:
  explicit EpochSyncFrameProvider(std::shared_ptr<playback::FfmpegFrameProvider> provider)
      : provider_(std::move(provider)) {}

  render::RenderResult<std::shared_ptr<const render::CpuFrame>>
  request(const render::AssetFrameRequest& request) override {
    provider_->begin_epoch(request.request_epoch);
    return provider_->request(request);
  }

private:
  std::shared_ptr<playback::FfmpegFrameProvider> provider_;
};

class TimelinePlaybackAudioProvider final : public audio::PlaybackAudioProvider {
public:
  TimelinePlaybackAudioProvider(std::shared_ptr<audio_render::TimelineAudioRenderer> renderer,
                                edit::TimelineSnapshot snapshot, const std::int64_t endSample)
      : renderer_(std::move(renderer)), snapshot_(std::move(snapshot)), end_sample_(endSample) {}

  audio::PlaybackRenderResult render(const audio::PlaybackRenderRequest& request) override {
    if (request.cancellation.stop_requested()) {
      return audio::PlaybackRenderResult::cancelled("audio pre-render request was cancelled");
    }
    if (request.start_sample >= end_sample_) {
      return audio::PlaybackRenderResult::end_of_stream();
    }
    auto rendered = renderer_->render(snapshot_, {.start_sample = request.start_sample,
                                                  .sample_count = request.sample_count,
                                                  .cancellation = request.cancellation});
    if (!rendered) {
      const auto& error = rendered.error();
      if (error.code == audio_render::AudioRenderErrorCode::Cancelled) {
        return audio::PlaybackRenderResult::cancelled(error.message);
      }
      return audio::PlaybackRenderResult::failure(error.message);
    }
    audio::AudioBlock block = std::move(rendered).value();
    if (block.start_sample() != request.start_sample ||
        block.frame_count() != request.sample_count ||
        block.format().sample_rate != audio::kPlaybackAudioFormat.sample_rate ||
        block.format().channels != audio::kPlaybackAudioFormat.channels) {
      return audio::PlaybackRenderResult::failure(
          "timeline audio renderer returned a block outside the requested 48 kHz stereo range");
    }
    return audio::PlaybackRenderResult::ready(std::move(block));
  }

private:
  std::shared_ptr<audio_render::TimelineAudioRenderer> renderer_;
  edit::TimelineSnapshot snapshot_;
  std::int64_t end_sample_{0};
};

} // namespace

EditorController::EditorController(desktop_ui::EditorWindow& window, QObject* parent)
    : QObject(parent), window_(window),
      playback_registry_(std::make_shared<playback::AssetRegistry>()),
      audio_registry_(std::make_shared<audio_render::OriginalAudioRegistry>()),
      frame_provider_(std::make_shared<playback::FfmpegFrameProvider>(playback_registry_)),
      renderer_(std::make_shared<render::CpuRenderer>(frame_provider_)) {
  if (auto gpu = render::GpuRenderer::create(); gpu != nullptr) {
    const render::GpuCapabilities capabilities = gpu->capabilities();
    if (capabilities.available() && capabilities.offscreen_rendering) {
      gpu_renderer_ = std::shared_ptr<render::GpuRenderer>(std::move(gpu));
      gpu_timeline_renderer_ =
          std::make_shared<render::GpuTimelineRenderer>(frame_provider_, gpu_renderer_);
      window_.programViewer()->setTitle(
          tr("Program · %1 GPU ready").arg(gpuBackendName(capabilities.backend)));
    } else {
      gpu_fallback_latched_ = true;
      window_.programViewer()->setTitle(tr("Program · CPU"));
    }
  } else {
    gpu_fallback_latched_ = true;
    window_.programViewer()->setTitle(tr("Program · CPU"));
  }
  window_.installEventFilter(this);
  connect(&window_, &desktop_ui::EditorWindow::newProjectRequested, this,
          &EditorController::newProject);
  connect(&window_, &desktop_ui::EditorWindow::openProjectRequested, this,
          &EditorController::openProject);
  connect(&window_, &desktop_ui::EditorWindow::saveProjectRequested, this,
          &EditorController::saveProject);
  connect(&window_, &desktop_ui::EditorWindow::saveProjectAsRequested, this,
          &EditorController::saveProjectAs);
  connect(&window_, &desktop_ui::EditorWindow::importMediaRequested, this,
          &EditorController::chooseMedia);
  connect(&window_, &desktop_ui::EditorWindow::mediaActivated, this,
          &EditorController::insertAsset);
  connect(&window_, &desktop_ui::EditorWindow::splitClipRequested, this,
          &EditorController::splitSelectedClip);
  connect(&window_, &desktop_ui::EditorWindow::deleteSelectionRequested, this,
          &EditorController::deleteSelectedClip);
  connect(&window_, &desktop_ui::EditorWindow::undoRequested, this, &EditorController::undo);
  connect(&window_, &desktop_ui::EditorWindow::redoRequested, this, &EditorController::redo);
  connect(&window_, &desktop_ui::EditorWindow::seekRequested, this, &EditorController::seek);
  connect(&window_, &desktop_ui::EditorWindow::playbackRateRequested, this,
          &EditorController::setPlaybackRate);
  connect(window_.programViewer(), &desktop_ui::ProgramViewer::filesDropped, this,
          &EditorController::importPaths);
  connect(window_.programViewer(), &desktop_ui::ProgramViewer::togglePlaybackRequested, this,
          [this] { setPlaybackRate(playback_rate_ == 0.0 ? 1.0 : 0.0); });
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::importCaptionsRequested, this,
          &EditorController::chooseCaptionFile);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::exportCaptionsRequested, this,
          &EditorController::chooseCaptionExport);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::captionActivated, this,
          &EditorController::seekCaption);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::addCaptionRequested, this,
          &EditorController::addCaptionAtPlayhead);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::removeCaptionRequested, this,
          &EditorController::removeCaption);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::captionTextEdited, this,
          &EditorController::updateCaptionText);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::findInTranscriptRequested,
          this, &EditorController::searchTranscript);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::transcribeRequested, this,
          [this] {
            window_.showTransientMessage(
                tr("Local transcription requires the optional checksummed Whisper model"));
          });
  connect(&window_, &desktop_ui::EditorWindow::exportRequested, this,
          &EditorController::chooseVideoExport);
  connect(&window_, &desktop_ui::EditorWindow::parameterEdited, this,
          &EditorController::updateSelectedClipProperty);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::muteToggled, this,
          &EditorController::setAudioTrackMuted);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::soloToggled, this,
          &EditorController::setAudioTrackSolo);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipActivated, this,
          [this](const QString& clipId) {
            selected_clip_ = parseId(clipId);
            refreshTimelineView();
            refreshInspectorView();
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipEditCommitted, this,
          [this](const QString& clipId, const int destinationTrackIndex, const qint64 startDelta,
                 const qint64 durationDelta, const desktop_ui::TimelineWidget::EditMode mode,
                 const desktop_ui::TimelineWidget::EditIntent intent, const bool snapped) {
            Q_UNUSED(snapped)
            commitTimelineEdit(clipId, destinationTrackIndex, startDelta, durationDelta,
                               static_cast<int>(mode), static_cast<int>(intent));
          });
  connect(window_.mediaBin(), &desktop_ui::MediaBinWidget::proxyRequested, this,
          &EditorController::generateProxy);

  playback_timer_.setTimerType(Qt::PreciseTimer);
  playback_timer_.setInterval(16);
  connect(&playback_timer_, &QTimer::timeout, this, &EditorController::advancePlayback);
  newProject();
}

EditorController::~EditorController() {
  stopAudioPlayback();
  export_stop_source_.request_stop();
  for (const auto& [asset_id, cancellation] : proxy_jobs_) {
    Q_UNUSED(asset_id)
    cancellation->request_stop();
  }
  if (export_future_.isRunning()) {
    export_future_.waitForFinished();
  }
  for (QFuture<ProxyOutcome>& future : proxy_futures_) {
    if (future.isRunning()) {
      future.waitForFinished();
    }
  }
  playback_timer_.stop();
  if (store_) {
    try {
      store_->mark_clean_close(store_->metadata().head_revision);
    } catch (...) {
      // Recovery state remains intentionally unclean when final metadata cannot be written.
    }
  }
}

std::int64_t EditorController::audioMasterSampleCounter() const noexcept {
  return audio_playback_ != nullptr ? audio_playback_->sample_counter() : playhead_;
}

std::uint64_t EditorController::audioXrunCount() const {
  return audio_playback_ != nullptr ? audio_playback_->diagnostics().playback.xrun_count : 0;
}

bool EditorController::audioControlPending() const {
  return audio_playback_ != nullptr &&
         audio_playback_->diagnostics().latest_status == audio::PlaybackCommandStatus::Pending;
}

edit::Project EditorController::makeDefaultProject() {
  edit::Project project;
  project.name = "Untitled Project";
  edit::Sequence sequence;
  sequence.name = "Timeline 1";
  sequence.frame_rate = edit::Rate(30'000, 1'001);
  sequence.width = 1'920;
  sequence.height = 1'080;
  sequence.audio_sample_rate = 48'000;
  for (int index = 1; index <= 2; ++index) {
    edit::Track track;
    track.kind = edit::TrackKind::Video;
    track.name = "V" + std::to_string(index);
    sequence.tracks.push_back(std::move(track));
  }
  for (int index = 1; index <= 4; ++index) {
    edit::Track track;
    track.kind = edit::TrackKind::Audio;
    track.name = "A" + std::to_string(index);
    sequence.tracks.push_back(std::move(track));
  }
  project.sequences.push_back(std::move(sequence));
  return project;
}

std::filesystem::path EditorController::recoveryDirectory() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath() + QStringLiteral("/VideoEditor");
  }
  const QString recovery = QDir(root).filePath(QStringLiteral("recovery"));
  QDir().mkpath(recovery);
  return pathFromQString(recovery);
}

std::filesystem::path EditorController::proxyCacheDirectory() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath() + QStringLiteral("/VideoEditor-cache");
  }
  const QString proxies = QDir(root).filePath(QStringLiteral("proxies"));
  QDir().mkpath(proxies);
  return pathFromQString(proxies);
}

std::filesystem::path EditorController::newWorkingPath(const edit::EntityId& projectId) const {
  return recoveryDirectory() / (projectId.toString() + ".working.sqlite");
}

bool EditorController::eventFilter(QObject* watched, QEvent* event) {
  if (watched == &window_ && event->type() == QEvent::Close && !closing_after_confirmation_) {
    if (!confirmDiscardChanges()) {
      static_cast<QCloseEvent*>(event)->ignore();
      return true;
    }
    closing_after_confirmation_ = true;
  }
  return QObject::eventFilter(watched, event);
}

bool EditorController::confirmDiscardChanges() {
  if (!dirty_) {
    return true;
  }
  const auto answer = QMessageBox::warning(
      &window_, tr("Unsaved changes"), tr("Save the current project before continuing?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (answer == QMessageBox::Cancel) {
    return false;
  }
  if (answer == QMessageBox::Save) {
    saveProject();
    return !dirty_;
  }
  return true;
}

void EditorController::newProject() {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("Cancel the current export before creating a new project"));
    return;
  }
  if (editor_ && !confirmDiscardChanges()) {
    return;
  }
  try {
    edit::Project project = makeDefaultProject();
    const auto working = newWorkingPath(project.id);
    std::error_code ignored;
    std::filesystem::remove(working, ignored);
    auto wal = working;
    wal += "-wal";
    auto shm = working;
    shm += "-shm";
    std::filesystem::remove(wal, ignored);
    std::filesystem::remove(shm, ignored);
    auto project_store = std::make_unique<store::ProjectStore>(
        working, store::OpenOptions{.project_uuid = project.id.toString()});
    installProject(std::move(project), working, std::move(project_store), std::nullopt);
    persistSnapshot("project.created");
    store_->mark_saved(store_->metadata().head_revision);
    setDirty(false);
    window_.showTransientMessage(tr("New project ready"));
  } catch (const std::exception& exception) {
    showError(tr("Could not create project"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::openProject() {
  const QString path = QFileDialog::getOpenFileName(&window_, tr("Open project"), {},
                                                    tr("Video Editor projects (*.veproj)"));
  if (!path.isEmpty()) {
    (void)openProjectFile(pathFromQString(path));
  }
}

bool EditorController::openProjectFile(const std::filesystem::path& checkpoint) {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("Cancel the current export before opening another project"));
    return false;
  }
  return confirmDiscardChanges() && loadCheckpoint(checkpoint);
}

bool EditorController::offerRecoveryOnStartup() {
  try {
    const store::RecoveryCatalog catalog = store::scan_recovery_directory(recoveryDirectory());
    const auto candidate = std::find_if(
        catalog.candidates.begin(), catalog.candidates.end(), [this](const auto& item) {
          return item.valid_project_database && item.recovery_recommended &&
                 item.working_database != working_path_;
        });
    if (candidate == catalog.candidates.end()) {
      return false;
    }

    const QString last_edit = QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(candidate->heartbeat_utc_ms, QTimeZone::UTC).toLocalTime(),
        QLocale::ShortFormat);
    const QString details =
        candidate->head_revision != candidate->saved_revision
            ? tr("It contains committed edits newer than the last saved checkpoint.")
            : tr("It was not closed cleanly, so its last committed state is available.");
    const auto answer = QMessageBox::question(
        &window_, tr("Recover your last project?"),
        tr("Video Editor found a recoverable project from %1.\n\n%2\n\nOpen it now?")
            .arg(last_edit, details),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    return answer == QMessageBox::Yes && loadWorkingRecovery(candidate->working_database);
  } catch (const std::exception& exception) {
    window_.showTransientMessage(
        tr("Recovery scan could not be completed: %1").arg(QString::fromUtf8(exception.what())));
    return false;
  }
}

bool EditorController::loadWorkingRecovery(const std::filesystem::path& workingDatabase) {
  try {
    auto recovered_store = std::make_unique<store::ProjectStore>(
        workingDatabase, store::OpenOptions{.create_if_missing = false,
                                            .run_integrity_check = true,
                                            .busy_timeout_ms = 5'000,
                                            .project_uuid = std::nullopt});
    const auto commands = recovered_store->read_commands();
    const store::JournalEntry* snapshot_entry = nullptr;
    for (const auto& command : commands) {
      if (command.command_type == "project.snapshot.v1") {
        snapshot_entry = &command;
      }
    }
    if (snapshot_entry == nullptr) {
      throw std::runtime_error("Recovery database contains no readable project snapshot");
    }
    const auto bytes = binaryPayload(*snapshot_entry);
    auto decoded = project_codec::deserialize_project(std::span<const std::byte>(bytes));
    if (!decoded) {
      throw std::runtime_error(decoded.error().message);
    }
    if (decoded.value().id.toString() != recovered_store->metadata().project_uuid) {
      throw std::runtime_error("Recovery project identity does not match its database");
    }
    installProject(std::move(decoded).value(), workingDatabase, std::move(recovered_store),
                   std::nullopt);
    setDirty(true);
    window_.showTransientMessage(tr("Recovered the latest committed edit; save it to keep it"),
                                 8'000);
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not recover project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

bool EditorController::loadCheckpoint(const std::filesystem::path& checkpoint) {
  try {
    auto working_name = checkpoint.stem();
    working_name += "-" + edit::EntityId::generate().toString() + ".working.sqlite";
    const std::filesystem::path working = recoveryDirectory() / working_name;
    std::filesystem::copy_file(checkpoint, working,
                               std::filesystem::copy_options::overwrite_existing);
    auto opened_store = std::make_unique<store::ProjectStore>(working);
    const auto commands = opened_store->read_commands();
    const store::JournalEntry* snapshot_entry = nullptr;
    for (const auto& command : commands) {
      if (command.command_type == "project.snapshot.v1") {
        snapshot_entry = &command;
      }
    }
    if (snapshot_entry == nullptr) {
      throw std::runtime_error("Project contains no readable model snapshot");
    }
    const auto bytes = binaryPayload(*snapshot_entry);
    auto decoded = project_codec::deserialize_project(std::span<const std::byte>(bytes));
    if (!decoded) {
      throw std::runtime_error(decoded.error().message);
    }
    installProject(std::move(decoded).value(), working, std::move(opened_store), checkpoint);
    setDirty(false);
    window_.showTransientMessage(tr("Project opened"));
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not open project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

void EditorController::installProject(edit::Project project, std::filesystem::path workingPath,
                                      std::unique_ptr<store::ProjectStore> projectStore,
                                      std::optional<std::filesystem::path> checkpoint) {
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  if (store_) {
    try {
      store_->mark_clean_close(store_->metadata().head_revision);
    } catch (...) {
      // Preserve the previous recovery database as unclean if finalization fails.
    }
  }
  editor_ = std::make_unique<edit::TimelineEditor>(std::move(project));
  store_ = std::move(projectStore);
  working_path_ = std::move(workingPath);
  checkpoint_path_ = std::move(checkpoint);
  imported_assets_.clear();
  visible_caption_indices_.clear();
  caption_search_.clear();
  selected_clip_.reset();
  playhead_ = 0;
  rebuildPlaybackRegistry();
  refreshViews();
}

void EditorController::saveProject() {
  if (!checkpoint_path_.has_value()) {
    saveProjectAs();
    return;
  }
  (void)saveTo(*checkpoint_path_);
}

void EditorController::saveProjectAs() {
  QString suggested;
  if (editor_) {
    suggested = QString::fromStdString(editor_->projectAt(editor_->revision())->name) +
                QStringLiteral(".veproj");
  }
  const QString path = QFileDialog::getSaveFileName(&window_, tr("Save project"), suggested,
                                                    tr("Video Editor projects (*.veproj)"));
  if (!path.isEmpty()) {
    (void)saveTo(pathFromQString(path));
  }
}

bool EditorController::saveTo(const std::filesystem::path& destination) {
  try {
    if (destination.extension() != ".veproj") {
      auto corrected = destination;
      corrected += ".veproj";
      checkpoint_path_ = corrected;
    } else {
      checkpoint_path_ = destination;
    }
    const auto revision = store_->metadata().head_revision;
    store_->checkpoint_to(*checkpoint_path_, revision);
    setDirty(false);
    window_.showTransientMessage(tr("Project saved"));
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not save project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

bool EditorController::saveProjectFile(const std::filesystem::path& destination) {
  return saveTo(destination);
}

void EditorController::chooseMedia() {
  const QStringList paths =
      QFileDialog::getOpenFileNames(&window_, tr("Import media"), {},
                                    tr("Media files (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.wav "
                                       "*.flac *.mp3 *.ogg);;All files (*)"));
  importPaths(paths);
}

void EditorController::chooseCaptionFile() {
  const QString path = QFileDialog::getOpenFileName(
      &window_, tr("Import captions"), {},
      tr("Caption files (*.srt *.vtt);;SubRip captions (*.srt);;WebVTT captions (*.vtt)"));
  if (!path.isEmpty()) {
    (void)importCaptionFile(pathFromQString(path));
  }
}

void EditorController::chooseCaptionExport() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || sequence->captions.empty()) {
    window_.showTransientMessage(tr("There are no sequence captions to export"));
    return;
  }
  const QString path =
      QFileDialog::getSaveFileName(&window_, tr("Export captions"), QStringLiteral("captions.srt"),
                                   tr("SubRip captions (*.srt);;WebVTT captions (*.vtt)"));
  if (!path.isEmpty()) {
    (void)exportCaptionFile(pathFromQString(path));
  }
}

bool EditorController::importCaptionFile(const std::filesystem::path& source) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    showError(tr("Could not import captions"), tr("The project has no active sequence."));
    return false;
  }
  QFile file(qStringFromPath(source));
  if (!file.open(QIODevice::ReadOnly)) {
    showError(tr("Could not import captions"), file.errorString());
    return false;
  }
  const QByteArray contents = file.readAll();
  const QString suffix = qStringFromPath(source.extension()).toLower();
  const auto format = suffix == QStringLiteral(".vtt") ? caption_service::SubtitleFormat::WebVtt
                                                       : caption_service::SubtitleFormat::Srt;
  auto parsed = caption_service::parse(
      std::string_view(contents.constData(), static_cast<std::size_t>(contents.size())), format);
  if (!parsed) {
    showError(tr("Could not import captions"), captionDiagnostics(parsed.error()));
    return false;
  }

  const auto captions = caption_service::toEditCaptions(parsed.value());
  if (captions.empty()) {
    showError(tr("Could not import captions"), tr("The caption file contains no cues."));
    return false;
  }
  const std::string gesture = "import-captions:" + edit::EntityId::generate().toString();
  std::vector<edit::EditCommand> commands;
  commands.reserve(captions.size());
  for (const edit::Caption& caption : captions) {
    commands.push_back(
        {.operation = edit::AddCaptionCommand{.sequence_id = sequence->id, .caption = caption},
         .coalescing_key = gesture});
  }
  if (!applyBatch(std::move(commands), tr("Could not import captions"))) {
    return false;
  }
  window_.showTransientMessage(tr("Imported %1 caption cue(s)").arg(captions.size()));
  return true;
}

bool EditorController::exportCaptionFile(const std::filesystem::path& destination) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || sequence->captions.empty()) {
    showError(tr("Could not export captions"), tr("The active sequence has no captions."));
    return false;
  }
  const QString suffix = qStringFromPath(destination.extension()).toLower();
  const auto format = suffix == QStringLiteral(".vtt") ? caption_service::SubtitleFormat::WebVtt
                                                       : caption_service::SubtitleFormat::Srt;
  const auto document = caption_service::fromEditCaptions(sequence->captions, format);
  const caption_service::SerializeOptions options{
      .timestamp_policy = caption_service::TimestampPolicy::NearestMillisecond,
      .emit_utf8_bom = false,
      .validation = {}};
  auto serialized = caption_service::serialize(document, format, options);
  if (!serialized) {
    showError(tr("Could not export captions"), captionDiagnostics(serialized.error()));
    return false;
  }

  QSaveFile output(qStringFromPath(destination));
  if (!output.open(QIODevice::WriteOnly)) {
    showError(tr("Could not export captions"), output.errorString());
    return false;
  }
  const std::string& text = serialized.value();
  if (output.write(text.data(), static_cast<qint64>(text.size())) !=
          static_cast<qint64>(text.size()) ||
      !output.commit()) {
    showError(tr("Could not export captions"), output.errorString());
    return false;
  }
  window_.showTransientMessage(tr("Captions exported"));
  return true;
}

void EditorController::chooseVideoExport(const QString& presetId) {
  if (export_in_flight_) {
    export_stop_source_.request_stop();
    window_.showTransientMessage(tr("Cancelling export after the current frame…"));
    return;
  }
  const auto preset = presetId == QStringLiteral("master.prores")
                          ? export_service::VideoPreset::ProRes422HqMov
                          : export_service::VideoPreset::Ffv1Matroska;
  const QString extension = preset == export_service::VideoPreset::Ffv1Matroska
                                ? QStringLiteral("mkv")
                                : QStringLiteral("mov");
  const QString filter = preset == export_service::VideoPreset::Ffv1Matroska
                             ? tr("Matroska video (*.mkv)")
                             : tr("QuickTime movie (*.mov)");
  const QString destination = QFileDialog::getSaveFileName(
      &window_, tr("Export video master"), QStringLiteral("export.%1").arg(extension), filter);
  if (!destination.isEmpty()) {
    (void)startVideoExport(pathFromQString(destination), presetId, true);
  }
}

bool EditorController::startVideoExport(const std::filesystem::path& destination,
                                        const QString& presetId, const bool overwriteExisting) {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("An export is already running"));
    return false;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || edit::sequenceDuration(*sequence).isZero()) {
    showError(tr("Could not export"), tr("Add at least one clip to the timeline first."));
    return false;
  }

  const auto preset = presetId == QStringLiteral("master.prores")
                          ? export_service::VideoPreset::ProRes422HqMov
                          : export_service::VideoPreset::Ffv1Matroska;
  const export_service::PresetInfo preset_details = export_service::preset_info(preset);
  if (!preset_details.available) {
    showError(tr("Encoder unavailable"),
              tr("The selected %1 encoder is not available in this build.")
                  .arg(QString::fromStdString(preset_details.display_name)));
    return false;
  }

  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    showError(tr("Could not export"), QString::fromStdString(snapshot_result.error().message));
    return false;
  }

  auto export_provider = std::make_shared<playback::FfmpegFrameProvider>(playback_registry_);
  auto synchronized_provider = std::make_shared<EpochSyncFrameProvider>(export_provider);
  auto export_renderer = std::make_shared<render::CpuRenderer>(synchronized_provider);
  auto export_audio_renderer =
      std::make_shared<audio_render::TimelineAudioRenderer>(audio_registry_);
  auto snapshot = std::move(snapshot_result).value();
  const auto output_path = destination;
  const QString output_display = qStringFromPath(destination);
  export_stop_source_ = std::stop_source{};
  const std::stop_token stop_token = export_stop_source_.get_token();
  export_in_flight_ = true;
  window_.deliverPanel()->setExportRunning(true, 0);
  window_.showTransientMessage(
      tr("Exporting full-quality video and 48 kHz audio from original media…"), 0);

  QPointer<EditorController> guard(this);
  auto* watcher = new QFutureWatcher<VideoExportOutcome>(this);
  connect(watcher, &QFutureWatcher<VideoExportOutcome>::finished, this,
          [this, watcher, output_display] {
            const VideoExportOutcome outcome = watcher->result();
            watcher->deleteLater();
            export_in_flight_ = false;
            window_.deliverPanel()->setExportRunning(false, 0);
            if (outcome.succeeded) {
              const QString message = tr("Export complete · %1 video frames · %2 audio samples")
                                          .arg(outcome.frame_count)
                                          .arg(outcome.audio_sample_count);
              window_.showTransientMessage(message);
              emit videoExportFinished(true, output_display, message);
            } else if (outcome.cancelled) {
              const QString message = tr("Export cancelled; the destination was left unchanged");
              window_.showTransientMessage(message);
              emit videoExportFinished(false, output_display, message);
            } else {
              showError(tr("Export failed"), outcome.error);
              emit videoExportFinished(false, output_display, outcome.error);
            }
          });
  auto future = QtConcurrent::run([snapshot = std::move(snapshot),
                                   export_renderer = std::move(export_renderer), output_path,
                                   export_audio_renderer = std::move(export_audio_renderer), preset,
                                   stop_token, guard, overwriteExisting]() mutable {
    int last_percent = -1;
    export_service::ExportRequest request{
        .snapshot = std::move(snapshot),
        .renderer = std::move(export_renderer),
        .audio_renderer = std::move(export_audio_renderer),
        .destination = output_path,
        .preset = preset,
        .overwrite_existing = overwriteExisting,
        .include_audio = true,
        .cancellation = stop_token,
        .progress = [guard, &last_percent](const export_service::ExportProgress& progress) {
          const int percent =
              std::clamp(static_cast<int>(std::lround(progress.fraction * 100.0)), 0, 100);
          if (percent == last_percent || (percent != 100 && percent % 2 != 0)) {
            return;
          }
          last_percent = percent;
          if (guard) {
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, percent] {
                  if (guard) {
                    guard->window_.deliverPanel()->setExportRunning(true, percent);
                  }
                },
                Qt::QueuedConnection);
          }
        }};
    auto outcome = export_service::export_video(request);
    if (!outcome) {
      return VideoExportOutcome{.succeeded = false,
                                .cancelled = outcome.error().code ==
                                             export_service::ExportErrorCode::Cancelled,
                                .frame_count = 0,
                                .audio_sample_count = 0,
                                .error = QString::fromStdString(outcome.error().message)};
    }
    return VideoExportOutcome{.succeeded = true,
                              .cancelled = false,
                              .frame_count = outcome.value().frame_count,
                              .audio_sample_count = outcome.value().audio_sample_count,
                              .error = {}};
  });
  export_future_ = future;
  watcher->setFuture(future);
  return true;
}

void EditorController::importPaths(const QStringList& paths) {
  if (paths.isEmpty()) {
    return;
  }
  window_.showTransientMessage(tr("Importing %1 item(s)…").arg(paths.size()), 0);
  auto* watcher = new QFutureWatcher<std::vector<ImportOutcome>>(this);
  connect(watcher, &QFutureWatcher<std::vector<ImportOutcome>>::finished, this, [this, watcher] {
    const auto outcomes = watcher->result();
    watcher->deleteLater();
    int imported = 0;
    QStringList errors;
    for (const auto& outcome : outcomes) {
      if (outcome.asset.has_value()) {
        addImportedAsset(*outcome.asset);
        ++imported;
      } else {
        errors.push_back(QStringLiteral("%1: %2").arg(qStringFromPath(outcome.path.filename()),
                                                      QString::fromStdString(outcome.error)));
      }
    }
    refreshViews();
    window_.showTransientMessage(tr("Imported %1 item(s)").arg(imported));
    if (!errors.isEmpty()) {
      showError(tr("Some media could not be imported"), errors.join(QLatin1Char('\n')));
    }
  });

  std::vector<std::filesystem::path> media_paths;
  media_paths.reserve(static_cast<std::size_t>(paths.size()));
  for (const QString& path : paths) {
    media_paths.emplace_back(pathFromQString(path));
  }
  watcher->setFuture(QtConcurrent::run([media_paths = std::move(media_paths)] {
    assets::AssetService service;
    std::vector<ImportOutcome> outcomes;
    outcomes.reserve(media_paths.size());
    for (const auto& path : media_paths) {
      auto imported = service.import(path);
      if (imported) {
        outcomes.push_back({.path = path, .asset = std::move(imported).value(), .error = {}});
      } else {
        outcomes.push_back(
            {.path = path, .asset = std::nullopt, .error = imported.error().message});
      }
    }
    return outcomes;
  }));
}

void EditorController::addImportedAsset(assets::AssetRecord asset) {
  edit::Asset model_asset;
  const auto parsed_id = edit::EntityId::parse(asset.id);
  model_asset.id = parsed_id.value_or(edit::EntityId::generate());
  asset.id = model_asset.id.toString();
  model_asset.name = utf8StringFromPath(asset.uri.filename());
  model_asset.source_uri = utf8StringFromPath(asset.uri);
  model_asset.fingerprint = asset.fingerprint.quick_sha256;
  if (asset.descriptor.duration_microseconds.has_value() &&
      *asset.descriptor.duration_microseconds > 0) {
    model_asset.duration = edit::Time(*asset.descriptor.duration_microseconds, 1'000'000);
  } else {
    model_asset.duration = edit::Time(5, 1);
  }
  model_asset.metadata["container"] = asset.descriptor.format_name;
  for (const auto& stream : asset.descriptor.streams) {
    if (stream.video.has_value() && !model_asset.has_video) {
      model_asset.has_video = true;
      model_asset.width = static_cast<std::uint32_t>(std::max(stream.video->width, 0));
      model_asset.height = static_cast<std::uint32_t>(std::max(stream.video->height, 0));
      model_asset.metadata["video_codec"] = stream.codec_name;
      if (stream.video->average_frame_rate.numerator > 0 &&
          stream.video->average_frame_rate.denominator > 0) {
        model_asset.nominal_frame_rate =
            edit::Rate(static_cast<std::uint32_t>(stream.video->average_frame_rate.numerator),
                       static_cast<std::uint32_t>(stream.video->average_frame_rate.denominator));
      }
    }
    if (stream.audio.has_value() && !model_asset.has_audio) {
      model_asset.has_audio = true;
      model_asset.audio_sample_rate = static_cast<std::uint32_t>(stream.audio->sample_rate);
      model_asset.audio_channels = static_cast<std::uint32_t>(stream.audio->channels);
      model_asset.metadata["audio_codec"] = stream.codec_name;
    }
  }
  if (apply(edit::EditCommand{.operation = edit::AddAssetCommand{.asset = model_asset},
                              .coalescing_key = {}},
            tr("Could not add imported media"))) {
    playback::AssetPlaybackSources sources{
        .original = {.path = asset.uri, .video_stream_index = -1}, .proxy = std::nullopt};
    if (asset.proxy.has_value() && asset.proxy->complete) {
      sources.proxy =
          playback::AssetStreamLocation{.path = asset.proxy->proxy_uri, .video_stream_index = -1};
    }
    if (playback_registry_->register_asset(model_asset.id, std::move(sources))) {
      registered_playback_assets_.push_back(model_asset.id);
    }
    if (model_asset.has_audio) {
      if (audio_registry_->register_original(
              model_asset.id,
              audio_render::OriginalAudioMedia{.path = asset.uri, .audio_stream_index = -1})) {
        registered_audio_assets_.push_back(model_asset.id);
      }
    }
    imported_assets_.push_back(std::move(asset));
  }
}

void EditorController::generateProxy(const QString& assetId) {
  const std::string key = assetId.toStdString();
  if (const auto running = proxy_jobs_.find(key); running != proxy_jobs_.end()) {
    running->second->request_stop();
    window_.showTransientMessage(tr("Cancelling proxy generation…"));
    return;
  }

  auto record =
      std::find_if(imported_assets_.begin(), imported_assets_.end(),
                   [&key](const assets::AssetRecord& candidate) { return candidate.id == key; });
  if (record == imported_assets_.end()) {
    window_.showTransientMessage(tr("Reimport this media before creating its proxy"));
    return;
  }
  if (record->proxy.has_value() && record->proxy->complete) {
    window_.showTransientMessage(tr("This media already has an editing proxy"));
    return;
  }

  const assets::ProxyProfile asset_profile = assets::AssetService::default_proxy_profile(*record);
  proxy::ProxyProfile profile{.video_codec = asset_profile.codec == assets::ProxyCodec::Ffv1
                                                 ? proxy::VideoCodec::Ffv1
                                                 : proxy::VideoCodec::ProResProxy,
                              .scale_numerator = 1,
                              .scale_denominator = 2,
                              .maximum_width = asset_profile.maximum_width,
                              .maximum_height = asset_profile.maximum_height,
                              .include_pcm_audio = asset_profile.include_pcm_audio,
                              .allow_ffv1_fallback = true};
  const auto resolved = proxy::resolve_profile(profile, proxy::encoder_availability());
  if (!resolved) {
    showError(tr("Could not create proxy"), QString::fromStdString(resolved.error().message));
    return;
  }

  const QString extension = resolved.value().container == proxy::Container::QuickTime
                                ? QStringLiteral(".mov")
                                : QStringLiteral(".mkv");
  const std::filesystem::path destination =
      proxyCacheDirectory() / pathFromQString(assetId + QStringLiteral(".proxy") + extension);
  const std::filesystem::path source = record->uri;
  auto cancellation = std::make_shared<std::stop_source>();
  const std::stop_token stop_token = cancellation->get_token();
  proxy_jobs_.emplace(key, cancellation);
  refreshMediaView();
  window_.showTransientMessage(tr("Creating a half-resolution editing proxy…"), 0);

  auto* watcher = new QFutureWatcher<ProxyOutcome>(this);
  connect(watcher, &QFutureWatcher<ProxyOutcome>::finished, this, [this, watcher] {
    const ProxyOutcome outcome = watcher->result();
    watcher->deleteLater();
    proxy_jobs_.erase(outcome.asset_id);
    auto imported = std::find_if(imported_assets_.begin(), imported_assets_.end(),
                                 [&outcome](const assets::AssetRecord& candidate) {
                                   return candidate.id == outcome.asset_id;
                                 });
    if (outcome.succeeded && imported != imported_assets_.end()) {
      assets::ProxyProfile manifest_profile =
          assets::AssetService::default_proxy_profile(*imported);
      manifest_profile.codec =
          outcome.ffv1 ? assets::ProxyCodec::Ffv1 : assets::ProxyCodec::ProResProxy;
      imported->proxy = assets::ProxyManifest{.proxy_uri = outcome.destination,
                                              .profile = manifest_profile,
                                              .source_fingerprint = imported->fingerprint,
                                              .engine_version = "proxy-service-v1",
                                              .complete = true};
      const auto model_id = edit::EntityId::parse(outcome.asset_id);
      if (model_id.has_value()) {
        playback::AssetPlaybackSources sources{
            .original = {.path = imported->uri, .video_stream_index = -1},
            .proxy = playback::AssetStreamLocation{.path = outcome.destination,
                                                   .video_stream_index = -1}};
        (void)playback_registry_->register_asset(*model_id, std::move(sources));
        frame_provider_->invalidate(*model_id);
      }
      window_.showTransientMessage(outcome.ffv1 ? tr("Proxy ready (FFV1 compatibility profile)")
                                                : tr("Proxy ready"));
    } else if (outcome.cancelled) {
      window_.showTransientMessage(tr("Proxy generation cancelled; the cache was left intact"));
    } else if (!outcome.error.isEmpty()) {
      showError(tr("Proxy generation failed"), outcome.error);
    }
    refreshViews();
  });

  auto future = QtConcurrent::run([source, destination, profile, stop_token, key] {
    const proxy::GenerateRequest request{.source = source,
                                         .destination = destination,
                                         .pts_map_destination = std::nullopt,
                                         .profile = profile};
    const auto generated = proxy::generate_proxy(request, stop_token);
    if (!generated) {
      return ProxyOutcome{.asset_id = key,
                          .destination = destination,
                          .succeeded = false,
                          .cancelled = generated.error().code == proxy::ErrorCode::Cancelled,
                          .ffv1 = false,
                          .error = QString::fromStdString(generated.error().message)};
    }
    return ProxyOutcome{.asset_id = key,
                        .destination = generated.value().destination,
                        .succeeded = true,
                        .cancelled = false,
                        .ffv1 = generated.value().profile.video_codec == proxy::VideoCodec::Ffv1,
                        .error = {}};
  });
  proxy_futures_.push_back(future);
  watcher->setFuture(future);
}

void EditorController::insertAsset(const QString& assetId) {
  const edit::Asset* asset = assetByTextId(assetId);
  const edit::Sequence* sequence = currentSequence();
  if (asset == nullptr || sequence == nullptr) {
    return;
  }
  const edit::Asset asset_copy = *asset;
  const edit::EntityId sequence_id = sequence->id;
  std::optional<edit::EntityId> video_track_id;
  std::optional<edit::EntityId> audio_track_id;
  for (const edit::Track& track : sequence->tracks) {
    if (!track.locked && track.kind == edit::TrackKind::Video && !video_track_id.has_value()) {
      video_track_id = track.id;
    }
    if (!track.locked && track.kind == edit::TrackKind::Audio && !audio_track_id.has_value()) {
      audio_track_id = track.id;
    }
  }
  const edit::Time start = playheadTime();
  const edit::Time duration = asset_copy.duration.isZero() ? edit::Time(5, 1) : asset_copy.duration;
  const edit::EntityId linked = edit::EntityId::generate();
  std::optional<edit::EntityId> first_inserted;
  std::vector<edit::EditCommand> commands;

  const bool sequence_has_no_clips =
      std::all_of(sequence->tracks.begin(), sequence->tracks.end(),
                  [](const edit::Track& track) { return track.clips.empty(); });
  if (sequence_has_no_clips && asset_copy.has_video && asset_copy.width > 0 &&
      asset_copy.height > 0) {
    commands.push_back(
        {.operation =
             edit::SetSequenceFormatCommand{
                 .sequence_id = sequence_id,
                 .frame_rate = asset_copy.nominal_frame_rate.value_or(sequence->frame_rate),
                 .width = asset_copy.width,
                 .height = asset_copy.height},
         .coalescing_key = {}});
  }

  auto prepare_insert = [&](const std::optional<edit::EntityId> track_id,
                            const edit::ClipKind clip_kind) {
    if (!track_id.has_value()) {
      return false;
    }
    edit::Clip clip;
    clip.asset_id = asset_copy.id;
    clip.kind = clip_kind;
    clip.name = asset_copy.name;
    clip.timeline_range = {start, duration};
    clip.source_range = {edit::Time{}, duration};
    if (asset_copy.has_video && asset_copy.has_audio) {
      clip.linked_group = linked;
    }
    if (!first_inserted.has_value()) {
      first_inserted = clip.id;
    }
    commands.push_back(
        {.operation = edit::InsertClipCommand{.sequence_id = sequence_id,
                                              .track_id = *track_id,
                                              .clip = std::move(clip),
                                              .mode = edit::InsertMode::RejectOverlap},
         .coalescing_key = {}});
    return true;
  };

  if (asset_copy.has_video && !prepare_insert(video_track_id, edit::ClipKind::Video)) {
    window_.showTransientMessage(tr("No unlocked video track is available"));
    return;
  }
  if (asset_copy.has_audio && !prepare_insert(audio_track_id, edit::ClipKind::Audio)) {
    window_.showTransientMessage(tr("No unlocked audio track is available"));
    return;
  }
  if (applyBatch(std::move(commands), tr("Could not insert media at the playhead"))) {
    selected_clip_ = first_inserted;
    refreshViews();
  }
}

void EditorController::splitSelectedClip() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !selected_clip_.has_value()) {
    window_.showTransientMessage(tr("Select a clip before splitting"));
    return;
  }
  (void)apply(edit::EditCommand{.operation = edit::SplitClipCommand{.sequence_id = sequence->id,
                                                                    .clip_id = *selected_clip_,
                                                                    .split_time = playheadTime()},
                                .coalescing_key = {}},
              tr("Could not split the selected clip"));
}

void EditorController::deleteSelectedClip(const bool ripple) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !selected_clip_.has_value()) {
    return;
  }
  if (apply(edit::EditCommand{.operation = edit::RemoveClipCommand{.sequence_id = sequence->id,
                                                                   .clip_id = *selected_clip_,
                                                                   .ripple = ripple},
                              .coalescing_key = {}},
            tr("Could not remove the selected clip"))) {
    selected_clip_.reset();
  }
}

void EditorController::commitTimelineEdit(const QString& clipId, const int destinationTrackIndex,
                                          const qint64 startDelta, const qint64 durationDelta,
                                          const int editMode, const int editIntent) {
  const edit::Sequence* sequence = currentSequence();
  const auto parsed_clip_id = parseId(clipId);
  if (sequence == nullptr || !parsed_clip_id.has_value() || destinationTrackIndex < 0 ||
      destinationTrackIndex >= static_cast<int>(sequence->tracks.size())) {
    window_.showTransientMessage(tr("The timeline edit target is no longer available"));
    refreshTimelineView();
    return;
  }

  const edit::Clip* selected_clip = edit::findClip(*sequence, *parsed_clip_id);
  if (selected_clip == nullptr) {
    window_.showTransientMessage(tr("The selected clip no longer exists"));
    refreshTimelineView();
    return;
  }
  const edit::Clip clip = *selected_clip;
  const edit::Time start_delta(startDelta, static_cast<std::uint32_t>(kUiTimescale));
  const edit::Time duration_delta(durationDelta, static_cast<std::uint32_t>(kUiTimescale));
  selected_clip_ = clip.id;

  try {
    const auto mode = static_cast<desktop_ui::TimelineWidget::EditMode>(editMode);
    const auto intent = static_cast<desktop_ui::TimelineWidget::EditIntent>(editIntent);
    if (mode == desktop_ui::TimelineWidget::EditMode::Move) {
      edit::InsertMode insert_mode = edit::InsertMode::RejectOverlap;
      if (intent == desktop_ui::TimelineWidget::EditIntent::Ripple) {
        insert_mode = edit::InsertMode::Ripple;
      } else if (intent == desktop_ui::TimelineWidget::EditIntent::Overwrite) {
        insert_mode = edit::InsertMode::Overwrite;
      }
      (void)apply(
          edit::EditCommand{
              .operation =
                  edit::MoveClipCommand{
                      .sequence_id = sequence->id,
                      .clip_id = clip.id,
                      .destination_track_id =
                          sequence->tracks[static_cast<std::size_t>(destinationTrackIndex)].id,
                      .new_start = clip.timeline_range.start + start_delta,
                      .mode = insert_mode,
                      .include_linked = true},
              .coalescing_key = {}},
          tr("Could not move the selected clip"));
      return;
    }

    const edit::TimeRange timeline_range(clip.timeline_range.start + start_delta,
                                         clip.timeline_range.duration + duration_delta);
    const edit::Time head_delta = timeline_range.start - clip.timeline_range.start;
    const edit::Time tail_delta = timeline_range.end() - clip.timeline_range.end();
    const auto source_delta = [&clip](const edit::Time delta) {
      return delta
          .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                  edit::RoundingMode::NearestTiesEven)
          .rescaledTo(clip.source_range.duration.timescale(), edit::RoundingMode::NearestTiesEven);
    };
    const edit::Time head_source_delta = source_delta(head_delta);
    const edit::Time tail_source_delta = source_delta(tail_delta);
    edit::Time source_start = clip.source_range.start;
    edit::Time source_end = clip.source_range.end();
    if (!clip.reversed) {
      source_start = source_start + head_source_delta;
      source_end = source_end + tail_source_delta;
    } else {
      source_start = source_start - tail_source_delta;
      source_end = source_end - head_source_delta;
    }
    (void)apply(
        edit::EditCommand{.operation =
                              edit::TrimClipCommand{.sequence_id = sequence->id,
                                                    .clip_id = clip.id,
                                                    .timeline_range = timeline_range,
                                                    .source_range = edit::TimeRange(
                                                        source_start, source_end - source_start),
                                                    .include_linked = true},
                          .coalescing_key = {}},
        tr("Could not trim the selected clip"));
  } catch (const std::exception& exception) {
    window_.showTransientMessage(
        tr("Could not apply the timeline edit: %1").arg(QString::fromUtf8(exception.what())));
    refreshTimelineView();
  }
}

void EditorController::undo() {
  const auto result = editor_->undo(editor_->revision());
  if (!result) {
    window_.showTransientMessage(QString::fromStdString(result.error().message));
    return;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("history.undo");
    setDirty(true);
    selected_clip_.reset();
    refreshViews();
  } catch (const std::exception& exception) {
    showError(tr("Could not persist undo"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::redo() {
  const auto result = editor_->redo(editor_->revision());
  if (!result) {
    window_.showTransientMessage(QString::fromStdString(result.error().message));
    return;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("history.redo");
    setDirty(true);
    selected_clip_.reset();
    refreshViews();
  } catch (const std::exception& exception) {
    showError(tr("Could not persist redo"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::seek(const qint64 position) {
  playhead_ = std::max<qint64>(position, 0);
  if (audio_playback_ != nullptr && !audio_session_stale_ &&
      audio_playback_->requested_state() != audio::PlaybackState::Stopped) {
    const audio::PlaybackCommandReceipt receipt = audio_playback_->request_seek(playhead_);
    if (receipt.accepted) {
      audio_control_intent_ = AudioControlIntent::Seek;
      audio_command_version_ = receipt.version;
      audio_master_active_ = false;
      playback_timer_.start();
    } else {
      const QString failure = receipt.error.has_value()
                                  ? QString::fromStdString(receipt.error->message)
                                  : tr("the audio control queue rejected the seek");
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Audio device seek failed; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
    }
  }
  window_.timeline()->setPlayhead(playhead_);
  requestPreview();
}

void EditorController::setPlaybackRate(const double rate) {
  playback_rate_ = rate;
  if (std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
    audio_start_pending_ = false;
    if (audio_playback_ != nullptr && !audio_session_stale_ &&
        audio_playback_->requested_state() != audio::PlaybackState::Stopped) {
      const audio::PlaybackCommandReceipt receipt = audio_playback_->request_pause();
      if (receipt.accepted) {
        audio_control_intent_ = AudioControlIntent::Pause;
        audio_command_version_ = receipt.version;
        audio_master_active_ = false;
        playback_timer_.start();
        return;
      }
      stopAudioPlayback();
    }
    audio_master_active_ = false;
    playback_timer_.stop();
    return;
  }

  if (std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon()) {
    if (audio_playback_ != nullptr && !audio_session_stale_) {
      const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
      if (diagnostics.requested_state == audio::PlaybackState::Paused ||
          diagnostics.effective_state == audio::PlaybackState::Paused) {
        const audio::PlaybackCommandReceipt receipt = audio_playback_->request_resume();
        if (receipt.accepted) {
          audio_control_intent_ = AudioControlIntent::Resume;
          audio_command_version_ = receipt.version;
          audio_master_active_ = false;
          playback_timer_.start();
          return;
        }
        stopAudioPlayback();
      } else if (diagnostics.effective_state == audio::PlaybackState::Playing &&
                 diagnostics.playback.device_running) {
        audio_master_active_ = true;
        playback_timer_.start();
        return;
      }
    }
    if (startAudioMasterPlayback()) {
      playback_timer_.start();
      return;
    }
  } else {
    if (audio_playback_ != nullptr) {
      playhead_ = std::max<qint64>(audio_playback_->sample_counter(), 0);
      window_.timeline()->setPlayhead(playhead_);
      requestPreview();
    }
    stopAudioPlayback();
    if (!shuttle_silence_announced_) {
      shuttle_silence_announced_ = true;
      window_.showTransientMessage(tr("Shuttle playback outside 1× is silent"));
    }
  }
  playback_clock_.restart();
  playback_timer_.start();
}

void EditorController::advancePlayback() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    stopAudioPlayback();
    playback_timer_.stop();
    return;
  }
  const qint64 end = std::max<qint64>(toUiTime(edit::sequenceDuration(*sequence)), 0);
  bool audio_clock_applied = false;

  if (audio_start_pending_) {
    bool ready_to_replace = audio_playback_ == nullptr;
    if (audio_playback_ != nullptr) {
      const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
      ready_to_replace = diagnostics.requested_state == audio::PlaybackState::Stopped &&
                         diagnostics.latest_status != audio::PlaybackCommandStatus::Pending &&
                         diagnostics.playback.state == audio::PlaybackState::Stopped;
    }
    if (ready_to_replace) {
      audio_playback_.reset();
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      audio_start_pending_ = false;
      if (std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon() &&
          startAudioMasterPlayback()) {
        return;
      }
      playback_clock_.restart();
    } else {
      return;
    }
  }

  if (audio_playback_ != nullptr && audio_control_intent_ != AudioControlIntent::None) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const bool matching_result = diagnostics.latest_result_version == audio_command_version_;
    const bool pending =
        matching_result && diagnostics.latest_status == audio::PlaybackCommandStatus::Pending;
    if (pending) {
      if (audio_control_intent_ == AudioControlIntent::Pause) {
        playhead_ = std::clamp<qint64>(diagnostics.playback.sample_counter, 0, end);
        window_.timeline()->setPlayhead(playhead_);
        requestPreview();
      }
      return;
    }

    if (matching_result && diagnostics.latest_status == audio::PlaybackCommandStatus::Failed) {
      const QString failure = diagnostics.latest_error.has_value()
                                  ? QString::fromStdString(diagnostics.latest_error->message)
                                  : tr("the audio control operation failed");
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Realtime audio stopped; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
      if (std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
        playback_timer_.stop();
        return;
      }
    } else if (matching_result) {
      const AudioControlIntent completed_intent = audio_control_intent_;
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      playhead_ = std::clamp<qint64>(diagnostics.playback.sample_counter, 0, end);
      if (diagnostics.effective_state == audio::PlaybackState::Playing &&
          diagnostics.playback.device_running &&
          std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon()) {
        audio_master_active_ = true;
        audio_clock_applied = true;
        if (!audio_status_announced_) {
          audio_status_announced_ = true;
          window_.showTransientMessage(
              tr("Realtime 48 kHz audio is the latency-compensated playback master clock"));
        }
      } else {
        audio_master_active_ = false;
        if (completed_intent == AudioControlIntent::Pause ||
            std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
          window_.timeline()->setPlayhead(playhead_);
          requestPreview();
          playback_timer_.stop();
          return;
        }
      }
    }
  }

  if (audio_master_active_ && audio_playback_ != nullptr) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const audio::PlaybackDiagnostics& playback = diagnostics.playback;
    if (playback.state == audio::PlaybackState::Failed || !playback.device_running) {
      const QString failure = playback.last_error.empty()
                                  ? tr("the audio device stopped")
                                  : QString::fromStdString(playback.last_error);
      playhead_ = std::clamp<qint64>(playback.sample_counter, 0, end);
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Realtime audio stopped; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
    } else {
      playhead_ = std::clamp<qint64>(playback.sample_counter, 0, end);
      audio_clock_applied = true;
      if (playback.xrun_count > last_audio_xrun_count_) {
        if (last_audio_xrun_count_ == 0) {
          window_.showTransientMessage(
              tr("Audio playback underrun detected; consider proxies or a larger audio buffer"),
              8'000);
        }
        last_audio_xrun_count_ = playback.xrun_count;
      }
    }
  }

  if (!audio_clock_applied) {
    const qint64 elapsed_ms = playback_clock_.restart();
    const auto delta =
        static_cast<qint64>(std::llround(playback_rate_ * static_cast<double>(kUiTimescale) *
                                         static_cast<double>(elapsed_ms) / 1000.0));
    playhead_ = std::clamp(playhead_ + delta, qint64{0}, end);
  }
  window_.timeline()->setPlayhead(playhead_);
  requestPreview();
  if ((playback_rate_ > 0.0 && playhead_ >= end) || (playback_rate_ < 0.0 && playhead_ <= 0)) {
    stopAudioPlayback();
    playback_timer_.stop();
    playback_rate_ = 0.0;
  }
}

void EditorController::seekCaption(const int visibleRow) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index < sequence->captions.size()) {
    seek(toUiTime(sequence->captions[caption_index].range.start));
  }
}

void EditorController::addCaptionAtPlayhead() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  edit::Caption caption;
  caption.range = edit::TimeRange(playheadTime(), edit::Time(2, 1));
  caption.text = "New caption";
  if (apply(edit::EditCommand{.operation = edit::AddCaptionCommand{.sequence_id = sequence->id,
                                                                   .caption = std::move(caption)},
                              .coalescing_key = {}},
            tr("Could not add caption"))) {
    refreshCaptionView();
  }
}

void EditorController::removeCaption(const int visibleRow) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index >= sequence->captions.size()) {
    return;
  }
  (void)apply(edit::EditCommand{.operation =
                                    edit::RemoveCaptionCommand{
                                        .sequence_id = sequence->id,
                                        .caption_id = sequence->captions[caption_index].id},
                                .coalescing_key = {}},
              tr("Could not delete caption"));
}

void EditorController::updateCaptionText(const int visibleRow, const QString& text) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    refreshCaptionView();
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index >= sequence->captions.size()) {
    refreshCaptionView();
    return;
  }
  const QString normalized = text.trimmed();
  if (normalized.isEmpty()) {
    window_.showTransientMessage(tr("Caption text cannot be empty"));
    refreshCaptionView();
    return;
  }
  edit::Caption caption = sequence->captions[caption_index];
  if (caption.text == normalized.toStdString()) {
    return;
  }
  caption.text = normalized.toStdString();
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateCaptionCommand{.sequence_id = sequence->id,
                                                                .caption = std::move(caption)},
                        .coalescing_key = {}},
      tr("Could not update caption"));
}

void EditorController::searchTranscript(const QString& query) {
  caption_search_ = query.trimmed();
  refreshCaptionView();
}

void EditorController::updateSelectedClipProperty(const QString& parameterId,
                                                  const QVariant& value) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !selected_clip_.has_value()) {
    window_.showTransientMessage(tr("Select a clip before changing its properties"));
    return;
  }
  const edit::Clip* selected = edit::findClip(*sequence, *selected_clip_);
  if (selected == nullptr) {
    selected_clip_.reset();
    refreshInspectorView();
    window_.showTransientMessage(tr("The selected clip is no longer available"));
    return;
  }

  const std::string coalescing_key =
      "inspector:" + selected->id.toString() + ":" + parameterId.toStdString();
  if (parameterId == QStringLiteral("positionX") || parameterId == QStringLiteral("positionY") ||
      parameterId == QStringLiteral("scale") || parameterId == QStringLiteral("scaleX") ||
      parameterId == QStringLiteral("scaleY") || parameterId == QStringLiteral("rotation") ||
      parameterId == QStringLiteral("opacity") || parameterId == QStringLiteral("anchorX") ||
      parameterId == QStringLiteral("anchorY") || parameterId == QStringLiteral("cropLeft") ||
      parameterId == QStringLiteral("cropTop") || parameterId == QStringLiteral("cropRight") ||
      parameterId == QStringLiteral("cropBottom")) {
    edit::Transform transform = selected->transform;
    if (parameterId == QStringLiteral("positionX")) {
      transform.position.x = value.toDouble();
    } else if (parameterId == QStringLiteral("positionY")) {
      transform.position.y = value.toDouble();
    } else if (parameterId == QStringLiteral("scale")) {
      const double uniform_scale = value.toDouble() / 100.0;
      transform.scale = {uniform_scale, uniform_scale};
    } else if (parameterId == QStringLiteral("scaleX")) {
      transform.scale.x = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("scaleY")) {
      transform.scale.y = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("rotation")) {
      transform.rotation_degrees = value.toDouble();
    } else if (parameterId == QStringLiteral("opacity")) {
      transform.opacity = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("anchorX")) {
      transform.anchor_x = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("anchorY")) {
      transform.anchor_y = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropLeft")) {
      transform.crop_left = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropTop")) {
      transform.crop_top = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropRight")) {
      transform.crop_right = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropBottom")) {
      transform.crop_bottom = value.toDouble() / 100.0;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipTransformCommand{.sequence_id = sequence->id,
                                                                     .clip_id = selected->id,
                                                                     .transform = transform},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip"));
    return;
  }

  if (parameterId == QStringLiteral("blendMode")) {
    const QString mode_id = value.toString();
    edit::BlendMode mode = edit::BlendMode::Normal;
    if (mode_id == QStringLiteral("add")) {
      mode = edit::BlendMode::Add;
    } else if (mode_id == QStringLiteral("multiply")) {
      mode = edit::BlendMode::Multiply;
    } else if (mode_id == QStringLiteral("screen")) {
      mode = edit::BlendMode::Screen;
    } else if (mode_id == QStringLiteral("overlay")) {
      mode = edit::BlendMode::Overlay;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipBlendModeCommand{.sequence_id = sequence->id,
                                                                     .clip_id = selected->id,
                                                                     .blend_mode = mode},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip blend mode"));
    return;
  }

  if (parameterId == QStringLiteral("audioGain") || parameterId == QStringLiteral("audioPan") ||
      parameterId == QStringLiteral("fadeIn") || parameterId == QStringLiteral("fadeOut")) {
    double gain_db = selected->audio_gain_db;
    double pan = selected->audio_pan;
    edit::Time fade_in = selected->fade_in;
    edit::Time fade_out = selected->fade_out;
    if (parameterId == QStringLiteral("audioGain")) {
      gain_db = value.toDouble();
    } else if (parameterId == QStringLiteral("audioPan")) {
      pan = value.toDouble() / 100.0;
    } else {
      const edit::Time fade_samples(
          static_cast<std::int64_t>(std::llround(value.toDouble() * 48'000.0)), 48'000);
      if (parameterId == QStringLiteral("fadeIn")) {
        fade_in = fade_samples;
      } else {
        fade_out = fade_samples;
      }
    }
    (void)apply(
        edit::EditCommand{.operation =
                              edit::SetClipAudioPropertiesCommand{.sequence_id = sequence->id,
                                                                  .clip_id = selected->id,
                                                                  .gain_db = gain_db,
                                                                  .pan = pan,
                                                                  .fade_in = fade_in,
                                                                  .fade_out = fade_out},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip audio"));
  }
}

void EditorController::setAudioTrackMuted(const int trackIndex, const bool muted) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioStateCommand{.sequence_id = sequence->id,
                                                                       .track_id = track.id,
                                                                       .muted = muted,
                                                                       .solo = track.solo},
                          .coalescing_key = "mixer:" + track.id.toString() + ":mute"},
        tr("Could not update track mute"));
    return;
  }
  refreshMixerView();
}

void EditorController::setAudioTrackSolo(const int trackIndex, const bool soloed) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioStateCommand{.sequence_id = sequence->id,
                                                                       .track_id = track.id,
                                                                       .muted = track.muted,
                                                                       .solo = soloed},
                          .coalescing_key = "mixer:" + track.id.toString() + ":solo"},
        tr("Could not update track solo"));
    return;
  }
  refreshMixerView();
}

void EditorController::persistSnapshot(const std::string_view reason) {
  const auto project = editor_->projectAt(editor_->revision());
  const project_codec::ProjectBytes bytes = project_codec::serialize_project(*project);
  const auto metadata = store_->metadata();
  store_->append_command("project.snapshot.v1", std::span<const std::byte>(bytes),
                         metadata.head_revision);
  store_->update_heartbeat();
  (void)reason;
}

bool EditorController::apply(edit::EditCommand command, const QString& failureContext) {
  const auto result = editor_->apply(std::move(command), editor_->revision());
  if (!result) {
    window_.showTransientMessage(QStringLiteral("%1: %2").arg(
        failureContext, QString::fromStdString(result.error().message)));
    return false;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("edit.command");
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    Q_UNUSED(rollback);
    showError(tr("Project write failed"), QString::fromUtf8(exception.what()));
    refreshViews();
    return false;
  }
  setDirty(true);
  refreshViews();
  return true;
}

bool EditorController::applyBatch(std::vector<edit::EditCommand> commands,
                                  const QString& failureContext) {
  if (commands.empty()) {
    return true;
  }
  const std::string batch_key = "batch:" + edit::EntityId::generate().toString();
  for (edit::EditCommand& command : commands) {
    command.coalescing_key = batch_key;
  }
  std::size_t applied_count = 0;
  for (edit::EditCommand& command : commands) {
    const auto result = editor_->apply(std::move(command), editor_->revision());
    if (!result) {
      if (applied_count > 0) {
        const auto rollback = editor_->undo(editor_->revision());
        (void)rollback;
      }
      window_.showTransientMessage(QStringLiteral("%1: %2").arg(
          failureContext, QString::fromStdString(result.error().message)));
      refreshViews();
      return false;
    }
    ++applied_count;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("edit.batch");
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    (void)rollback;
    showError(tr("Project write failed"), QString::fromUtf8(exception.what()));
    refreshViews();
    return false;
  }
  setDirty(true);
  refreshViews();
  return true;
}

void EditorController::refreshViews() {
  if (!editor_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  window_.setProjectDisplayName(QString::fromStdString(project->name));
  const edit::Sequence* sequence = currentSequence();
  window_.deliverPanel()->setExportEnabled(
      export_in_flight_ || (sequence != nullptr && !edit::sequenceDuration(*sequence).isZero()));
  refreshMediaView();
  refreshTimelineView();
  refreshInspectorView();
  refreshMixerView();
  refreshCaptionView();
  requestPreview();
}

void EditorController::refreshMediaView() {
  const auto project = editor_->projectAt(editor_->revision());
  QVector<MediaItemView> items;
  items.reserve(static_cast<qsizetype>(project->assets.size()));
  for (const edit::Asset& asset : project->assets) {
    const auto container = asset.metadata.find("container");
    QString format =
        container == asset.metadata.end() ? QString{} : QString::fromStdString(container->second);
    if (asset.has_video) {
      format += QStringLiteral(" %1×%2").arg(asset.width).arg(asset.height);
    }
    const auto proxy = std::find_if(
        imported_assets_.begin(), imported_assets_.end(),
        [&asset](const assets::AssetRecord& record) { return record.id == asset.id.toString(); });
    const bool proxy_available =
        proxy != imported_assets_.end() && proxy->proxy.has_value() && proxy->proxy->complete;
    const bool proxy_recommended =
        proxy != imported_assets_.end() && assets::AssetService::should_recommend_proxy(*proxy);
    items.push_back({
        .id = QString::fromStdString(asset.id.toString()),
        .displayName = QString::fromStdString(asset.name),
        .filePath = qStringFromPath(pathFromUtf8String(asset.source_uri)),
        .durationText = durationText(asset.duration),
        .formatText = format.trimmed(),
        .offline = !QFileInfo::exists(qStringFromPath(pathFromUtf8String(asset.source_uri))),
        .proxyAvailable = proxy_available,
        .proxyRecommended = proxy_recommended,
        .proxyGenerating = proxy_jobs_.contains(asset.id.toString()),
    });
  }
  window_.setMediaItems(items);
}

void EditorController::refreshTimelineView() {
  const auto project = editor_->projectAt(editor_->revision());
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    window_.setTimelineView(kUiTimescale * 10, kUiTimescale, {}, {});
    return;
  }
  QVector<TimelineTrackView> tracks;
  QVector<TimelineClipView> clips;
  tracks.reserve(static_cast<qsizetype>(sequence->tracks.size()));
  std::size_t track_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    tracks.push_back({
        .id = QString::fromStdString(track.id.toString()),
        .displayName = QString::fromStdString(track.name),
        .kind = uiTrackKind(track.kind),
        .muted = track.muted,
        .soloed = track.solo,
        .locked = track.locked,
    });
    for (const edit::Clip& clip : track.clips) {
      const auto proxy = std::find_if(
          imported_assets_.begin(), imported_assets_.end(),
          [&clip](const assets::AssetRecord& item) { return item.id == clip.asset_id.toString(); });
      clips.push_back({
          .id = QString::fromStdString(clip.id.toString()),
          .displayName = QString::fromStdString(clip.name),
          .trackIndex = static_cast<int>(track_index),
          .start = toUiTime(clip.timeline_range.start),
          .duration = std::max<qint64>(1, toUiTime(clip.timeline_range.duration)),
          .color = colorForTrack(track.kind, track_index),
          .selected = selected_clip_.has_value() && *selected_clip_ == clip.id,
          .offline = edit::findAsset(*project, clip.asset_id) == nullptr,
          .proxy =
              proxy != imported_assets_.end() && proxy->proxy.has_value() && proxy->proxy->complete,
      });
    }
    ++track_index;
  }
  const qint64 duration =
      std::max<qint64>(toUiTime(edit::sequenceDuration(*sequence)), kUiTimescale * 10);
  window_.setTimelineView(duration, kUiTimescale, std::move(tracks), std::move(clips));
  window_.timeline()->setFrameRate(sequence->frame_rate.numerator(),
                                   sequence->frame_rate.denominator());
  window_.timeline()->setPlayhead(playhead_);
  window_.programViewer()->setTimecode(timecodeText(playhead_, sequence->frame_rate));
}

void EditorController::refreshInspectorView() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !selected_clip_.has_value()) {
    window_.inspector()->clearSelection();
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *selected_clip_);
  if (clip == nullptr) {
    selected_clip_.reset();
    window_.inspector()->clearSelection();
    return;
  }

  window_.inspector()->setSelectionName(QString::fromStdString(clip->name));
  window_.inspector()->setClipCapabilities(clip->kind == edit::ClipKind::Video ||
                                               clip->kind == edit::ClipKind::Title,
                                           clip->kind == edit::ClipKind::Audio);
  window_.inspector()->setParameter(QStringLiteral("positionX"), clip->transform.position.x);
  window_.inspector()->setParameter(QStringLiteral("positionY"), clip->transform.position.y);
  window_.inspector()->setParameter(QStringLiteral("scale"), clip->transform.scale.x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("scaleX"), clip->transform.scale.x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("scaleY"), clip->transform.scale.y * 100.0);
  window_.inspector()->setParameter(QStringLiteral("rotation"), clip->transform.rotation_degrees);
  window_.inspector()->setParameter(QStringLiteral("opacity"), clip->transform.opacity * 100.0);
  window_.inspector()->setParameter(QStringLiteral("anchorX"), clip->transform.anchor_x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("anchorY"), clip->transform.anchor_y * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropLeft"), clip->transform.crop_left * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropTop"), clip->transform.crop_top * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropRight"),
                                    clip->transform.crop_right * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropBottom"),
                                    clip->transform.crop_bottom * 100.0);
  const QString blend_mode = [clip] {
    switch (clip->blend_mode) {
    case edit::BlendMode::Add:
      return QStringLiteral("add");
    case edit::BlendMode::Multiply:
      return QStringLiteral("multiply");
    case edit::BlendMode::Screen:
      return QStringLiteral("screen");
    case edit::BlendMode::Overlay:
      return QStringLiteral("overlay");
    case edit::BlendMode::Normal:
    default:
      return QStringLiteral("normal");
    }
  }();
  window_.inspector()->setParameter(QStringLiteral("blendMode"), blend_mode);
  window_.inspector()->setParameter(QStringLiteral("audioGain"), clip->audio_gain_db);
  window_.inspector()->setParameter(QStringLiteral("audioPan"), clip->audio_pan * 100.0);
  window_.inspector()->setParameter(QStringLiteral("fadeIn"),
                                    static_cast<double>(clip->fade_in.value()) /
                                        static_cast<double>(clip->fade_in.timescale()));
  window_.inspector()->setParameter(QStringLiteral("fadeOut"),
                                    static_cast<double>(clip->fade_out.value()) /
                                        static_cast<double>(clip->fade_out.timescale()));
}

void EditorController::refreshMixerView() {
  QVector<desktop_ui::AudioTrackView> tracks;
  const edit::Sequence* sequence = currentSequence();
  if (sequence != nullptr) {
    for (const edit::Track& track : sequence->tracks) {
      if (track.kind == edit::TrackKind::Audio) {
        tracks.push_back({.displayName = QString::fromStdString(track.name),
                          .muted = track.muted,
                          .soloed = track.solo});
      }
    }
  }
  window_.audioMixer()->setTracks(tracks);
}

void EditorController::refreshCaptionView() {
  const edit::Sequence* sequence = currentSequence();
  visible_caption_indices_.clear();
  QStringList timecodes;
  QStringList text;
  if (sequence == nullptr) {
    window_.captionsPanel()->setCaptionRows(timecodes, text);
    return;
  }

  if (caption_search_.isEmpty()) {
    visible_caption_indices_.reserve(sequence->captions.size());
    for (std::size_t index = 0; index < sequence->captions.size(); ++index) {
      visible_caption_indices_.push_back(index);
    }
  } else {
    const auto document = caption_service::fromEditCaptions(
        sequence->captions, caption_service::SubtitleFormat::WebVtt);
    auto matches = caption_service::search(document, caption_search_.toStdString());
    if (matches) {
      for (const auto& hit : matches.value()) {
        if (visible_caption_indices_.empty() || visible_caption_indices_.back() != hit.cue_index) {
          visible_caption_indices_.push_back(hit.cue_index);
        }
      }
    }
  }

  timecodes.reserve(static_cast<qsizetype>(visible_caption_indices_.size()));
  text.reserve(static_cast<qsizetype>(visible_caption_indices_.size()));
  for (const std::size_t index : visible_caption_indices_) {
    if (index >= sequence->captions.size()) {
      continue;
    }
    const edit::Caption& caption = sequence->captions[index];
    timecodes.push_back(timecodeText(toUiTime(caption.range.start), sequence->frame_rate) +
                        QStringLiteral("  →  ") +
                        timecodeText(toUiTime(caption.range.end()), sequence->frame_rate));
    text.push_back(QString::fromStdString(caption.text));
  }
  window_.captionsPanel()->setCaptionRows(timecodes, text);
}

void EditorController::rebuildPlaybackRegistry() {
  for (const edit::EntityId& id : registered_playback_assets_) {
    (void)playback_registry_->unregister_asset(id);
    frame_provider_->invalidate(id);
  }
  registered_playback_assets_.clear();
  for (const edit::EntityId& id : registered_audio_assets_) {
    (void)audio_registry_->unregister_asset(id);
  }
  registered_audio_assets_.clear();
  if (!editor_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  for (const edit::Asset& asset : project->assets) {
    playback::AssetPlaybackSources sources{
        .original = {.path = pathFromUtf8String(asset.source_uri), .video_stream_index = -1},
        .proxy = std::nullopt};
    if (playback_registry_->register_asset(asset.id, std::move(sources))) {
      registered_playback_assets_.push_back(asset.id);
    }
    if (asset.has_audio) {
      if (audio_registry_->register_original(
              asset.id,
              audio_render::OriginalAudioMedia{.path = pathFromUtf8String(asset.source_uri),
                                               .audio_stream_index = -1})) {
        registered_audio_assets_.push_back(asset.id);
      }
    }
  }
}

bool EditorController::startAudioMasterPlayback() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !audio::MiniaudioOutputDevice::available()) {
    if (!audio_fallback_announced_) {
      audio_fallback_announced_ = true;
      window_.showTransientMessage(
          tr("The realtime audio-device adapter is unavailable; playback will be silent"), 8'000);
    }
    return false;
  }

  if (audio_playback_ != nullptr) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const bool stopped = diagnostics.requested_state == audio::PlaybackState::Stopped &&
                         diagnostics.latest_status != audio::PlaybackCommandStatus::Pending &&
                         diagnostics.playback.state == audio::PlaybackState::Stopped;
    if (!stopped) {
      static_cast<void>(audio_playback_->request_stop());
      audio_master_active_ = false;
      audio_session_stale_ = true;
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      audio_start_pending_ = true;
      return true;
    }
    audio_playback_.reset();
  }

  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    return false;
  }

  try {
    edit::TimelineSnapshot snapshot = std::move(snapshot_result).value();
    const std::int64_t end_sample =
        snapshot.duration()
            .rescaledTo(audio::kPlaybackAudioFormat.sample_rate, edit::RoundingMode::Ceil)
            .value();
    if (playhead_ < 0 || playhead_ >= end_sample) {
      return false;
    }

    auto timeline_renderer = std::make_shared<audio_render::TimelineAudioRenderer>(audio_registry_);
    auto provider = std::make_shared<TimelinePlaybackAudioProvider>(
        std::move(timeline_renderer), std::move(snapshot), end_sample);
    audio::RealtimePlaybackConfiguration configuration{
        .ring_capacity_frames = 192'000,
        .render_block_frames = 24'000,
        .prefill_frames = 48'000,
        .prefill_timeout = std::chrono::milliseconds(2'000),
    };
    auto candidate = std::make_unique<audio::AsyncRealtimeAudioPlayback>(
        std::move(provider), configuration, std::make_unique<audio::MiniaudioOutputDevice>());
    const audio::PlaybackCommandReceipt receipt = candidate->request_start(playhead_);
    if (!receipt.accepted) {
      const QString failure = receipt.error.has_value()
                                  ? QString::fromStdString(receipt.error->message)
                                  : tr("the audio control queue rejected the start request");
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Could not start realtime audio; using silent timer playback: %1").arg(failure),
            8'000);
      }
      return false;
    }

    audio_playback_ = std::move(candidate);
    audio_master_active_ = false;
    audio_start_pending_ = false;
    audio_session_stale_ = false;
    audio_control_intent_ = AudioControlIntent::Start;
    audio_command_version_ = receipt.version;
    last_audio_xrun_count_ = 0;
    return true;
  } catch (const std::exception& exception) {
    if (!audio_fallback_announced_) {
      audio_fallback_announced_ = true;
      window_.showTransientMessage(
          tr("Could not prepare realtime audio; using silent timer playback: %1")
              .arg(QString::fromUtf8(exception.what())),
          8'000);
    }
    return false;
  }
}

void EditorController::stopAudioPlayback() noexcept {
  audio_master_active_ = false;
  audio_start_pending_ = false;
  audio_session_stale_ = true;
  audio_control_intent_ = AudioControlIntent::None;
  audio_command_version_ = 0;
  last_audio_xrun_count_ = 0;
  if (audio_playback_ != nullptr) {
    static_cast<void>(audio_playback_->request_stop());
  }
}

void EditorController::requestPreview() {
  if (!editor_ || !renderer_ || !frame_provider_) {
    return;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr ||
      std::none_of(sequence->tracks.begin(), sequence->tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Video && !track.clips.empty();
      })) {
    ++preview_epoch_;
    renderer_->begin_epoch(preview_epoch_);
    if (gpu_timeline_renderer_ != nullptr) {
      gpu_timeline_renderer_->begin_epoch(preview_epoch_);
    }
    frame_provider_->begin_epoch(preview_epoch_);
    gpu_preview_active_ = false;
    window_.programViewer()->clearFrame();
    return;
  }
  requested_preview_position_ = playhead_;
  ++preview_epoch_;
  renderer_->begin_epoch(preview_epoch_);
  if (gpu_timeline_renderer_ != nullptr) {
    gpu_timeline_renderer_->begin_epoch(preview_epoch_);
  }
  frame_provider_->begin_epoch(preview_epoch_);
  if (!preview_in_flight_) {
    launchPreviewRequest();
  }
}

void EditorController::launchPreviewRequest() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    window_.programViewer()->clearFrame();
    return;
  }
  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    window_.programViewer()->clearFrame();
    return;
  }
  const std::uint64_t epoch = preview_epoch_;
  const edit::Time requested_time(requested_preview_position_,
                                  static_cast<std::uint32_t>(kUiTimescale));
  auto snapshot = std::move(snapshot_result).value();
  const auto renderer = renderer_;
  const auto gpu_renderer = gpu_fallback_latched_ ? nullptr : gpu_renderer_;
  const auto gpu_timeline_renderer = gpu_fallback_latched_ ? nullptr : gpu_timeline_renderer_;
  preview_in_flight_ = true;

  auto* watcher = new QFutureWatcher<PreviewOutcome>(this);
  connect(watcher, &QFutureWatcher<PreviewOutcome>::finished, this, [this, watcher] {
    const PreviewOutcome outcome = watcher->result();
    watcher->deleteLater();
    preview_in_flight_ = false;
    if (outcome.epoch == preview_epoch_) {
      gpu_preview_active_ = outcome.gpu_used;
      if (outcome.gpu_failed && !gpu_fallback_latched_) {
        gpu_fallback_latched_ = true;
        gpu_timeline_renderer_.reset();
        gpu_renderer_.reset();
        window_.programViewer()->setTitle(tr("Program · CPU fallback"));
        window_.showTransientMessage(
            tr("GPU preview failed; using the CPU renderer for this session: %1")
                .arg(outcome.gpu_diagnostic),
            8'000);
      } else if (outcome.gpu_used) {
        window_.programViewer()->setTitle(tr("Program · %1 GPU").arg(outcome.gpu_backend));
        if (!gpu_status_announced_) {
          gpu_status_announced_ = true;
          window_.showTransientMessage(
              tr("GPU preview active through libplacebo (%1)").arg(outcome.gpu_backend));
        }
      } else if (!outcome.gpu_used && !outcome.gpu_failed && !outcome.gpu_diagnostic.isEmpty() &&
                 outcome.gpu_backend != QStringLiteral("CPU")) {
        // Unsupported timeline features fall back for this frame only. Keep
        // the ready backend visible without claiming that the displayed image
        // was GPU-rendered.
        window_.programViewer()->setTitle(
            tr("Program · CPU frame · %1 GPU ready").arg(outcome.gpu_backend));
      }
      if (!outcome.image.isNull()) {
        window_.programViewer()->setFrame(outcome.image);
      } else if (!outcome.error.isEmpty()) {
        window_.programViewer()->clearFrame();
        window_.showTransientMessage(outcome.error);
      }
    }
    if (outcome.epoch != preview_epoch_) {
      launchPreviewRequest();
    }
  });
  watcher->setFuture(QtConcurrent::run([renderer, gpu_renderer, gpu_timeline_renderer,
                                        snapshot = std::move(snapshot), requested_time,
                                        epoch]() mutable {
    const render::PreviewProfile profile{
        .scale = render::PreviewScale::Half, .bypass_expensive_effects = true, .use_proxies = true};
    const auto cpu_fallback = [&](QString backend, QString diagnostic,
                                  const bool gpu_failed) -> PreviewOutcome {
      auto result = renderer->request_frame(snapshot, requested_time, profile, epoch);
      if (!result) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QString::fromStdString(result.error->message),
                              .gpu_backend = std::move(backend),
                              .gpu_diagnostic = std::move(diagnostic),
                              .gpu_used = false,
                              .gpu_failed = gpu_failed};
      }
      const auto* cpu =
          std::get_if<std::shared_ptr<const render::CpuFrame>>(&result.value->storage);
      if (cpu == nullptr || !*cpu) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QObject::tr("CPU preview returned unsupported storage"),
                              .gpu_backend = std::move(backend),
                              .gpu_diagnostic = std::move(diagnostic),
                              .gpu_used = false,
                              .gpu_failed = gpu_failed};
      }
      return PreviewOutcome{.epoch = epoch,
                            .image = EditorController::displayImage(**cpu),
                            .error = {},
                            .gpu_backend = std::move(backend),
                            .gpu_diagnostic = std::move(diagnostic),
                            .gpu_used = false,
                            .gpu_failed = gpu_failed};
    };

    if (gpu_renderer != nullptr && gpu_timeline_renderer != nullptr) {
      const render::GpuCapabilities capabilities = gpu_renderer->capabilities();
      const QString backend = gpuBackendName(capabilities.backend);
      if (!capabilities.available() || !capabilities.offscreen_rendering) {
        return cpu_fallback(backend, QString::fromStdString(capabilities.diagnostic), true);
      }

      auto gpu_frame =
          gpu_timeline_renderer->request_frame(snapshot, requested_time, profile, epoch);
      if (gpu_frame) {
        auto downloaded = gpu_renderer->download(*gpu_frame.value);
        if (downloaded) {
          const auto* gpu_cpu =
              std::get_if<std::shared_ptr<const render::CpuFrame>>(&downloaded.value->storage);
          if (gpu_cpu != nullptr && *gpu_cpu) {
            return PreviewOutcome{.epoch = epoch,
                                  .image = EditorController::displayImage(**gpu_cpu),
                                  .error = {},
                                  .gpu_backend = backend,
                                  .gpu_diagnostic = {},
                                  .gpu_used = true,
                                  .gpu_failed = false};
          }
        }
        const QString failure = downloaded.error.has_value()
                                    ? QString::fromStdString(downloaded.error->message)
                                    : QObject::tr("GPU readback returned no CPU frame");
        return cpu_fallback(backend, failure, true);
      }

      const render::RenderError& failure = *gpu_frame.error;
      if (failure.code == render::RenderErrorCode::StaleRequest) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QString::fromStdString(failure.message),
                              .gpu_backend = backend,
                              .gpu_diagnostic = {},
                              .gpu_used = false,
                              .gpu_failed = false};
      }
      const bool should_latch = failure.code == render::RenderErrorCode::GpuUnavailable ||
                                failure.code == render::RenderErrorCode::GpuUploadFailed ||
                                failure.code == render::RenderErrorCode::GpuRenderFailed ||
                                failure.code == render::RenderErrorCode::GpuDownloadFailed ||
                                failure.code == render::RenderErrorCode::GpuDeviceLost;
      return cpu_fallback(backend, QString::fromStdString(failure.message), should_latch);
    }
    return cpu_fallback(QStringLiteral("CPU"), {}, false);
  }));
}

QImage EditorController::displayImage(const render::CpuFrame& frame) {
  QImage image(frame.width(), frame.height(), QImage::Format_RGBA8888);
  if (image.isNull()) {
    return {};
  }
  const auto encode = [](float linear) {
    linear = std::max(0.0F, linear);
    const float encoded =
        linear <= 0.0031308F ? linear * 12.92F : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
    return static_cast<uchar>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
  };
  for (int y = 0; y < frame.height(); ++y) {
    uchar* output = image.scanLine(y);
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      const float alpha = std::clamp(pixel[3], 0.0F, 1.0F);
      const float inverse_alpha = alpha > 0.0F ? 1.0F / alpha : 0.0F;
      output[(x * 4) + 0] = encode(pixel[0] * inverse_alpha);
      output[(x * 4) + 1] = encode(pixel[1] * inverse_alpha);
      output[(x * 4) + 2] = encode(pixel[2] * inverse_alpha);
      output[(x * 4) + 3] = static_cast<uchar>(std::lround(alpha * 255.0F));
    }
  }
  return image;
}

const edit::Sequence* EditorController::currentSequence() const {
  if (!editor_) {
    return nullptr;
  }
  const auto project = editor_->projectAt(editor_->revision());
  return project->sequences.empty() ? nullptr : &project->sequences.front();
}

const edit::Asset* EditorController::assetByTextId(const QString& text) const {
  const auto id = parseId(text);
  if (!id.has_value()) {
    return nullptr;
  }
  const auto project = editor_->projectAt(editor_->revision());
  return edit::findAsset(*project, *id);
}

edit::Time EditorController::playheadTime() const {
  return edit::Time(playhead_, static_cast<std::uint32_t>(kUiTimescale));
}

void EditorController::setDirty(const bool dirty) {
  dirty_ = dirty;
  window_.setProjectDirty(dirty_);
}

void EditorController::showError(const QString& title, const QString& message) {
  QMessageBox::critical(&window_, title, message);
}

} // namespace video_editor::app
