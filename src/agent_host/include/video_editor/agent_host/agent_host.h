// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/time.h"

#include <QJsonObject>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace video_editor::edit {
class TimelineEditor;
}

namespace video_editor::playback {
class AssetRegistry;
class FfmpegFrameProvider;
}

namespace video_editor::render {
class CpuRenderer;
}

namespace video_editor::store {
class ProjectStore;
}

namespace video_editor::agent_host {

inline constexpr std::uint32_t kAgentUiTimescale = 48'000;

struct AgentSession final {
  edit::Time playhead{0, kAgentUiTimescale};
  std::vector<edit::EntityId> selected_clip_ids;
  edit::EntityId active_sequence_id{};
};

class AgentHost final {
public:
  AgentHost();
  ~AgentHost();

  AgentHost(const AgentHost&) = delete;
  AgentHost& operator=(const AgentHost&) = delete;

  [[nodiscard]] QJsonObject handleRequest(const QJsonObject& request);

private:
  [[nodiscard]] QJsonObject methodOpenProject(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodNewProject(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodSaveProject(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodReadSession();
  [[nodiscard]] QJsonObject methodSetSession(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodApplyCommands(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodUndo(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodRedo(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodImportMedia(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodInsertMedia(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodGetPreviewFrame(const QJsonObject& request);
  [[nodiscard]] QJsonObject methodGetScopes(const QJsonObject& request);

  void installProject(edit::Project project, std::filesystem::path working_path,
                      std::unique_ptr<store::ProjectStore> project_store,
                      std::optional<std::filesystem::path> checkpoint);
  void registerAllAssets();
  void persistSnapshot(std::string_view reason);
  void persistAndCheckpoint(std::string_view reason);
  [[nodiscard]] edit::Revision resolveExpectedRevision(const QJsonObject& request) const;
  [[nodiscard]] QJsonObject sessionPayload() const;
  [[nodiscard]] QJsonObject compactSessionPayload() const;
  [[nodiscard]] const edit::Sequence* activeSequence() const;
  [[nodiscard]] std::filesystem::path agentWorkingPathForProject(
      const std::filesystem::path& directory, const std::string& project_id) const;

  std::unique_ptr<edit::TimelineEditor> editor_;
  std::unique_ptr<store::ProjectStore> store_;
  std::filesystem::path working_path_;
  std::optional<std::filesystem::path> checkpoint_path_;
  std::shared_ptr<playback::AssetRegistry> registry_;
  std::shared_ptr<playback::FfmpegFrameProvider> frame_provider_;
  std::shared_ptr<render::CpuRenderer> renderer_;
  AgentSession session_;
  bool dirty_{false};
  std::uint64_t preview_epoch_{0};
};

} // namespace video_editor::agent_host
