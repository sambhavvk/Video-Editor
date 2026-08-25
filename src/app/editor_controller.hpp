// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/asset_service.h"
#include "video_editor/audio_engine/audio_device_manager.h"
#include "video_editor/audio_render/silence_detector.h"
#include "video_editor/desktop_ui/ui_types.hpp"
#include "video_editor/edit_model/edit_model.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/render_engine/gpu_backend.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QEvent;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QVariant;

namespace video_editor::desktop_ui {
class EditorWindow;
}

namespace video_editor::media_cache {
class CacheStore;
}

namespace video_editor::store {
class ProjectStore;
}

namespace video_editor::playback {
class AssetRegistry;
class FfmpegFrameProvider;
} // namespace video_editor::playback

namespace video_editor::render {
class CpuFrame;
class CpuRenderer;
class GpuTimelineRenderer;
class RenderCache;
} // namespace video_editor::render

namespace video_editor::audio_render {
class OriginalAudioRegistry;
class TimelineAudioRenderer;
} // namespace video_editor::audio_render

namespace video_editor::audio {
class AsyncRealtimeAudioPlayback;
}

namespace video_editor::app {

class WorkerHostSession;

struct AudioDevicePollDecision final {
  bool selected_missing{false};
  bool default_missing{false};
  bool selected_recovered{false};
  bool default_recovered{false};
};

[[nodiscard]] AudioDevicePollDecision
evaluateAudioDevicePoll(std::span<const audio::AudioDeviceInfo> previous,
                        std::span<const audio::AudioDeviceInfo> current,
                        std::string_view selected_id) noexcept;

enum class AudioMixerOutputStatus : std::uint8_t {
  BackendMissing,
  SelectedUnavailable,
  Ready,
};

// System default can open even when enumeration returns no named devices.
[[nodiscard]] AudioMixerOutputStatus audioMixerOutputStatus(bool backend_available,
                                                            bool selected_lost) noexcept;

// Maps asset-relative transcription words into the selected clip's timeline range. Words are
// clipped to the source range and returned in playback order, including for reversed clips.
[[nodiscard]] std::vector<edit::CaptionWord>
mapTranscriptionWordsToTimeline(std::span<const edit::CaptionWord> source_words,
                                edit::TimeRange source_range, edit::TimeRange timeline_range,
                                edit::Rate playback_rate, bool reversed);

// Validates both a known Content-Length and the bytes already staged for the
// pinned model. A negative content length means that the server omitted it.
[[nodiscard]] bool modelDownloadSizeAllowed(std::uintmax_t bytes_received,
                                            std::int64_t content_length,
                                            std::uintmax_t expected_bytes) noexcept;

struct NormalizationCompletionGate final {
  std::uint64_t generation{0};
  bool busy{false};

  [[nodiscard]] std::uint64_t begin() noexcept {
    busy = true;
    return ++generation;
  }
  void invalidate() noexcept {
    ++generation;
    busy = false;
  }
  [[nodiscard]] bool complete(const std::uint64_t completion_generation) noexcept {
    if (completion_generation != generation) {
      busy = false;
      return false;
    }
    busy = false;
    return true;
  }
};

class EditorController final : public QObject {
  Q_OBJECT

public:
  explicit EditorController(desktop_ui::EditorWindow& window, QObject* parent = nullptr);
  ~EditorController() override;

  [[nodiscard]] const edit::TimelineEditor& editor() const noexcept {
    return *editor_;
  }
  [[nodiscard]] bool dirty() const noexcept {
    return dirty_;
  }
  [[nodiscard]] bool audioMasterActive() const noexcept {
    return audio_master_active_;
  }
  [[nodiscard]] std::int64_t audioMasterSampleCounter() const noexcept;
  [[nodiscard]] std::uint64_t audioXrunCount() const;
  [[nodiscard]] bool audioControlPending() const;
  [[nodiscard]] bool playbackRunning() const noexcept {
    return playback_rate_ != 0.0;
  }
  [[nodiscard]] std::uint64_t previewPresentationCount() const noexcept {
    return preview_presentation_count_;
  }
  [[nodiscard]] std::uint64_t playbackSeekCount() const noexcept;
  [[nodiscard]] std::uint64_t playbackDecodedFrameCount() const noexcept;
  [[nodiscard]] std::uint64_t sourcePresentationCount() const noexcept {
    return source_presentation_count_;
  }
  [[nodiscard]] bool sourceAssetLoaded() const noexcept {
    return source_asset_id_.has_value();
  }
  [[nodiscard]] bool gpuPreviewActive() const noexcept {
    return gpu_preview_active_;
  }

