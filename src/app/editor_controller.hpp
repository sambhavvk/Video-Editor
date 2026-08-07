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
#include <optional>
#include <stop_token>
#include <unordered_map>
#include <vector>

class QEvent;

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
} // namespace video_editor::render

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
  void searchTranscript(const QString& query);
  void chooseVideoExport(const QString& presetId);

private:
  struct ImportOutcome {
    std::filesystem::path path;
    std::optional<assets::AssetRecord> asset;
    std::string error;
  };

  struct PreviewOutcome {
    std::uint64_t epoch{0};
    QImage image;
    QString error;
  };

  struct VideoExportOutcome {
    bool succeeded{false};
    bool cancelled{false};
    std::uint64_t frame_count{0};
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
  void refreshCaptionView();
  void rebuildPlaybackRegistry();
  void commitTimelineEdit(const QString& clipId, int destinationTrackIndex, qint64 startDelta,
                          qint64 durationDelta, int editMode, int editIntent);
  void requestPreview();
  void launchPreviewRequest();
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
  std::shared_ptr<playback::FfmpegFrameProvider> frame_provider_;
  std::shared_ptr<render::CpuRenderer> renderer_;
  std::vector<edit::EntityId> registered_playback_assets_;
  std::optional<std::filesystem::path> checkpoint_path_;
  std::filesystem::path working_path_;
  std::vector<assets::AssetRecord> imported_assets_;
  std::unordered_map<std::string, std::shared_ptr<std::stop_source>> proxy_jobs_;
  std::vector<QFuture<ProxyOutcome>> proxy_futures_;
  std::vector<std::size_t> visible_caption_indices_;
  QString caption_search_;
  std::optional<edit::EntityId> selected_clip_;
  qint64 playhead_{0};
  qint64 requested_preview_position_{0};
  std::uint64_t preview_epoch_{0};
  bool preview_in_flight_{false};
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
