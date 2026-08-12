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
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
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

QString gapKey(const edit::EntityId& trackId, const edit::TimeRange& range) {
  return QStringLiteral("%1:%2/%3:%4/%5")
      .arg(QString::fromStdString(trackId.toString()))
      .arg(range.start.value())
      .arg(range.start.timescale())
      .arg(range.duration.value())
      .arg(range.duration.timescale());
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

std::optional<std::uint32_t> projectSnapshotSchema(const store::JournalEntry& entry) {
  if (entry.command_type == "project.snapshot.v1") {
    return 1U;
  }
  if (entry.command_type == "project.snapshot.v2") {
    return 2U;
  }
  return std::nullopt;
}

const store::JournalEntry* latestProjectSnapshot(const std::vector<store::JournalEntry>& entries) {
  const store::JournalEntry* result = nullptr;
  for (const auto& entry : entries) {
    const auto schema = projectSnapshotSchema(entry);
    if (!schema.has_value()) {
      if (entry.command_type.starts_with("project.snapshot.v")) {
        throw std::runtime_error("Project journal contains an unsupported snapshot version");
      }
      continue;
    }
    if (entry.payload_schema_version != *schema) {
      throw std::runtime_error("Project snapshot journal type does not match its payload schema");
    }
    result = &entry;
  }
  return result;
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
  connect(&window_, &desktop_ui::EditorWindow::keyframeToggleRequested, this,
          &EditorController::toggleSelectedClipKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::addTitleRequested, this,
          &EditorController::addTitleClip);
  connect(&window_, &desktop_ui::EditorWindow::transitionActivated, this,
          [this](const QString& transitionId) { setTransitionSelection(transitionId); });
  connect(&window_, &desktop_ui::EditorWindow::transitionDurationEdited, this,
          &EditorController::updateTransitionDuration);
  connect(&window_, &desktop_ui::EditorWindow::transitionRemoved, this,
          &EditorController::removeTransition);
  connect(&window_, &desktop_ui::EditorWindow::transitionPresetChanged, this,
          &EditorController::changeTransitionPreset);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::muteToggled, this,
          &EditorController::setAudioTrackMuted);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::soloToggled, this,
          &EditorController::setAudioTrackSolo);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::gainEdited, this,
          &EditorController::setAudioTrackGain);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::panEdited, this,
          &EditorController::setAudioTrackPan);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipActivated, this,
          [this](const QString& clipId) { setClipSelection({clipId}, clipId); });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipSelectionChanged, this,
          &EditorController::setClipSelection);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipBatchEditCommitted, this,
          [this](const QStringList& clipIds, const int destinationTrackIndex,
                 const qint64 startDelta, const qint64 durationDelta,
                 const desktop_ui::TimelineWidget::EditMode mode,
                 const desktop_ui::TimelineWidget::EditIntent intent,
                 const desktop_ui::TimelineSnapResult& snap) {
            Q_UNUSED(snap)
            commitTimelineBatchEdit(clipIds, destinationTrackIndex, startDelta, durationDelta,
                                    static_cast<int>(mode), static_cast<int>(intent));
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::frameNudgeRequested, this,
          [this](const QStringList& clipIds, const int frameCount,
                 const desktop_ui::TimelineWidget::EditIntent intent) {
            nudgeTimelineSelection(clipIds, frameCount, static_cast<int>(intent));
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerSelectionChanged, this,
          &EditorController::selectMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerAddRequested, this,
          &EditorController::addMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerMoveCommitted, this,
          [this](const QString& markerId, const qint64 start,
                 const desktop_ui::TimelineSnapResult& snap) {
            moveMarker(markerId, snap.snapped() ? snap.time : start);
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerRenameRequested, this,
          &EditorController::renameMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerRemoveRequested, this,
          &EditorController::removeMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::gapSelectionChanged, this,
          &EditorController::selectGap);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::closeGapRequested, this,
          &EditorController::closeGap);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackAddRequested, this,
          [this](const desktop_ui::TrackKind kind) { addTrack(static_cast<int>(kind)); });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackRenameRequested, this,
          &EditorController::renameTrack);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackReorderRequested, this,
          &EditorController::reorderTrack);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackLockToggled, this,
          &EditorController::setTrackLocked);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackVisibilityToggled, this,
          &EditorController::setTrackVisible);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackTargetToggled, this,
          &EditorController::setTrackTargeted);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackRemoveRequested, this,
          &EditorController::removeTrack);
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
    const store::JournalEntry* snapshot_entry = latestProjectSnapshot(commands);
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
    const store::JournalEntry* snapshot_entry = latestProjectSnapshot(commands);
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
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_marker_id_.reset();
  selected_gap_key_.clear();
  timeline_time_scale_ = static_cast<std::uint32_t>(kUiTimescale);
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
    if (!track.locked && track.targeted && track.kind == edit::TrackKind::Video &&
        !video_track_id.has_value()) {
      video_track_id = track.id;
    }
    if (!track.locked && track.targeted && track.kind == edit::TrackKind::Audio &&
        !audio_track_id.has_value()) {
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
    window_.showTransientMessage(tr("No unlocked targeted video track is available"));
    return;
  }
  if (asset_copy.has_audio && !prepare_insert(audio_track_id, edit::ClipKind::Audio)) {
    window_.showTransientMessage(tr("No unlocked targeted audio track is available"));
    return;
  }
  if (applyBatch(std::move(commands), tr("Could not insert media at the playhead"))) {
    selected_clip_ids_.clear();
    if (first_inserted.has_value()) {
      selected_clip_ids_.insert(*first_inserted);
    }
    active_clip_id_ = first_inserted;
    selected_marker_id_.reset();
    selected_gap_key_.clear();
    refreshViews();
  }
}

void EditorController::splitSelectedClip() {
  const edit::Sequence* sequence = currentSequence();
  const auto selected = selectedClipIds();
  if (sequence == nullptr || selected.empty()) {
    window_.showTransientMessage(tr("Select a clip before splitting"));
    return;
  }
  std::unordered_set<edit::EntityId> consumed;
  std::vector<edit::EditCommand> commands;
  for (const auto& selected_id : selected) {
    if (consumed.contains(selected_id)) {
      continue;
    }
    const auto participants = expandLinkedSelection(*sequence, {selected_id});
    consumed.insert(participants.begin(), participants.end());
    edit::SplitClipCommand split{.sequence_id = sequence->id,
                                 .clip_id = selected_id,
                                 .split_time = playheadTime(),
                                 .right_clip_id = edit::EntityId::generate(),
                                 .include_linked = participants.size() > 1,
                                 .linked_right_clip_ids = {}};
    for (const auto& participant : participants) {
      if (participant != selected_id) {
        split.linked_right_clip_ids.push_back(
            {.clip_id = participant, .right_clip_id = edit::EntityId::generate()});
      }
    }
    commands.push_back({.operation = std::move(split), .coalescing_key = {}});
  }
  (void)applyBatch(std::move(commands),
                   tr("Could not split the selected clips and their linked media"));
}