  void importPaths(const QStringList& paths);
  [[nodiscard]] bool offerRecoveryOnStartup();
  [[nodiscard]] bool openProjectFile(const std::filesystem::path& checkpoint);
  [[nodiscard]] bool saveProjectFile(const std::filesystem::path& destination);
  [[nodiscard]] bool importCaptionFile(const std::filesystem::path& source);
  [[nodiscard]] bool exportCaptionFile(const std::filesystem::path& destination);
  [[nodiscard]] bool startVideoExport(const std::filesystem::path& destination,
                                      const QString& presetId, bool overwriteExisting = false);

signals:
  void videoExportFinished(bool succeeded, const QString& destination, const QString& message);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
  void newProject();
  void openProject();
  void saveProject();
  void saveProjectAs();
  void chooseMedia();
  void chooseCaptionFile();
  void chooseCaptionExport();
  void insertAsset(const QString& assetId);
  void loadSourceAsset(const QString& assetId);
  void insertLoadedSource(edit::InsertMode mode);
  void markSourceIn();
  void markSourceOut();
  void seekSource(qint64 position);
  void setSourcePlaybackRate(double rate);
  void stepSourceShuttle(int direction);
  void stepSourceFrame(int direction);
  void advanceSourcePlayback();
  void splitSelectedClip();
  void deleteSelectedClip(bool ripple);
  void undo();
  void redo();
  void seek(qint64 position);
  void setPlaybackRate(double rate);
  void advancePlayback();
  void seekCaption(int visibleRow);
  void addCaptionAtPlayhead();
  void removeCaption(int visibleRow);
  void updateCaptionText(int visibleRow, const QString& text);
  void searchTranscript(const QString& query);
  void downloadTranscriptionModel(const QString& modelId);
  void startTranscription(const desktop_ui::TranscriptionOptionsView& options);
  void cancelTranscription();
  void applyCaptionReview();
  void discardCaptionReview();
  void captionStyleEdited(const QString& captionId, const desktop_ui::CaptionStyleView& style);
  void updateSelectedClipProperty(const QString& parameterId, const QVariant& value);
  void toggleSelectedClipKeyframe(const QString& parameterId);
  void addEffect(const QString& effectId);
  void updateSelectedEffectParameter(const QString& effectId, const QString& parameterId,
                                     const QVariant& value);
  void toggleSelectedEffectKeyframe(const QString& effectId, const QString& parameterId);
  void selectEffectKeyframe(const QString& effectId, const QString& parameterId, qint64 time);
  void updateSelectedEffectKeyframe(const QString& effectId, const QString& parameterId,
                                    const QString& keyframeId, qint64 time, double value);
  void updateSelectedEffectInterpolation(const QString& effectId, const QString& parameterId,
                                         const QString& keyframeId,
                                         desktop_ui::KeyframeInterpolationView interpolation);
  void removeSelectedEffectKeyframe(const QString& effectId, const QString& parameterId,
                                    const QString& keyframeId);
  void updateSelectedEffectControlPoints(const QString& effectId, const QString& parameterId,
                                         const QString& keyframeId, const QPointF& incoming,
                                         const QPointF& outgoing);
  void beginWhiteBalanceSampling();
  void applyWhiteBalanceSample(int frameX, int frameY);
  void browseEffectLut(const QString& effectId, const QString& parameterId);
  void addTitleClip();
  void setTransitionSelection(const QString& transitionId);
  void updateTransitionDuration(const QString& transitionId, qint64 duration);
  void removeTransition(const QString& transitionId);
  void changeTransitionPreset(const QString& transitionId, const QString& kind);
  void setAudioTrackMuted(int trackIndex, bool muted);
  void setAudioTrackSolo(int trackIndex, bool soloed);
  void setAudioTrackGain(int trackIndex, double gainDb);
  void setAudioTrackPan(int trackIndex, double pan);
  void addAudioTrackEffect(int trackIndex, const QString& effectType);
  void removeAudioTrackEffect(int trackIndex, const QString& effectId);
  void updateAudioTrackEffectParameter(int trackIndex, const QString& effectId,
                                       const QString& parameterId, const QVariant& value);
  void analyzeLoudnessNormalization();
  void applyLoudnessNormalization();
  void selectAudioOutputDevice(const QString& deviceId);
  void setNormalizationTarget(double targetLufs);
  void chooseVideoExport(const QString& presetId);

private:
  enum class AudioControlIntent : std::uint8_t { None, Start, Pause, Resume, Seek };
  enum class PreviewRequestPolicy : std::uint8_t { Replace, Coalesce };

