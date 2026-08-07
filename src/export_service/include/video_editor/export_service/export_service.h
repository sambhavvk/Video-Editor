// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/render_engine/cpu_renderer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace video_editor::export_service {

// The beta export service is intentionally video-only. Audio is represented in
// both the request and result so callers cannot accidentally imply that a muxed
// audio program was produced.
enum class VideoPreset : std::uint8_t {
  Ffv1Matroska,
  ProRes422HqMov,
};

struct PresetInfo final {
  VideoPreset preset{VideoPreset::Ffv1Matroska};
  std::string display_name;
  std::string container;
  std::string codec;
  bool available{false};
  bool lossless{false};
};

[[nodiscard]] PresetInfo preset_info(VideoPreset preset);

struct ExportProgress final {
  std::uint64_t completed_frames{0};
  std::uint64_t total_frames{0};
  edit::Time timeline_time{};
  double fraction{0.0};
};

using ProgressCallback = std::function<void(const ExportProgress&)>;

struct ExportRequest final {
  edit::TimelineSnapshot snapshot;
  std::shared_ptr<render::CpuRenderer> renderer;
  std::filesystem::path destination;
  VideoPreset preset{VideoPreset::Ffv1Matroska};
  bool overwrite_existing{false};
  bool include_audio{false};
  std::stop_token cancellation;
  ProgressCallback progress;
};

enum class ExportErrorCode : std::uint8_t {
  InvalidRequest,
  AudioNotSupported,
  DestinationExists,
  EncoderUnavailable,
  Cancelled,
  RenderFailed,
  EncodingFailed,
  IoFailed,
  CommitFailed,
  ProgressCallbackFailed,
};

struct ExportError final {
  ExportErrorCode code{ExportErrorCode::InvalidRequest};
  std::string message;
};

struct ExportResult final {
  std::filesystem::path destination;
  VideoPreset preset{VideoPreset::Ffv1Matroska};
  std::string container;
  std::string video_codec;
  std::uint64_t frame_count{0};
  edit::Time source_timeline_duration{};
  edit::Time encoded_video_duration{};
  bool video_exported{true};
  bool audio_exported{false};
};

using ExportOutcome = edit::Result<ExportResult, ExportError>;

// Renders the immutable snapshot at full sequence resolution, with original
// assets authoritative and expensive effects enabled, then atomically commits
// the finished sibling file to request.destination.
[[nodiscard]] ExportOutcome export_video(const ExportRequest& request);

} // namespace video_editor::export_service