void EditorController::deleteSelectedClip(const bool ripple) {
  const edit::Sequence* sequence = currentSequence();
  const auto selected = selectedClipIds();
  if (sequence == nullptr || selected.empty()) {
    return;
  }
  std::vector<edit::EditCommand> commands;
  std::unordered_set<edit::EntityId> consumed;
  for (const auto& clipId : selected) {
    if (consumed.contains(clipId)) {
      continue;
    }
    const auto participants = expandLinkedSelection(*sequence, {clipId});
    consumed.insert(participants.begin(), participants.end());
    commands.push_back(
        {.operation = edit::RemoveClipCommand{.sequence_id = sequence->id,
                                              .clip_id = clipId,
                                              .ripple = ripple,
                                              .include_linked = participants.size() > 1},
         .coalescing_key = {}});
  }
  if (applyBatch(std::move(commands), ripple ? tr("Could not ripple-delete the selection")
                                             : tr("Could not delete the selection"))) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
  }
}

void EditorController::commitTimelineEdit(const QString& clipId, const int destinationTrackIndex,
                                          const qint64 startDelta, const qint64 durationDelta,
                                          const int editMode, const int editIntent) {
  commitTimelineBatchEdit({clipId}, destinationTrackIndex, startDelta, durationDelta, editMode,
                          editIntent);
}