  struct ImportOutcome {
    std::filesystem::path path;
    std::optional<assets::AssetRecord> asset;
    std::string error;
  };

  struct PreviewOutcome {
    std::uint64_t epoch{0};
    QImage image;
    std::shared_ptr<const render::CpuFrame> cpu_frame;
    QString error;
    QString gpu_backend;
    QString gpu_diagnostic;
    bool gpu_used{false};
    bool gpu_failed{false};
    bool native_presented{false};
  };

  struct VideoExportOutcome {
    bool succeeded{false};
    bool cancelled{false};
    std::uint64_t frame_count{0};
    std::uint64_t audio_sample_count{0};
    QString error;
  };

  struct CaptionAnalysisOutcome {
    std::uint64_t generation{0};
    std::uint64_t base_revision{0};
    std::vector<audio_render::SilenceRange> silence_ranges;
    std::vector<edit::TimeRange> filler_ranges;
    QString error;
  };

  struct ModelVerificationOutcome {
    std::uint64_t generation{0};
    bool staged_verified{false};
    bool existing_verified{false};
    bool cancelled{false};
  };

  struct ProxyOutcome {
    std::string asset_id;
    std::filesystem::path destination;
    bool succeeded{false};
    bool cancelled{false};
    bool ffv1{false};
    QString error;
  };

  struct CacheJobOutcome {
    std::string asset_id;
    int kind{0};
    std::uint64_t generation{0};
    bool succeeded{false};
    bool cancelled{false};
    bool disk_full{false};
    QImage thumbnail;
    QVector<desktop_ui::WaveformBucketView> waveform;
    QString error;
  };

