// SPDX-License-Identifier: MPL-2.0
#include "video_editor/asset_service/edit_asset.h"

#include "video_editor/edit_model/entity_id.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace video_editor::assets {
namespace {

[[nodiscard]] std::string utf8_string_from_path(const std::filesystem::path& value) {
  const auto utf8 = value.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace

edit::Asset asset_from_record(const AssetRecord& record) {
  edit::Asset model_asset;
  const auto parsed_id = edit::EntityId::parse(record.id);
  model_asset.id = parsed_id.value_or(edit::EntityId::generate());
  model_asset.name = utf8_string_from_path(record.uri.filename());
  model_asset.source_uri = utf8_string_from_path(record.uri);
  model_asset.fingerprint = record.fingerprint.quick_sha256;
  if (record.descriptor.duration_microseconds.has_value() &&
      *record.descriptor.duration_microseconds > 0) {
    model_asset.duration = edit::Time(*record.descriptor.duration_microseconds, 1'000'000);
  } else {
    model_asset.duration = edit::Time(5, 1);
  }
  model_asset.metadata["container"] = record.descriptor.format_name;
  if (record.descriptor.format_name == "image2-sequence") {
    model_asset.metadata["media_kind"] = "image_sequence";
  } else if (model_asset.duration == edit::Time(5, 1) &&
             (record.descriptor.format_name.find("image") != std::string::npos ||
              record.descriptor.format_name.find("png") != std::string::npos ||
              record.descriptor.format_name.find("jpeg") != std::string::npos ||
              record.descriptor.format_name.find("pipe") != std::string::npos)) {
    model_asset.metadata["media_kind"] = "still";
  }
  if (record.descriptor.start_time_microseconds.has_value()) {
    model_asset.metadata["timecode_start_us"] =
        std::to_string(*record.descriptor.start_time_microseconds);
  }
  for (const auto& stream : record.descriptor.streams) {
    if (stream.start_time.has_value() && stream.kind == media::StreamKind::Video &&
        !model_asset.metadata.contains("stream_timecode_start_us")) {
      model_asset.metadata["stream_timecode_start_us"] = std::to_string(*stream.start_time);
    }
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
  return model_asset;
}

} // namespace video_editor::assets