void EditorController::commitTimelineBatchEdit(const QStringList& clipIds,
                                               const int destinationTrackIndex,
                                               const qint64 startDelta, const qint64 durationDelta,
                                               const int editMode, const int editIntent) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || destinationTrackIndex < 0 ||
      destinationTrackIndex >= static_cast<int>(sequence->tracks.size())) {
    window_.showTransientMessage(tr("The timeline edit target is no longer available"));
    refreshTimelineView();
    return;
  }
  if (!clipIds.isEmpty()) {
    const QString prior_active = active_clip_id_.has_value()
                                     ? QString::fromStdString(active_clip_id_->toString())
                                     : QString{};
    const QString active =
        !prior_active.isEmpty() && clipIds.contains(prior_active) ? prior_active : clipIds.front();
    setClipSelection(clipIds, active);
  }
  const auto selection = selectedClipIds();
  if (selection.empty() || !active_clip_id_.has_value()) {
    window_.showTransientMessage(tr("Select one or more clips before editing"));
    refreshTimelineView();
    return;
  }

  try {
    const auto mode = static_cast<desktop_ui::TimelineWidget::EditMode>(editMode);
    const auto intent = static_cast<desktop_ui::TimelineWidget::EditIntent>(editIntent);
    const edit::InsertMode insert_mode =
        intent == desktop_ui::TimelineWidget::EditIntent::Ripple ? edit::InsertMode::Ripple
        : intent == desktop_ui::TimelineWidget::EditIntent::Overwrite
            ? edit::InsertMode::Overwrite
            : edit::InsertMode::RejectOverlap;
    const edit::Time start_delta = timelineTime(startDelta);
    const edit::Time duration_delta = timelineTime(durationDelta);
    const auto trackForClip = [sequence](const edit::EntityId& id) -> const edit::Track* {
      for (const auto& track : sequence->tracks) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [&id](const auto& clip) { return clip.id == id; })) {
          return &track;
        }
      }
      return nullptr;
    };
    const auto sourceRangeForTimelineRange = [](const edit::Clip& clip,
                                                const edit::TimeRange& timelineRange) {
      const edit::Time head_delta = timelineRange.start - clip.timeline_range.start;
      const edit::Time tail_delta = timelineRange.end() - clip.timeline_range.end();
      const auto source_delta = [&clip](const edit::Time delta) {
        return delta
            .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                    edit::RoundingMode::NearestTiesEven)
            .rescaledTo(clip.source_range.duration.timescale(),
                        edit::RoundingMode::NearestTiesEven);
      };
      edit::Time source_start = clip.source_range.start;
      edit::Time source_end = clip.source_range.end();
      if (!clip.reversed) {
        source_start = source_start + source_delta(head_delta);
        source_end = source_end + source_delta(tail_delta);
      } else {
        source_start = source_start - source_delta(tail_delta);
        source_end = source_end - source_delta(head_delta);
      }
      return edit::TimeRange(source_start, source_end - source_start);
    };

    std::vector<edit::EditCommand> commands;
    std::unordered_set<edit::EntityId> consumed;
    for (const auto& id : selection) {
      if (consumed.contains(id)) {
        continue;
      }
      const edit::Clip* clip = edit::findClip(*sequence, id);
      const edit::Track* source_track = trackForClip(id);
      if (clip == nullptr || source_track == nullptr) {
        window_.showTransientMessage(tr("A selected clip no longer exists"));
        refreshTimelineView();
        return;
      }
      const auto linked = expandLinkedSelection(*sequence, {id});
      consumed.insert(linked.begin(), linked.end());
      const bool include_linked = linked.size() > 1;
      if (mode == desktop_ui::TimelineWidget::EditMode::Move) {
        const edit::Track& destination =
            id == *active_clip_id_
                ? sequence->tracks[static_cast<std::size_t>(destinationTrackIndex)]
                : *source_track;
        commands.push_back(
            {.operation =
                 edit::MoveClipCommand{.sequence_id = sequence->id,
                                       .clip_id = clip->id,
                                       .destination_track_id = destination.id,
                                       .new_start = clip->timeline_range.start + start_delta,
                                       .mode = insert_mode,
                                       .include_linked = include_linked},
             .coalescing_key = {}});
      } else if (mode == desktop_ui::TimelineWidget::EditMode::TrimIn ||
                 mode == desktop_ui::TimelineWidget::EditMode::TrimOut) {
        const edit::TimeRange timeline_range(clip->timeline_range.start + start_delta,
                                             clip->timeline_range.duration + duration_delta);
        commands.push_back(
            {.operation = edit::TrimClipCommand{.sequence_id = sequence->id,
                                                .clip_id = clip->id,
                                                .timeline_range = timeline_range,
                                                .source_range = sourceRangeForTimelineRange(
                                                    *clip, timeline_range),
                                                .include_linked = include_linked,
                                                .mode = insert_mode},
             .coalescing_key = {}});
      }
    }

    const edit::Clip* active = edit::findClip(*sequence, *active_clip_id_);
    const edit::Track* active_track = trackForClip(*active_clip_id_);
    if (active == nullptr || active_track == nullptr) {
      throw std::invalid_argument("the active clip is no longer available");
    }
    if (mode == desktop_ui::TimelineWidget::EditMode::Roll) {
      const auto active_index = static_cast<std::size_t>(std::distance(
          active_track->clips.begin(),
          std::find_if(active_track->clips.begin(), active_track->clips.end(),
                       [active](const auto& clip) { return clip.id == active->id; })));
      const edit::Clip* left = nullptr;
      const edit::Clip* right = nullptr;
      edit::Time cut{};
      const bool incoming_edge = start_delta != edit::Time{} && duration_delta == -start_delta;
      const bool has_preceding =
          active_index > 0 && active_track->clips[active_index - 1].timeline_range.end() ==
                                  active->timeline_range.start;
      const bool has_following = active_index + 1 < active_track->clips.size() &&
                                 active->timeline_range.end() ==
                                     active_track->clips[active_index + 1].timeline_range.start;
      // The widget represents an incoming roll as an inverse start/duration
      // pair. Outgoing rolls use the shared-cut delta directly. Prefer the
      // matching edge, then retain the only available adjacent cut.
      if (incoming_edge && has_preceding) {
        left = &active_track->clips[active_index - 1];
        right = active;
        cut = active->timeline_range.start;
      } else if (!incoming_edge && has_following) {
        left = active;
        right = &active_track->clips[active_index + 1];
        cut = active->timeline_range.end();
      } else if (has_preceding) {
        left = &active_track->clips[active_index - 1];
        right = active;
        cut = active->timeline_range.start;
      } else if (has_following) {
        left = active;
        right = &active_track->clips[active_index + 1];
        cut = active->timeline_range.end();
      } else {
        throw std::invalid_argument("roll edit requires adjacent clips");
      }
      commands.clear();
      commands.push_back({.operation = edit::RollEditCommand{.sequence_id = sequence->id,
                                                             .left_clip_id = left->id,
                                                             .right_clip_id = right->id,
                                                             .new_cut_time = cut + start_delta},
                          .coalescing_key = {}});
    } else if (mode == desktop_ui::TimelineWidget::EditMode::Slip) {
      commands.clear();
      const edit::Time source_delta =
          start_delta
              .scaled(active->playback_rate.numerator(), active->playback_rate.denominator(),
                      edit::RoundingMode::NearestTiesEven)
              .rescaledTo(active->source_range.start.timescale(),
                          edit::RoundingMode::NearestTiesEven);
      commands.push_back(
          {.operation =
               edit::SlipClipCommand{.sequence_id = sequence->id,
                                     .clip_id = active->id,
                                     .new_source_start = active->source_range.start + source_delta,
                                     .include_linked =
                                         expandLinkedSelection(*sequence, {active->id}).size() > 1},
           .coalescing_key = {}});
    } else if (mode == desktop_ui::TimelineWidget::EditMode::Slide) {
      commands.clear();
      commands.push_back(
          {.operation =
               edit::SlideClipCommand{.sequence_id = sequence->id,
                                      .clip_id = active->id,
                                      .new_start = active->timeline_range.start + start_delta},
           .coalescing_key = {}});
    }
    (void)applyBatch(std::move(commands), tr("Could not apply the timeline edit"));
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
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    selected_marker_id_.reset();
    selected_gap_key_.clear();
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
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    selected_marker_id_.reset();
    selected_gap_key_.clear();
    refreshViews();
  } catch (const std::exception& exception) {
    showError(tr("Could not persist redo"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::seek(const qint64 position) {
  playhead_ = toUiTime(timelineTime(std::max<qint64>(position, 0)));
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
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
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
      window_.timeline()->setPlayhead(timelineValue(playheadTime()));
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
        window_.timeline()->setPlayhead(timelineValue(playheadTime()));
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
          window_.timeline()->setPlayhead(timelineValue(playheadTime()));
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
      // Poll the playback meter and push per-channel peak levels to the
      // mixer. The meter is callback-safe and resets on read.
      if (audio_playback_ != nullptr) {
        const audio::PlaybackMeter::Reading meter = audio_playback_->read_meter();
        QVector<float> peak_dbfs;
        peak_dbfs.reserve(2);
        for (std::size_t c = 0; c < 2U; ++c) {
          const float linear = std::max(c == 0 ? meter.peak[c] : meter.peak[c], 1.0e-12F);
          peak_dbfs.push_back(20.0F * std::log10(linear));
        }
        // Push to the first audio strip (master bus meter for now).
        window_.audioMixer()->setMeterLevels(0, peak_dbfs);
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
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
  requestPreview(PreviewRequestPolicy::Coalesce);
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
    seek(timelineValue(sequence->captions[caption_index].range.start));
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
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    window_.showTransientMessage(tr("Select a clip before changing its properties"));
    return;
  }
  const edit::Clip* selected = edit::findClip(*sequence, *active_clip_id_);
  if (selected == nullptr) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
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
    return;
  }

  if (parameterId == QStringLiteral("speed") || parameterId == QStringLiteral("reverse")) {
    // Speed is expressed as a percentage of normal in the UI (100 = 1x).
    const double speed_percent = (parameterId == QStringLiteral("speed"))
                                     ? value.toDouble()
                                     : selected_speed_percent_;
    const bool reversed = (parameterId == QStringLiteral("reverse"))
                               ? value.toBool()
                               : selected->reversed;
    selected_speed_percent_ = speed_percent;
    // Convert percentage to an exact rational rate. 100% = 1/1, 200% = 2/1,
    // 50% = 1/2. Use a reduced fraction of (percent, 100).
    const auto percent = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        std::llround(speed_percent), 1, 100'000));
    std::uint32_t num = percent;
    std::uint32_t den = 100U;
    const auto g = static_cast<std::uint32_t>(std::gcd(num, den));
    if (g > 1U) {
      num /= g;
      den /= g;
    }
    const edit::Rate rate{num, den};
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipSpeedCommand{.sequence_id = sequence->id,
                                                                   .clip_id = selected->id,
                                                                   .playback_rate = rate,
                                                                   .reversed = reversed},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip speed"));
    return;
  }

  if (parameterId == QStringLiteral("titleText") || parameterId == QStringLiteral("titleFont") ||
      parameterId == QStringLiteral("titleSize") || parameterId == QStringLiteral("titleAlign") ||
      parameterId == QStringLiteral("titleBold") ||
      parameterId == QStringLiteral("titleItalic")) {
    if (selected->kind != edit::ClipKind::Title || !selected->title.has_value()) {
      return;
    }
    edit::Title title = *selected->title;
    if (parameterId == QStringLiteral("titleText")) {
      title.text = value.toString().toStdString();
    } else if (parameterId == QStringLiteral("titleFont")) {
      title.font_family = value.toString().toStdString();
    } else if (parameterId == QStringLiteral("titleSize")) {
      title.font_size = value.toDouble();
    } else if (parameterId == QStringLiteral("titleAlign")) {
      const QString align = value.toString();
      title.horizontal_alignment =
          align == QStringLiteral("left")    ? edit::TitleHorizontalAlignment::Left
          : align == QStringLiteral("right") ? edit::TitleHorizontalAlignment::Right
                                              : edit::TitleHorizontalAlignment::Center;
    } else if (parameterId == QStringLiteral("titleBold")) {
      title.bold = value.toBool();
    } else if (parameterId == QStringLiteral("titleItalic")) {
      title.italic = value.toBool();
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipTitleCommand{.sequence_id = sequence->id,
                                                                    .clip_id = selected->id,
                                                                    .title = title},
                          .coalescing_key = coalescing_key},
        tr("Could not update the title"));
  }
}

void EditorController::toggleSelectedClipKeyframe(const QString& parameterId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || active_clip_id_.has_value() == false) {
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    return;
  }
  // Find the effect parameter matching this Inspector field. The Inspector
  // parameter IDs (positionX, scale, etc.) map to transform/effect fields;
  // for now keyframing is wired only for effect parameters that carry the
  // same id. A full keyframe-curve editor is a follow-up; this toggles a
  // keyframe at the current playhead for the named parameter.
  const edit::Time key_time = playheadTime();
  for (const auto& effect : clip->effects) {
    auto it = effect.parameters.find(parameterId.toStdString());
    if (it == effect.parameters.end()) {
      continue;
    }
    edit::EffectParameter parameter = it->second;
    const auto existing =
        std::find_if(parameter.keyframes.begin(), parameter.keyframes.end(),
                      [&](const edit::Keyframe& key) { return key.time == key_time; });
    if (existing != parameter.keyframes.end()) {
      parameter.keyframes.erase(existing);
    } else {
      edit::Keyframe key;
      key.time = key_time;
      key.value = parameter.value;
      key.interpolation = edit::KeyframeInterpolation::Linear;
      parameter.keyframes.push_back(key);
      std::sort(parameter.keyframes.begin(), parameter.keyframes.end(),
                [](const edit::Keyframe& a, const edit::Keyframe& b) {
                  return a.time < b.time;
                });
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipEffectParameterCommand{
                              .sequence_id = sequence->id, .clip_id = clip->id,
                              .effect_id = effect.id, .parameter = parameter}},
        tr("Could not toggle the keyframe"));
    return;
  }
}