  [[nodiscard]] static edit::Project makeDefaultProject();
  [[nodiscard]] static std::filesystem::path recoveryDirectory();
  [[nodiscard]] static std::filesystem::path proxyCacheDirectory();
  [[nodiscard]] static std::filesystem::path mediaCacheDirectory();
  [[nodiscard]] std::filesystem::path newWorkingPath(const edit::EntityId& projectId) const;
  [[nodiscard]] bool confirmDiscardChanges();
  [[nodiscard]] bool saveTo(const std::filesystem::path& destination);
  [[nodiscard]] bool loadCheckpoint(const std::filesystem::path& checkpoint);
  [[nodiscard]] bool loadWorkingRecovery(const std::filesystem::path& workingDatabase);
  void installProject(edit::Project project, std::filesystem::path workingPath,
                      std::unique_ptr<store::ProjectStore> store,
                      std::optional<std::filesystem::path> checkpoint);
  void persistSnapshot(std::string_view reason);
  [[nodiscard]] bool apply(edit::EditCommand command, const QString& failureContext);
  [[nodiscard]] bool applyBatch(std::vector<edit::EditCommand> commands,
                                const QString& failureContext);
  void addImportedAsset(assets::AssetRecord asset);
  void generateProxy(const QString& assetId);
  [[nodiscard]] bool reconstructMediaState();
  void enqueueMediaCacheJobs(const assets::AssetRecord& asset);
  void pumpCacheJobs();
  void scheduleRecommendedProxies();
  void pumpProxyQueue();
  void relinkMedia(const QString& assetId);
  void selectMedia(const QString& mediaId);
  void saveAssetMetadata(const desktop_ui::AssetMetadataView& metadata);
  void showMediaCacheBrowser();
  void finishProxyJob(const std::string& asset_id, const ProxyOutcome& outcome);
  void finishVideoExport(const VideoExportOutcome& outcome);
  void clearExportCheckpoint();
  void refreshCacheInventory();
  void handleCacheBudgetChanged(qint64 budgetBytes);
  void removeCacheEntry(const QString& assetId, const QString& kindText);
  void removeCacheAsset(const QString& assetId);
  void evictCacheToBudget();
  void clearMediaCache();
  void loadCachedPreviews(const assets::AssetRecord& record);
  void presentAssetMetadata(const QString& assetId);
  void reregisterAssetMedia(const assets::AssetRecord& record);
  void dropCachedPreview(const std::string& asset_id);
  void cacheLutFile(const std::filesystem::path& path);
  void waitForInFlightCacheJob(bool cancel);
  void refreshViews();
  void refreshMediaView();
  void refreshTimelineView();
  void refreshInspectorView();
  void refreshMixerView();
  void refreshCaptionView();
  void refreshTranscriptionState();
  [[nodiscard]] bool selectedAudioInput(std::filesystem::path& path, edit::TimeRange& range,
                                        edit::EntityId& clipId) const;
  void handleTranscriptionEvent(const jobs::v1::WorkerEvent& event);
  void finishTranscriptionSession(bool abnormal);
  void launchCaptionReviewAnalysis();
  void captionAnalysisFinished();
  void modelVerificationFinished();
  void rebuildPlaybackRegistry();
  void commitTimelineEdit(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                          qint64 durationDelta, int editMode, int editIntent);
  void commitTimelineBatchEdit(const QStringList& clipIds, int destinationTrackIndex,
                               qint64 startDelta, qint64 durationDelta, int editMode,
                               int editIntent);
  void nudgeTimelineSelection(const QStringList& clipIds, int frameCount, int editIntent);
  void setClipSelection(const QStringList& clipIds, const QString& activeClipId);
  void selectMarker(const QString& markerId);
  void selectGap(const QString& gapKey);
  void pruneTimelineSelection(const edit::Sequence& sequence);
  [[nodiscard]] std::vector<edit::EntityId> selectedClipIds() const;
  [[nodiscard]] std::vector<edit::EntityId>
  expandLinkedSelection(const edit::Sequence& sequence,
                        const std::vector<edit::EntityId>& clipIds) const;
  [[nodiscard]] std::uint32_t timelineTimeScale(const edit::Sequence& sequence) const;
  [[nodiscard]] edit::Time timelineTime(qint64 value) const;
  [[nodiscard]] qint64 timelineValue(edit::Time time) const;
  [[nodiscard]] bool applyTrackCommand(edit::EditCommand command, const QString& failureContext);
  void addTrack(int trackKind);
  void renameTrack(const QString& trackId, const QString& name);
  void reorderTrack(const QString& trackId, int destinationIndex);
  void setTrackLocked(const QString& trackId, bool locked);
  void setTrackVisible(const QString& trackId, bool visible);
  void setTrackTargeted(const QString& trackId, bool targeted);
  void removeTrack(const QString& trackId);
  void addMarker(qint64 start);
  void moveMarker(const QString& markerId, qint64 start);
  void renameMarker(const QString& markerId, const QString& name);
  void removeMarker(const QString& markerId);
  void closeGap(const QString& gapKey);
  void requestPreview(PreviewRequestPolicy policy = PreviewRequestPolicy::Replace);
  void launchPreviewRequest();
  void requestSourcePreview();
  void launchSourcePreviewRequest();
  void updateSourceMonitorChrome();
  [[nodiscard]] qint64 sourceDurationUi() const;
  [[nodiscard]] edit::TimeRange markedSourceRange() const;
  void syncPreviewCacheIdentity();
  void startGpuInitialization();
  void startGpuInitializationWithPresentation(const desktop_ui::NativePresentationHandles& handles);
  void attachGpuRenderer(std::shared_ptr<render::GpuRenderer> gpu);
  [[nodiscard]] bool startAudioMasterPlayback();
  void stopAudioPlayback() noexcept;
  [[nodiscard]] static QImage displayImage(const render::CpuFrame& frame);
  [[nodiscard]] const edit::Sequence* currentSequence() const;
  [[nodiscard]] const edit::Asset* assetByTextId(const QString& text) const;
  [[nodiscard]] edit::Time playheadTime() const;
  void setDirty(bool dirty);
  void showError(const QString& title, const QString& message);
  void refreshAudioDevices();

