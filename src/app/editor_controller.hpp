// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/asset_service.h"
#include "video_editor/edit_model/edit_model.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QEvent;
class QVariant;

namespace video_editor::desktop_ui {
class EditorWindow;
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
class GpuRenderer;
class GpuTimelineRenderer;
} // namespace video_editor::render

namespace video_editor::audio_render {
class OriginalAudioRegistry;
}

namespace video_editor::audio {
class AsyncRealtimeAudioPlayback;
}

namespace video_editor::app {

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
  void updateSelectedClipProperty(const QString& parameterId, const QVariant& value);
  void toggleSelectedClipKeyframe(const QString& parameterId);
  void addTitleClip();
  void setTransitionSelection(const QString& transitionId);
  void updateTransitionDuration(const QString& transitionId, qint64 duration);
  void removeTransition(const QString& transitionId);
  void changeTransitionPreset(const QString& transitionId, const QString& kind);
  void setAudioTrackMuted(int trackIndex, bool muted);
  void setAudioTrackSolo(int trackIndex, bool soloed);
  void setAudioTrackGain(int trackIndex, double gainDb);
  void setAudioTrackPan(int trackIndex, double pan);
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
    QString error;
    QString gpu_backend;
    QString gpu_diagnostic;
    bool gpu_used{false};
    bool gpu_failed{false};
  };

  struct VideoExportOutcome {
    bool succeeded{false};
    bool cancelled{false};
    std::uint64_t frame_count{0};
    std::uint64_t audio_sample_count{0};
    QString error;
  };

  struct ProxyOutcome {
    std::string asset_id;
    std::filesystem::path destination;
    bool succeeded{false};
    bool cancelled{false};
    bool ffv1{false};
    QString error;
  };

  [[nodiscard]] static edit::Project makeDefaultProject();
  [[nodiscard]] static std::filesystem::path recoveryDirectory();
  [[nodiscard]] static std::filesystem::path proxyCacheDirectory();
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
  void refreshViews();
  void refreshMediaView();
  void refreshTimelineView();
  void refreshInspectorView();
  void refreshMixerView();
  void refreshCaptionView();
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
  [[nodiscard]] bool startAudioMasterPlayback();
  void stopAudioPlayback() noexcept;
  [[nodiscard]] static QImage displayImage(const render::CpuFrame& frame);
  [[nodiscard]] const edit::Sequence* currentSequence() const;
  [[nodiscard]] const edit::Asset* assetByTextId(const QString& text) const;
  [[nodiscard]] edit::Time playheadTime() const;
  void setDirty(bool dirty);
  void showError(const QString& title, const QString& message);

  desktop_ui::EditorWindow& window_;
  std::unique_ptr<edit::TimelineEditor> editor_;
  std::unique_ptr<store::ProjectStore> store_;
  std::shared_ptr<playback::AssetRegistry> playback_registry_;
  std::shared_ptr<audio_render::OriginalAudioRegistry> audio_registry_;
  std::shared_ptr<playback::FfmpegFrameProvider> frame_provider_;
  std::shared_ptr<render::CpuRenderer> renderer_;
  std::shared_ptr<render::GpuRenderer> gpu_renderer_;
  std::shared_ptr<render::GpuTimelineRenderer> gpu_timeline_renderer_;
  std::unique_ptr<audio::AsyncRealtimeAudioPlayback> audio_playback_;
  std::vector<edit::EntityId> registered_playback_assets_;
  std::vector<edit::EntityId> registered_audio_assets_;
  std::optional<std::filesystem::path> checkpoint_path_;
  std::filesystem::path working_path_;
  std::vector<assets::AssetRecord> imported_assets_;
  std::unordered_map<std::string, std::shared_ptr<std::stop_source>> proxy_jobs_;
  std::vector<QFuture<ProxyOutcome>> proxy_futures_;
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
  bool preview_in_flight_{false};
  bool gpu_preview_active_{false};
  bool gpu_fallback_latched_{false};
  bool gpu_status_announced_{false};
  bool audio_master_active_{false};
  bool audio_status_announced_{false};
  bool audio_fallback_announced_{false};
  bool shuttle_silence_announced_{false};
  bool audio_start_pending_{false};
  bool audio_session_stale_{true};
  AudioControlIntent audio_control_intent_{AudioControlIntent::None};
  std::uint64_t audio_command_version_{0};
  std::uint64_t last_audio_xrun_count_{0};
  std::stop_source export_stop_source_;
  QFuture<VideoExportOutcome> export_future_;
  bool export_in_flight_{false};
  bool dirty_{false};
  bool closing_after_confirmation_{false};
  double playback_rate_{0.0};
  QTimer playback_timer_;
  QElapsedTimer playback_clock_;
};

} // namespace video_editor::app