void EditorController::addTitleClip() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  // Insert a 5-second title clip on the first video track at the playhead.
  const edit::Time start = playheadTime();
  const edit::Time duration(5, 1);
  edit::Clip clip;
  clip.kind = edit::ClipKind::Title;
  clip.name = "Title";
  clip.timeline_range = edit::TimeRange(start, duration);
  clip.source_range = edit::TimeRange(edit::Time{}, duration);
  clip.playback_rate = edit::Rate{1, 1};
  clip.title = edit::Title{};
  // Place on the first video track that is not locked.
  edit::EntityId track_id;
  for (const auto& track : sequence->tracks) {
    if (track.kind == edit::TrackKind::Video && !track.locked) {
      track_id = track.id;
      break;
    }
  }
  if (track_id.isNil()) {
    window_.showTransientMessage(tr("No unlocked video track for a title"));
    return;
  }
  edit::InsertClipCommand insert{.sequence_id = sequence->id,
                                  .track_id = track_id,
                                  .clip = std::move(clip)};
  (void)apply(edit::EditCommand{.operation = std::move(insert)},
              tr("Could not add a title"));
}

void EditorController::setTransitionSelection(const QString& transitionId) {
  selected_transition_id_ = parseId(transitionId);
}

void EditorController::updateTransitionDuration(const QString& transitionId,
                                                const qint64 duration) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  const edit::Transition* transition = edit::findTransition(*sequence, *parsed_id);
  if (transition == nullptr) {
    return;
  }
  edit::Transition updated = *transition;
  const edit::Time new_duration = timelineTime(std::max<qint64>(1, duration));
  // Preserve the end of the transition range; adjust the start.
  const edit::Time end = updated.range.start + updated.range.duration;
  updated.range = edit::TimeRange(end - new_duration, new_duration);
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateTransitionCommand{.sequence_id = sequence->id,
                                                                     .transition = updated}},
      tr("Could not update the transition duration"));
}

void EditorController::removeTransition(const QString& transitionId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  (void)apply(
      edit::EditCommand{.operation = edit::RemoveTransitionCommand{
          .sequence_id = sequence->id, .transition_id = *parsed_id}},
      tr("Could not remove the transition"));
  selected_transition_id_.reset();
}

void EditorController::changeTransitionPreset(const QString& transitionId, const QString& kind) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  const edit::Transition* transition = edit::findTransition(*sequence, *parsed_id);
  if (transition == nullptr) {
    return;
  }
  edit::Transition updated = *transition;
  updated.kind = (kind == QStringLiteral("dip_to_black")) ? edit::TransitionKind::DipToBlack
                                                          : edit::TransitionKind::CrossDissolve;
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateTransitionCommand{.sequence_id = sequence->id,
                                                                     .transition = updated}},
      tr("Could not change the transition preset"));
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

