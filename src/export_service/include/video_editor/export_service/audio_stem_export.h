// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace video_editor::export_service {

struct AudioStemExportRequest final {
  edit::TimelineSnapshot snapshot;
  std::shared_ptr<audio_render::TimelineAudioRenderer> audio_renderer;
  std::vector<edit::EntityId> clip_ids;
  edit::TimeRange timeline_range{};
  edit::Time handle_before{};
  edit::Time handle_after{};
  std::filesystem::path destination_directory;
  std::uint32_t left_channel{0};
  std::uint32_t right_channel{1};
  std::stop_token cancellation{};
};

struct AudioStemFileResult final {
  std::filesystem::path path;
  edit::EntityId clip_id{};
  std::uint64_t sample_count{0};
  edit::Time source_start_reference{};
  std::vector<std::string> warnings;
};

struct AudioStemExportResult final {
  std::vector<AudioStemFileResult> stems;
};

[[nodiscard]] edit::Result<AudioStemExportResult, std::string>
export_production_audio_stems(const AudioStemExportRequest& request);

} // namespace video_editor::export_service