  desktop_ui::EditorWindow& window_;
  std::unique_ptr<edit::TimelineEditor> editor_;
  std::unique_ptr<store::ProjectStore> store_;
  std::shared_ptr<playback::AssetRegistry> playback_registry_;
  std::shared_ptr<audio_render::OriginalAudioRegistry> audio_registry_;
  std::shared_ptr<playback::FfmpegFrameProvider> frame_provider_;
  std::shared_ptr<playback::FfmpegFrameProvider> source_frame_provider_;
  std::shared_ptr<render::CpuRenderer> renderer_;
  std::shared_ptr<render::RenderCache> preview_cache_;
  std::shared_ptr<render::GpuRenderer> gpu_renderer_;
  std::shared_ptr<render::GpuTimelineRenderer> gpu_timeline_renderer_;
  std::unique_ptr<audio::AsyncRealtimeAudioPlayback> audio_playback_;
  std::shared_ptr<audio_render::TimelineAudioRenderer> playback_audio_renderer_;
  std::vector<edit::EntityId> registered_playback_assets_;
  std::vector<edit::EntityId> registered_audio_assets_;
  std::optional<std::filesystem::path> checkpoint_path_;
  std::filesystem::path working_path_;
  std::vector<assets::AssetRecord> imported_assets_;
  std::unique_ptr<media_cache::CacheStore> media_cache_;
  std::unordered_map<std::string, QImage> media_thumbnails_;
  std::unordered_map<std::string, QVector<desktop_ui::WaveformBucketView>> media_waveforms_;
  std::unordered_map<std::string, QString> media_metadata_titles_;
  std::deque<std::pair<std::string, int>> cache_job_queue_;
  bool cache_job_running_{false};
  bool cache_disk_full_{false};
  std::deque<QString> proxy_auto_queue_;
  QString selected_media_id_;
  std::stop_source cache_job_stop_source_;
  QFuture<CacheJobOutcome> cache_job_future_;
  std::uint64_t cache_job_generation_{0};
  struct ProxyJob {
    WorkerHostSession* session{nullptr};
    std::filesystem::path destination;
    bool cancel_requested{false};
  };
  std::unordered_map<std::string, ProxyJob> proxy_jobs_;
  std::vector<std::size_t> visible_caption_indices_;
  QString caption_search_;
  // Timeline selection is deliberately transient. It is never stored in a
  // project snapshot and is pruned against every authoritative revision.
  std::unordered_set<edit::EntityId> selected_clip_ids_;
  std::optional<edit::EntityId> active_clip_id_;
  std::optional<edit::EntityId> selected_transition_id_;
  // Last-applied speed percentage for the active clip, used when only the
  // reverse toggle changes (so the rate is preserved).
  double selected_speed_percent_{100.0};
  std::optional<edit::EntityId> selected_marker_id_;
  QString selected_gap_key_;
  std::uint32_t timeline_time_scale_{48'000};
  qint64 playhead_{0};
  qint64 requested_preview_position_{0};
  std::uint64_t preview_epoch_{0};
  std::uint64_t preview_request_serial_{0};
  std::uint64_t preview_presentation_count_{0};
  std::optional<edit::Revision> preview_cache_revision_;
  std::uint64_t preview_cache_generation_{0};
  bool preview_in_flight_{false};
  std::shared_ptr<const render::CpuFrame> last_preview_frame_;
  bool white_balance_sampling_{false};
  bool gpu_preview_active_{false};
  bool gpu_fallback_latched_{false};
  bool gpu_status_announced_{false};
  QFutureWatcher<std::shared_ptr<render::GpuRenderer>> gpu_init_watcher_;
  std::uint64_t gpu_init_generation_{0};
  bool gpu_init_started_{false};
  bool audio_master_active_{false};
  bool audio_status_announced_{false};
  bool audio_fallback_announced_{false};
  bool audio_start_pending_{false};
  bool audio_session_stale_{true};
  AudioControlIntent audio_control_intent_{AudioControlIntent::None};
  std::uint64_t audio_command_version_{0};
  std::uint64_t last_audio_xrun_count_{0};
  WorkerHostSession* export_session_{nullptr};
  std::filesystem::path export_checkpoint_path_;
  std::filesystem::path export_destination_;
  bool export_cancel_requested_{false};
  struct NormalizationReview {
    bool valid{false};
    edit::Revision revision{};
    double measured_lufs{0.0};
    double gain_db{0.0};
    double target_lufs{-14.0};
    QString error;
  } normalization_review_;
  QFuture<NormalizationReview> normalization_future_;
  QFuture<std::vector<audio::AudioDeviceInfo>> audio_devices_future_;
  QFutureWatcher<NormalizationReview> normalization_watcher_;
  QFutureWatcher<std::vector<audio::AudioDeviceInfo>> audio_devices_watcher_;
  QString selected_audio_device_id_;
  double normalization_target_lufs_{-14.0};
  std::vector<audio::AudioDeviceInfo> known_audio_devices_;
  QTimer audio_device_poll_timer_;
  bool audio_recovery_pending_{false};
  NormalizationCompletionGate normalization_completion_gate_;
  std::uint64_t active_normalization_generation_{0};
  std::stop_source normalization_stop_source_;
  bool export_in_flight_{false};
  QNetworkAccessManager* transcription_network_{nullptr};
  QNetworkReply* model_download_reply_{nullptr};
  QFutureWatcher<ModelVerificationOutcome> model_verification_watcher_;
  std::stop_source model_verification_stop_source_;
  std::uint64_t model_verification_generation_{0};
  QString model_download_staging_;
  QString model_download_cache_root_;
  WorkerHostSession* transcription_session_{nullptr};
  QFile* model_download_file_{nullptr};
  bool model_download_write_failed_{false};
  std::uintmax_t model_download_bytes_written_{0};
  bool model_download_size_rejected_{false};
  bool model_download_user_cancelled_{false};
  QString transcription_job_id_;
  bool transcription_terminal_{false};
  bool transcription_succeeded_{false};
  bool transcription_reported_failure_{false};
  std::uint64_t transcription_base_revision_{0};
  edit::EntityId transcription_clip_id_{};
  edit::TimeRange transcription_clip_range_{};
  edit::TimeRange transcription_source_range_{};
  edit::Rate transcription_playback_rate_{};
  bool transcription_reversed_{false};
  QVector<desktop_ui::CaptionProposalView> caption_proposals_;
  QVector<edit::TimeRange> proposal_cut_ranges_;
  QVector<int> proposal_caption_indices_;
  std::vector<edit::Caption> pending_caption_additions_;
  std::stop_source caption_analysis_stop_source_;
  QFuture<CaptionAnalysisOutcome> caption_analysis_future_;
  QFutureWatcher<CaptionAnalysisOutcome> caption_analysis_watcher_;
  std::uint64_t caption_analysis_generation_{0};
  bool media_paths_updated_on_install_{false};
  bool dirty_{false};
  bool closing_after_confirmation_{false};
  double playback_rate_{0.0};
  double audio_transport_rate_{1.0};
  std::int64_t audio_clock_origin_{0};
  QTimer playback_timer_;
  QElapsedTimer playback_clock_;
  std::optional<edit::EntityId> source_asset_id_;
  qint64 source_playhead_{0};
  std::optional<qint64> source_mark_in_;
  std::optional<qint64> source_mark_out_;
  double source_playback_rate_{0.0};
  std::uint64_t source_preview_epoch_{0};
  std::uint64_t source_preview_serial_{0};
  std::uint64_t source_presentation_count_{0};
  bool source_preview_in_flight_{false};
  QTimer source_playback_timer_;
  QElapsedTimer source_playback_clock_;
};

} // namespace video_editor::app