void EditorController::setAudioTrackGain(const int trackIndex, const double gainDb) {
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
        edit::EditCommand{.operation = edit::SetTrackAudioMixCommand{
                              .sequence_id = sequence->id,
                              .track_id = track.id,
                              .gain_db = gainDb,
                              .pan = track.audio_pan},
                          .coalescing_key = "mixer:" + track.id.toString() + ":gain"},
        tr("Could not update track gain"));
    return;
  }
  refreshMixerView();
}

void EditorController::setAudioTrackPan(const int trackIndex, const double pan) {
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
        edit::EditCommand{.operation = edit::SetTrackAudioMixCommand{
                              .sequence_id = sequence->id,
                              .track_id = track.id,
                              .gain_db = track.audio_gain_db,
                              .pan = pan},
                          .coalescing_key = "mixer:" + track.id.toString() + ":pan"},
        tr("Could not update track pan"));
    return;
  }
  refreshMixerView();
}

void EditorController::persistSnapshot(const std::string_view reason) {
  const auto project = editor_->projectAt(editor_->revision());
  const project_codec::ProjectBytes bytes = project_codec::serialize_project(*project);
  const auto metadata = store_->metadata();
  store_->append_command("project.snapshot.v2", std::span<const std::byte>(bytes),
                         metadata.head_revision, project_codec::kCurrentSchemaVersion);
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
  const auto result =
      editor_->applyBatch(std::move(commands), editor_->revision(), "Timeline batch", batch_key);
  if (!result) {
    window_.showTransientMessage(QStringLiteral("%1: %2").arg(
        failureContext, QString::fromStdString(result.error().message)));
    refreshViews();
    return false;
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

std::uint32_t EditorController::timelineTimeScale(const edit::Sequence& sequence) const {
  const std::uint64_t base = static_cast<std::uint64_t>(kUiTimescale);
  const std::uint64_t numerator = std::max<std::uint32_t>(1U, sequence.frame_rate.numerator());
  const std::uint64_t divisor = std::gcd(base, numerator);
  const std::uint64_t scale = (base / divisor) * numerator;
  return scale > std::numeric_limits<std::uint32_t>::max()
             ? static_cast<std::uint32_t>(kUiTimescale)
             : static_cast<std::uint32_t>(scale);
}

edit::Time EditorController::timelineTime(const qint64 value) const {
  return edit::Time(value, timeline_time_scale_);
}

qint64 EditorController::timelineValue(const edit::Time time) const {
  return static_cast<qint64>(
      time.rescaledTo(timeline_time_scale_, edit::RoundingMode::NearestTiesEven).value());
}

std::vector<edit::EntityId> EditorController::selectedClipIds() const {
  std::vector<edit::EntityId> result;
  result.reserve(selected_clip_ids_.size());
  for (const auto& id : selected_clip_ids_) {
    result.push_back(id);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<edit::EntityId>
EditorController::expandLinkedSelection(const edit::Sequence& sequence,
                                        const std::vector<edit::EntityId>& clipIds) const {
  std::unordered_set<edit::EntityId> result(clipIds.begin(), clipIds.end());
  for (const auto& id : clipIds) {
    const edit::Clip* clip = edit::findClip(sequence, id);
    if (clip == nullptr || !clip->linked_group.has_value()) {
      continue;
    }
    for (const auto& track : sequence.tracks) {
      for (const auto& candidate : track.clips) {
        if (candidate.linked_group == clip->linked_group) {
          result.insert(candidate.id);
        }
      }
    }
  }
  std::vector<edit::EntityId> ordered(result.begin(), result.end());
  std::sort(ordered.begin(), ordered.end());
  return ordered;
}

void EditorController::pruneTimelineSelection(const edit::Sequence& sequence) {
  for (auto it = selected_clip_ids_.begin(); it != selected_clip_ids_.end();) {
    if (edit::findClip(sequence, *it) == nullptr) {
      it = selected_clip_ids_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_clip_id_.has_value() && !selected_clip_ids_.contains(*active_clip_id_)) {
    active_clip_id_.reset();
  }
  if (!active_clip_id_.has_value() && !selected_clip_ids_.empty()) {
    active_clip_id_ = *std::min_element(selected_clip_ids_.begin(), selected_clip_ids_.end());
  }
  if (selected_marker_id_.has_value() &&
      std::none_of(
          sequence.markers.begin(), sequence.markers.end(),
          [this](const edit::Marker& marker) { return marker.id == *selected_marker_id_; })) {
    selected_marker_id_.reset();
  }
  if (!selected_gap_key_.isEmpty()) {
    bool still_current = false;
    const auto snapshot = editor_->snapshot(sequence.id, editor_->revision());
    if (snapshot) {
      const edit::Time limit = edit::sequenceDuration(sequence);
      for (const auto& track : sequence.tracks) {
        for (const auto& gap : snapshot.value().gaps(track.id, limit)) {
          if (gapKey(track.id, gap.timeline_range) == selected_gap_key_) {
            still_current = true;
            break;
          }
        }
        if (still_current) {
          break;
        }
      }
    }
    if (!still_current) {
      selected_gap_key_.clear();
    }
  }
}

void EditorController::setClipSelection(const QStringList& clipIds, const QString& activeClipId) {
  selected_clip_ids_.clear();
  for (const auto& text : clipIds) {
    if (const auto id = parseId(text); id.has_value()) {
      selected_clip_ids_.insert(*id);
    }
  }
  active_clip_id_ = parseId(activeClipId);
  if (!active_clip_id_.has_value() || !selected_clip_ids_.contains(*active_clip_id_)) {
    active_clip_id_ = selected_clip_ids_.empty()
                          ? std::nullopt
                          : std::optional<edit::EntityId>(*std::min_element(
                                selected_clip_ids_.begin(), selected_clip_ids_.end()));
  }
  selected_marker_id_.reset();
  selected_gap_key_.clear();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::selectMarker(const QString& markerId) {
  selected_marker_id_ = parseId(markerId);
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_gap_key_.clear();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::selectGap(const QString& gapKey) {
  selected_gap_key_ = gapKey;
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_marker_id_.reset();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::nudgeTimelineSelection(const QStringList& clipIds, const int frameCount,
                                              const int editIntent) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || frameCount == 0) {
    return;
  }
  const QString prior_active =
      active_clip_id_.has_value() ? QString::fromStdString(active_clip_id_->toString()) : QString{};
  const QString active = !prior_active.isEmpty() && clipIds.contains(prior_active)
                             ? prior_active
                             : (clipIds.isEmpty() ? QString{} : clipIds.front());
  setClipSelection(clipIds, active);
  const auto selection = selectedClipIds();
  if (selection.empty()) {
    return;
  }
  const auto intent = static_cast<desktop_ui::TimelineWidget::EditIntent>(editIntent);
  const edit::InsertMode mode = intent == desktop_ui::TimelineWidget::EditIntent::Ripple
                                    ? edit::InsertMode::Ripple
                                : intent == desktop_ui::TimelineWidget::EditIntent::Overwrite
                                    ? edit::InsertMode::Overwrite
                                    : edit::InsertMode::RejectOverlap;
  std::unordered_set<edit::EntityId> consumed;
  std::vector<edit::EditCommand> commands;
  for (const auto& id : selection) {
    if (consumed.contains(id)) {
      continue;
    }
    const edit::Clip* clip = edit::findClip(*sequence, id);
    if (clip == nullptr) {
      continue;
    }
    const auto linked = expandLinkedSelection(*sequence, {id});
    consumed.insert(linked.begin(), linked.end());
    const auto track =
        std::find_if(sequence->tracks.begin(), sequence->tracks.end(), [&id](const auto& item) {
          return std::any_of(item.clips.begin(), item.clips.end(),
                             [&id](const auto& candidate) { return candidate.id == id; });
        });
    if (track == sequence->tracks.end()) {
      continue;
    }
    // Nudge is a translation, not a re-quantization. In particular, a clip
    // deliberately placed between frames must retain that offset and every
    // linked group receives the same rational frame delta.
    const edit::Time target_start =
        clip->timeline_range.start + sequence->frame_rate.frameTime(frameCount);
    if (target_start.isNegative()) {
      window_.showTransientMessage(tr("Cannot nudge the selected clips before the timeline start"));
      return;
    }
    commands.push_back({.operation = edit::MoveClipCommand{.sequence_id = sequence->id,
                                                           .clip_id = id,
                                                           .destination_track_id = track->id,
                                                           .new_start = target_start,
                                                           .mode = mode,
                                                           .include_linked = linked.size() > 1},
                        .coalescing_key = {}});
  }
  (void)applyBatch(std::move(commands), tr("Could not nudge the timeline selection"));
}

bool EditorController::applyTrackCommand(edit::EditCommand command, const QString& failureContext) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return false;
  }
  return apply(std::move(command), failureContext);
}

void EditorController::addTrack(const int trackKind) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto kind = static_cast<desktop_ui::TrackKind>(trackKind);
  edit::Track track;
  track.kind = kind == desktop_ui::TrackKind::Audio     ? edit::TrackKind::Audio
               : kind == desktop_ui::TrackKind::Caption ? edit::TrackKind::Caption
                                                        : edit::TrackKind::Video;
  const char* prefix = track.kind == edit::TrackKind::Audio     ? "Audio"
                       : track.kind == edit::TrackKind::Caption ? "Captions"
                                                                : "Video";
  const auto ordinal =
      1 + std::count_if(sequence->tracks.begin(), sequence->tracks.end(),
                        [&track](const auto& item) { return item.kind == track.kind; });
  track.name = std::string(prefix) + " " + std::to_string(ordinal);
  (void)applyTrackCommand({.operation = edit::AddTrackCommand{.sequence_id = sequence->id,
                                                              .track = std::move(track),
                                                              .index = std::nullopt},
                           .coalescing_key = {}},
                          tr("Could not add a track"));
}

void EditorController::renameTrack(const QString& trackId, const QString& name) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value()) {
    return;
  }
  (void)applyTrackCommand(
      {.operation = edit::RenameTrackCommand{.sequence_id = sequence->id,
                                             .track_id = *id,
                                             .name = name.trimmed().toStdString()},
       .coalescing_key = {}},
      tr("Could not rename the track"));
}

void EditorController::reorderTrack(const QString& trackId, const int destinationIndex) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value() || destinationIndex < 0) {
    return;
  }
  (void)applyTrackCommand(
      {.operation = edit::ReorderTrackCommand{.sequence_id = sequence->id,
                                              .track_id = *id,
                                              .index = static_cast<std::size_t>(destinationIndex)},
       .coalescing_key = {}},
      tr("Could not reorder the track"));
}

