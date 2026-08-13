// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/edit_model/result.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/export_service/presets.h"
#include "video_editor/render_engine/cpu_renderer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace video_editor::export_service {

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
  // True on the first event after an automatic hardware-to-software restart.
  bool restarted_after_hardware_fallback{false};
};

using ProgressCallback = std::function<void(const ExportProgress&)>;

struct ExportRequest final {
  edit::TimelineSnapshot snapshot;
  std::shared_ptr<render::CpuRenderer> renderer;
  // Required only when include_audio is true. The audio renderer resolves
  // authoritative originals through its OriginalAudioProvider contract.
  std::shared_ptr<audio_render::TimelineAudioRenderer> audio_renderer{};
  std::filesystem::path destination;
  VideoPreset preset{VideoPreset::Ffv1Matroska};
  bool overwrite_existing{false};
  bool include_audio{false};
  // Attempt a runtime-probed VP9 hardware encoder for creator delivery. The
  // exporter retries the complete atomic export with libvpx-vp9 when device,
  // frame upload, or hardware encode initialization fails.
  bool prefer_hardware_encoder{true};
  std::stop_token cancellation;
  ProgressCallback progress;

  // NEW creator-ready controls:
  PlatformPreset platform_preset{PlatformPreset::ReferenceFfv1};
  CaptionExportMode caption_mode{CaptionExportMode::None};
  SidecarFormat sidecar_format{SidecarFormat::Srt};
  // Resolution override (0 = use sequence dimensions). Must be even for H.264.
  std::uint32_t override_width{0};
  std::uint32_t override_height{0};
  // Frame-rate override (0/0 = use sequence frame rate).
  std::uint32_t override_frame_rate_num{0};
  std::uint32_t override_frame_rate_den{0};
  // Audio bitrate override (0 = preset default).
  std::uint32_t override_audio_bitrate{0};
  // Video bitrate override (0 = preset default). For VP9, video_quality in
  // the range 0..63 selects constant-quality mode instead of a bitrate.
  std::uint64_t override_video_bitrate{0};
  std::optional<int> video_quality{};
  // Captions to burn in / export as sidecar. Drawn from the snapshot's
  // sequence.captions by the caller, or left empty for None mode.
  std::vector<edit::Caption> captions;
};

enum class ExportErrorCode : std::uint8_t {
  InvalidRequest,
  AudioRendererRequired,
  AudioRenderFailed,
  DestinationExists,
  EncoderUnavailable,
  HardwareEncoderFailed,
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
  // Present when an audio-render failure or cancellation preserves its typed
  // renderer cause.
  std::optional<audio_render::AudioRenderError> audio_render_error{};
};

struct ExportResult final {
  std::filesystem::path destination;
  VideoPreset preset{VideoPreset::Ffv1Matroska};
  std::string container;
  std::string video_codec;
  // Actual FFmpeg encoder selected. Hardware use is reported separately.
  std::string video_encoder;
  std::string audio_codec;
  std::string audio_encoder;
  std::uint64_t frame_count{0};
  std::uint64_t audio_sample_count{0};
  edit::Time source_timeline_duration{};
  edit::Time encoded_video_duration{};
  edit::Time encoded_audio_duration{};
  bool video_exported{true};
  bool audio_exported{false};
  bool hardware_encoder_used{false};
  // Path to the sidecar caption file if one was written alongside the media.
  std::filesystem::path caption_sidecar_path;
};

using ExportOutcome = edit::Result<ExportResult, ExportError>;

// Renders the immutable snapshot at full sequence resolution, with original
// assets authoritative and expensive effects enabled, then atomically commits
// the finished sibling file to request.destination.
[[nodiscard]] ExportOutcome export_video(const ExportRequest& request);

} // namespace video_editor::export_service