void EditorController::setTrackLocked(const QString& trackId, const bool locked) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackLockedCommand{.sequence_id = sequence->id,
                                                                    .track_id = *id,
                                                                    .locked = locked},
                           .coalescing_key = {}},
                          tr("Could not change the track lock"));
}

void EditorController::setTrackVisible(const QString& trackId, const bool visible) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackVisibilityCommand{.sequence_id = sequence->id,
                                                                        .track_id = *id,
                                                                        .visible = visible},
                           .coalescing_key = {}},
                          tr("Could not change track visibility"));
}

void EditorController::setTrackTargeted(const QString& trackId, const bool targeted) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackTargetedCommand{.sequence_id = sequence->id,
                                                                      .track_id = *id,
                                                                      .targeted = targeted},
                           .coalescing_key = {}},
                          tr("Could not change track targeting"));
}

void EditorController::removeTrack(const QString& trackId) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand(
      {.operation = edit::RemoveTrackCommand{.sequence_id = sequence->id, .track_id = *id},
       .coalescing_key = {}},
      tr("Could not remove the track"));
}

void EditorController::addMarker(const qint64 start) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr)
    return;
  edit::Marker marker;
  marker.range = edit::TimeRange(timelineTime(std::max<qint64>(0, start)), edit::Time{});
  marker.label = tr("Marker").toStdString();
  if (apply({.operation = edit::AddMarkerCommand{.sequence_id = sequence->id, .marker = marker},
             .coalescing_key = {}},
            tr("Could not add a marker"))) {
    selected_marker_id_ = marker.id;
    selected_clip_ids_.clear();
    active_clip_id_.reset();
  }
}

void EditorController::moveMarker(const QString& markerId, const qint64 start) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  const auto found = std::find_if(sequence->markers.begin(), sequence->markers.end(),
                                  [&id](const auto& marker) { return marker.id == *id; });
  if (found == sequence->markers.end()) {
    window_.showTransientMessage(tr("The marker no longer exists"));
    refreshTimelineView();
    return;
  }
  auto marker = *found;
  marker.range.start = timelineTime(std::max<qint64>(0, start));
  (void)apply({.operation = edit::UpdateMarkerCommand{.sequence_id = sequence->id,
                                                      .marker = std::move(marker)},
               .coalescing_key = {}},
              tr("Could not move the marker"));
}

void EditorController::renameMarker(const QString& markerId, const QString& name) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  const auto found = std::find_if(sequence->markers.begin(), sequence->markers.end(),
                                  [&id](const auto& marker) { return marker.id == *id; });
  if (found == sequence->markers.end())
    return;
  auto marker = *found;
  marker.label = name.trimmed().toStdString();
  (void)apply({.operation = edit::UpdateMarkerCommand{.sequence_id = sequence->id,
                                                      .marker = std::move(marker)},
               .coalescing_key = {}},
              tr("Could not rename the marker"));
}

void EditorController::removeMarker(const QString& markerId) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  if (apply({.operation = edit::RemoveMarkerCommand{.sequence_id = sequence->id, .marker_id = *id},
             .coalescing_key = {}},
            tr("Could not remove the marker"))) {
    selected_marker_id_.reset();
  }
}

void EditorController::closeGap(const QString& gapKeyText) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || gapKeyText.isEmpty()) {
    return;
  }
  const auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    window_.showTransientMessage(tr("Could not resolve the current timeline gaps"));
    refreshTimelineView();
    return;
  }
  const edit::TimelineSnapshot snapshot = snapshot_result.value();
  const edit::Time limit = edit::sequenceDuration(*sequence);
  for (const auto& track : sequence->tracks) {
    for (const auto& gap : snapshot.gaps(track.id, limit)) {
      if (gapKey(track.id, gap.timeline_range) != gapKeyText) {
        continue;
      }
      if (apply({.operation = edit::CloseGapCommand{.sequence_id = sequence->id,
                                                    .track_id = track.id,
                                                    .gap = gap.timeline_range},
                 .coalescing_key = {}},
                tr("Could not close the gap"))) {
        selected_gap_key_.clear();
      }
      return;
    }
  }
  window_.showTransientMessage(tr("That gap changed before it could be closed"));
  selected_gap_key_.clear();
  refreshTimelineView();
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
  timeline_time_scale_ = timelineTimeScale(*sequence);
  pruneTimelineSelection(*sequence);
  QVector<TimelineTrackView> tracks;
  QVector<TimelineClipView> clips;
  QVector<desktop_ui::TimelineMarkerView> markers;
  QVector<desktop_ui::TimelineGapView> gaps;
  tracks.reserve(static_cast<qsizetype>(sequence->tracks.size()));
  markers.reserve(static_cast<qsizetype>(sequence->markers.size()));
  std::size_t track_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    tracks.push_back({
        .id = QString::fromStdString(track.id.toString()),
        .displayName = QString::fromStdString(track.name),
        .kind = uiTrackKind(track.kind),
        .muted = track.muted,
        .soloed = track.solo,
        .locked = track.locked,
        .visible = track.visible,
        .targeted = track.targeted,
    });
    for (const edit::Clip& clip : track.clips) {
      const auto proxy = std::find_if(
          imported_assets_.begin(), imported_assets_.end(),
          [&clip](const assets::AssetRecord& item) { return item.id == clip.asset_id.toString(); });
      clips.push_back({
          .id = QString::fromStdString(clip.id.toString()),
          .displayName = QString::fromStdString(clip.name),
          .trackIndex = static_cast<int>(track_index),
          .start = timelineValue(clip.timeline_range.start),
          .duration = std::max<qint64>(1, timelineValue(clip.timeline_range.duration)),
          .color = colorForTrack(track.kind, track_index),
          .selected = selected_clip_ids_.contains(clip.id),
          .offline = edit::findAsset(*project, clip.asset_id) == nullptr,
          .proxy =
              proxy != imported_assets_.end() && proxy->proxy.has_value() && proxy->proxy->complete,
      });
    }
    ++track_index;
  }
  for (const auto& marker : sequence->markers) {
    markers.push_back(
        {.id = QString::fromStdString(marker.id.toString()),
         .displayName = QString::fromStdString(marker.label),
         .start = timelineValue(marker.range.start),
         .duration = timelineValue(marker.range.duration),
         .color = QColor::fromRgbF(
             static_cast<float>(marker.color.red), static_cast<float>(marker.color.green),
             static_cast<float>(marker.color.blue), static_cast<float>(marker.color.alpha)),
         .selected = selected_marker_id_.has_value() && marker.id == *selected_marker_id_});
  }
  const auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (snapshot_result) {
    const edit::Time limit = edit::sequenceDuration(*sequence);
    const auto snapshot = snapshot_result.value();
    for (std::size_t index = 0; index < sequence->tracks.size(); ++index) {
      const auto& track = sequence->tracks[index];
      for (const auto& gap : snapshot.gaps(track.id, limit)) {
        const QString key = gapKey(track.id, gap.timeline_range);
        gaps.push_back({.key = key,
                        .trackId = QString::fromStdString(track.id.toString()),
                        .trackIndex = static_cast<int>(index),
                        .start = timelineValue(gap.timeline_range.start),
                        .duration = timelineValue(gap.timeline_range.duration),
                        .selected = key == selected_gap_key_});
      }
    }
  }
  const qint64 duration = std::max<qint64>(timelineValue(edit::sequenceDuration(*sequence)),
                                           static_cast<qint64>(timeline_time_scale_) * 10);
  window_.setTimelineView(duration, timeline_time_scale_, std::move(tracks), std::move(clips),
                          std::move(markers), std::move(gaps));
  window_.timeline()->setSnapResolver([this](const desktop_ui::TimelineSnapRequest& request) {
    const edit::Sequence* current = currentSequence();
    if (current == nullptr) {
      return desktop_ui::TimelineSnapResult{
          .time = request.proposedTime, .kind = desktop_ui::TimelineSnapKind::None, .label = {}};
    }
    edit::SnapRequest snap_request;
    snap_request.proposed_time = timelineTime(request.proposedTime);
    const double pixels_per_second = std::max(1.0, window_.timeline()->pixelsPerSecond());
    const auto threshold_units = static_cast<std::int64_t>(
        std::ceil(static_cast<double>(std::max(0, request.thresholdPixels)) / pixels_per_second *
                  static_cast<double>(timeline_time_scale_)));
    snap_request.threshold =
        edit::Time(std::max<std::int64_t>(0, threshold_units), timeline_time_scale_);
    snap_request.playhead = playheadTime();
    for (const auto& text : request.excludedClipIds) {
      if (const auto id = parseId(text); id.has_value()) {
        snap_request.excluded_clip_ids.insert(*id);
      }
    }
    if (const auto marker_id = parseId(request.excludedMarkerId); marker_id.has_value()) {
      snap_request.excluded_marker_ids.insert(*marker_id);
    }
    const auto candidate = edit::nearestSnapCandidate(*current, snap_request);
    if (!candidate.has_value()) {
      return desktop_ui::TimelineSnapResult{
          .time = request.proposedTime, .kind = desktop_ui::TimelineSnapKind::None, .label = {}};
    }
    const auto kind = [candidate] {
      switch (candidate->kind) {
      case edit::SnapTargetKind::Playhead:
        return desktop_ui::TimelineSnapKind::Playhead;
      case edit::SnapTargetKind::Marker:
        return desktop_ui::TimelineSnapKind::Marker;
      case edit::SnapTargetKind::ClipEdge:
        return desktop_ui::TimelineSnapKind::ClipEdge;
      case edit::SnapTargetKind::FrameGrid:
      default:
        return desktop_ui::TimelineSnapKind::Frame;
      }
    }();
    return desktop_ui::TimelineSnapResult{
        .time = timelineValue(candidate->time),
        .kind = kind,
        .label = QString::fromStdString(candidate->entity_id.has_value()
                                            ? candidate->entity_id->toString()
                                            : candidate->time.toString())};
  });
  window_.timeline()->setFrameRate(sequence->frame_rate.numerator(),
                                   sequence->frame_rate.denominator());
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
  window_.programViewer()->setTimecode(timecodeText(playhead_, sequence->frame_rate));
}

void EditorController::refreshInspectorView() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    window_.inspector()->clearSelection();
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    window_.inspector()->clearSelection();
    return;
  }

  window_.inspector()->setSelectionName(QString::fromStdString(clip->name));
  window_.inspector()->setClipCapabilities(clip->kind == edit::ClipKind::Video ||
                                               clip->kind == edit::ClipKind::Title,
                                           clip->kind == edit::ClipKind::Audio);
  window_.inspector()->setTitleControlsVisible(clip->kind == edit::ClipKind::Title);
  window_.inspector()->setSpeedControlsVisible(clip->kind == edit::ClipKind::Video ||
                                               clip->kind == edit::ClipKind::Audio);
  if (clip->kind == edit::ClipKind::Title && clip->title.has_value()) {
    const edit::Title& title = *clip->title;
    window_.inspector()->setParameter(QStringLiteral("titleText"),
                                      QString::fromStdString(title.text));
    window_.inspector()->setParameter(QStringLiteral("titleFont"),
                                      QString::fromStdString(title.font_family));
    window_.inspector()->setParameter(QStringLiteral("titleSize"), title.font_size);
    const QString align =
        title.horizontal_alignment == edit::TitleHorizontalAlignment::Left  ? QStringLiteral("left")
        : title.horizontal_alignment == edit::TitleHorizontalAlignment::Right ? QStringLiteral("right")
                                                                              : QStringLiteral("center");
    window_.inspector()->setParameter(QStringLiteral("titleAlign"), align);
    window_.inspector()->setParameter(QStringLiteral("titleBold"), title.bold);
    window_.inspector()->setParameter(QStringLiteral("titleItalic"), title.italic);
  }
  // Speed is shown as a percentage of normal (100 = 1x).
  const double speed_percent =
      static_cast<double>(clip->playback_rate.numerator()) * 100.0 /
      static_cast<double>(clip->playback_rate.denominator());
  selected_speed_percent_ = speed_percent;
  window_.inspector()->setParameter(QStringLiteral("speed"), speed_percent);
  window_.inspector()->setParameter(QStringLiteral("reverse"), clip->reversed);
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
                          .soloed = track.solo,
                          .gain_db = track.audio_gain_db,
                          .pan = track.audio_pan});
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

void EditorController::requestPreview(const PreviewRequestPolicy policy) {
  if (!editor_ || !renderer_ || !frame_provider_) {
    return;
  }
  ++preview_request_serial_;
  const auto invalidate_in_flight = [this] {
    ++preview_epoch_;
    renderer_->begin_epoch(preview_epoch_);
    if (gpu_timeline_renderer_ != nullptr) {
      gpu_timeline_renderer_->begin_epoch(preview_epoch_);
    }
    frame_provider_->begin_epoch(preview_epoch_);
  };
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr ||
      std::none_of(sequence->tracks.begin(), sequence->tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Video && !track.clips.empty();
      })) {
    invalidate_in_flight();
    gpu_preview_active_ = false;
    window_.programViewer()->clearFrame();
    return;
  }
  requested_preview_position_ = playhead_;
  if (preview_in_flight_) {
    if (policy == PreviewRequestPolicy::Replace) {
      invalidate_in_flight();
    }
    return;
  }
  launchPreviewRequest();
}

void EditorController::launchPreviewRequest() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr ||
      std::none_of(sequence->tracks.begin(), sequence->tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Video && !track.clips.empty();
      })) {
    window_.programViewer()->clearFrame();
    return;
  }
  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    window_.programViewer()->clearFrame();
    return;
  }
  const std::uint64_t request_serial = preview_request_serial_;
  const std::uint64_t epoch = ++preview_epoch_;
  renderer_->begin_epoch(epoch);
  if (gpu_timeline_renderer_ != nullptr) {
    gpu_timeline_renderer_->begin_epoch(epoch);
  }
  frame_provider_->begin_epoch(epoch);
  const edit::Time requested_time(requested_preview_position_,
                                  static_cast<std::uint32_t>(kUiTimescale));
  auto snapshot = std::move(snapshot_result).value();
  const auto renderer = renderer_;
  const auto gpu_renderer = gpu_fallback_latched_ ? nullptr : gpu_renderer_;
  const auto gpu_timeline_renderer = gpu_fallback_latched_ ? nullptr : gpu_timeline_renderer_;
  preview_in_flight_ = true;

  using PreviewWatcher = QFutureWatcher<PreviewOutcome>;
  auto* watcher = new PreviewWatcher(this);
  connect(watcher, &PreviewWatcher::finished, this, [this, watcher, request_serial] {
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
        ++preview_presentation_count_;
      } else if (!outcome.error.isEmpty()) {
        window_.programViewer()->clearFrame();
        window_.showTransientMessage(outcome.error);
      }
    }
    if (outcome.epoch != preview_epoch_ || request_serial != preview_request_serial_) {
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
