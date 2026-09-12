// SPDX-License-Identifier: MPL-2.0
#include "video_editor/project_codec/project_codec.h"

#include "project_snapshot.pb.h"
#include "video_editor/edit_model/timeline_editor.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/unknown_field_set.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace video_editor::project_codec {
namespace {

namespace wire = ::video_editor::persistence::v1;

class IdRegistry;
[[nodiscard]] bool validUtf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if (byte <= 0x7FU) {
      code_point = byte;
    } else if ((byte & 0xE0U) == 0xC0U) {
      code_point = byte & 0x1FU;
      continuation = 1;
    } else if ((byte & 0xF0U) == 0xE0U) {
      code_point = byte & 0x0FU;
      continuation = 2;
    } else if ((byte & 0xF8U) == 0xF0U) {
      code_point = byte & 0x07U;
      continuation = 3;
    } else {
      return false;
    }
    if (index + continuation >= text.size())
      return false;
    for (std::size_t i = 0; i < continuation; ++i) {
      const auto next = static_cast<unsigned char>(text[index + 1U + i]);
      if ((next & 0xC0U) != 0x80U)
        return false;
      code_point = (code_point << 6U) | (next & 0x3FU);
    }
    if ((continuation == 1U && code_point < 0x80U) || (continuation == 2U && code_point < 0x800U) ||
        (continuation == 3U && code_point < 0x10000U) || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU))
      return false;
    index += continuation + 1U;
  }
  return true;
}
void requirePresent(bool present, std::string_view path);
[[nodiscard]] edit::EntityId decodeId(const wire::EntityId& value, std::string_view path,
                                      IdRegistry* registry = nullptr, bool allow_nil = false);
[[nodiscard]] edit::TimeRange decodeRange(const wire::TimeRange& value, std::string_view path);
[[nodiscard]] edit::ColorRgba decodeColor(const wire::ColorRgba& value, std::string_view path);

constexpr std::size_t kMaximumTitleTextBytes = 64U * 1024U;
constexpr std::size_t kMaximumTitleFontFamilyBytes = 1024U;
constexpr double kMinimumTitleFontSize = 1.0;
constexpr double kMaximumTitleFontSize = 4096.0;
constexpr double kMinimumAudioGainDb = -96.0;
constexpr double kMaximumAudioGainDb = 24.0;

[[noreturn]] void fail(CodecErrorCode code, std::string field_path, std::string message) {
  throw CodecException(CodecError{code, std::move(message), std::move(field_path)});
}

void require(bool condition, CodecErrorCode code, std::string_view field_path,
             std::string_view message) {
  if (!condition) {
    fail(code, std::string(field_path), std::string(message));
  }
}

[[nodiscard]] std::string childPath(std::string_view parent, std::string_view child) {
  if (parent.empty()) {
    return std::string(child);
  }
  std::string result(parent);
  result.push_back('.');
  result.append(child);
  return result;
}

[[nodiscard]] std::string indexedPath(std::string_view parent, std::string_view field,
                                      std::size_t index) {
  auto result = childPath(parent, field);
  result.push_back('[');
  result.append(std::to_string(index));
  result.push_back(']');
  return result;
}

class IdRegistry final {
public:
  void add(edit::EntityId id, std::string_view path) {
    require(!id.isNil(), CodecErrorCode::InvalidField, path, "entity id cannot be nil");
    if (!ids_.emplace(id).second) {
      fail(CodecErrorCode::DuplicateId, std::string(path),
           "entity id is duplicated within the project snapshot");
    }
  }

private:
  std::unordered_set<edit::EntityId> ids_;
};

void encodeId(edit::EntityId value, wire::EntityId* output, std::string_view path,
              IdRegistry* registry = nullptr, bool allow_nil = false) {
  require(output != nullptr, CodecErrorCode::SerializationFailed, path,
          "protobuf id destination is null");
  require(allow_nil || !value.isNil(), CodecErrorCode::InvalidField, path,
          "entity id cannot be nil");
  if (registry != nullptr) {
    registry->add(value, path);
  }
  output->set_value(value.bytes().data(), value.bytes().size());
}

void encodeTime(edit::Time value, wire::Time* output) {
  output->set_value(value.value());
  output->set_timescale(value.timescale());
}

void encodeRange(const edit::TimeRange& value, wire::TimeRange* output) {
  encodeTime(value.start, output->mutable_start());
  encodeTime(value.duration, output->mutable_duration());
}

void encodeRate(const edit::Rate& value, wire::Rate* output) {
  output->set_numerator(value.numerator());
  output->set_denominator(value.denominator());
}

void requireFinite(double value, std::string_view path) {
  require(std::isfinite(value), CodecErrorCode::InvalidField, path,
          "floating-point value must be finite");
}

void encodeVec2(const edit::Vec2& value, wire::Vec2* output, std::string_view path) {
  requireFinite(value.x, childPath(path, "x"));
  requireFinite(value.y, childPath(path, "y"));
  output->set_x(value.x);
  output->set_y(value.y);
}

void encodeColor(const edit::ColorRgba& value, wire::ColorRgba* output, std::string_view path) {
  requireFinite(value.red, childPath(path, "red"));
  requireFinite(value.green, childPath(path, "green"));
  requireFinite(value.blue, childPath(path, "blue"));
  requireFinite(value.alpha, childPath(path, "alpha"));
  output->set_red(value.red);
  output->set_green(value.green);
  output->set_blue(value.blue);
  output->set_alpha(value.alpha);
}

[[nodiscard]] wire::TitleAlignment encodeTitleAlignment(const edit::TitleHorizontalAlignment value,
                                                        std::string_view path) {
  if (value == edit::TitleHorizontalAlignment::Left) {
    return wire::TITLE_ALIGNMENT_LEFT;
  }
  if (value == edit::TitleHorizontalAlignment::Center) {
    return wire::TITLE_ALIGNMENT_CENTER;
  }
  if (value == edit::TitleHorizontalAlignment::Right) {
    return wire::TITLE_ALIGNMENT_RIGHT;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "unknown title alignment");
}

void encodeTitle(const edit::Title& value, wire::Title* output, std::string_view path) {
  require(value.text.size() <= kMaximumTitleTextBytes, CodecErrorCode::InvalidField,
          childPath(path, "text"), "title text exceeds the byte limit");
  require(!value.font_family.empty(), CodecErrorCode::InvalidField, childPath(path, "font_family"),
          "title font family cannot be empty");
  require(value.font_family.size() <= kMaximumTitleFontFamilyBytes, CodecErrorCode::InvalidField,
          childPath(path, "font_family"), "title font family exceeds the byte limit");
  requireFinite(value.font_size, childPath(path, "font_size"));
  require(value.font_size >= kMinimumTitleFontSize && value.font_size <= kMaximumTitleFontSize,
          CodecErrorCode::InvalidField, childPath(path, "font_size"),
          "title font size must be within the supported bounds");
  output->set_text(value.text);
  output->set_font_family(value.font_family);
  output->set_font_size(value.font_size);
  encodeColor(value.foreground_color, output->mutable_text_color(), childPath(path, "text_color"));
  encodeColor(value.background_color, output->mutable_background_color(),
              childPath(path, "background_color"));
  output->set_alignment(
      encodeTitleAlignment(value.horizontal_alignment, childPath(path, "alignment")));
  output->set_bold(value.bold);
  output->set_italic(value.italic);
}

void encodeClipTitle(const edit::Clip& value, wire::Clip* output, std::string_view path) {
  if (value.kind == edit::ClipKind::Title) {
    require(value.title.has_value(), CodecErrorCode::InvalidField, childPath(path, "title"),
            "title clips require a title payload");
    encodeTitle(*value.title, output->mutable_title(), childPath(path, "title"));
  } else {
    require(!value.title.has_value(), CodecErrorCode::InvalidField, childPath(path, "title"),
            "media clips cannot carry a title payload");
  }
}

void encodeClipNestedSequence(const edit::Clip& value, wire::Clip* output, std::string_view path) {
  if (value.kind == edit::ClipKind::NestedSequence) {
    require(value.nested_sequence_id.has_value(), CodecErrorCode::InvalidField,
            childPath(path, "nested_sequence_id"),
            "nested sequence clips require nested_sequence_id");
    encodeId(*value.nested_sequence_id, output->mutable_nested_sequence_id(),
             childPath(path, "nested_sequence_id"));
  } else {
    require(!value.nested_sequence_id.has_value(), CodecErrorCode::InvalidField,
            childPath(path, "nested_sequence_id"),
            "only nested sequence clips may reference a nested sequence");
  }
}

[[nodiscard]] wire::TransitionKind encodeTransitionKind(const edit::TransitionKind value,
                                                        std::string_view path) {
  if (value == edit::TransitionKind::CrossDissolve) {
    return wire::TRANSITION_KIND_CROSS_DISSOLVE;
  }
  if (value == edit::TransitionKind::DipToBlack) {
    return wire::TRANSITION_KIND_DIP_TO_BLACK;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "unknown transition kind");
}

void encodeTransition(const edit::Transition& value, wire::Transition* output,
                      std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.outgoing_clip_id, output->mutable_outgoing_clip_id(),
           childPath(path, "outgoing_clip_id"));
  encodeId(value.incoming_clip_id, output->mutable_incoming_clip_id(),
           childPath(path, "incoming_clip_id"));
  encodeRange(value.range, output->mutable_timeline_range());
  output->set_kind(encodeTransitionKind(value.kind, childPath(path, "kind")));
  output->set_enabled(value.enabled);
}

void encodeTransitions(const edit::Sequence& value, wire::Sequence* output, std::string_view path,
                       IdRegistry& ids) {
  for (std::size_t index = 0; index < value.transitions.size(); ++index) {
    encodeTransition(value.transitions[index], output->add_transitions(),
                     indexedPath(path, "transitions", index), ids);
  }
}

void encodeMetadata(const std::map<std::string, std::string, std::less<>>& value,
                    google::protobuf::RepeatedPtrField<wire::StringEntry>* output,
                    std::string_view path) {
  for (const auto& [key, item] : value) {
    require(!key.empty(), CodecErrorCode::InvalidField, path, "metadata keys cannot be empty");
    auto* entry = output->Add();
    entry->set_key(key);
    entry->set_value(item);
  }
}

[[nodiscard]] wire::KeyframeInterpolation encodeInterpolation(edit::KeyframeInterpolation value) {
  switch (value) {
  case edit::KeyframeInterpolation::Hold:
    return wire::KEYFRAME_INTERPOLATION_HOLD;
  case edit::KeyframeInterpolation::Linear:
    return wire::KEYFRAME_INTERPOLATION_LINEAR;
  case edit::KeyframeInterpolation::Bezier:
    return wire::KEYFRAME_INTERPOLATION_BEZIER;
  }
  fail(CodecErrorCode::InvalidField, "keyframe.interpolation", "unknown keyframe interpolation");
}

void encodeEffectValue(const edit::EffectValue& value, wire::EffectValue* output,
                       std::string_view path) {
  std::visit(
      [&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
          output->set_integer_value(item);
        } else if constexpr (std::is_same_v<T, double>) {
          requireFinite(item, path);
          output->set_double_value(item);
        } else if constexpr (std::is_same_v<T, bool>) {
          output->set_boolean_value(item);
        } else if constexpr (std::is_same_v<T, std::string>) {
          output->set_string_value(item);
        } else if constexpr (std::is_same_v<T, edit::Time>) {
          encodeTime(item, output->mutable_time_value());
        } else if constexpr (std::is_same_v<T, edit::Vec2>) {
          encodeVec2(item, output->mutable_vec2_value(), path);
        } else if constexpr (std::is_same_v<T, edit::ColorRgba>) {
          encodeColor(item, output->mutable_color_value(), path);
        }
      },
      value);
}

void encodeKeyframe(const edit::Keyframe& value, wire::Keyframe* output, std::string_view path,
                    IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.time.isNegative(), CodecErrorCode::InvalidField, childPath(path, "time"),
          "keyframe time cannot be negative");
  encodeTime(value.time, output->mutable_time());
  encodeEffectValue(value.value, output->mutable_value(), childPath(path, "value"));
  output->set_interpolation(encodeInterpolation(value.interpolation));
  encodeVec2(value.incoming_control, output->mutable_incoming_control(),
             childPath(path, "incoming_control"));
  encodeVec2(value.outgoing_control, output->mutable_outgoing_control(),
             childPath(path, "outgoing_control"));
}

void encodeParameter(const edit::EffectParameter& value, wire::EffectParameter* output,
                     std::string_view path, IdRegistry& ids) {
  require(!value.id.empty(), CodecErrorCode::InvalidField, childPath(path, "id"),
          "effect parameter id cannot be empty");
  output->set_id(value.id);
  encodeEffectValue(value.value, output->mutable_value(), childPath(path, "value"));
  std::optional<edit::Time> previous_time;
  for (std::size_t index = 0; index < value.keyframes.size(); ++index) {
    const auto keyframe_path = indexedPath(path, "keyframes", index);
    if (previous_time) {
      require(value.keyframes[index].time > *previous_time, CodecErrorCode::InvalidField,
              childPath(keyframe_path, "time"), "keyframes must be strictly ordered by time");
    }
    encodeKeyframe(value.keyframes[index], output->add_keyframes(), keyframe_path, ids);
    previous_time = value.keyframes[index].time;
  }
}

void encodeEffect(const edit::Effect& value, wire::Effect* output, std::string_view path,
                  IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.type.empty(), CodecErrorCode::InvalidField, childPath(path, "type"),
          "effect type cannot be empty");
  require(value.version > 0, CodecErrorCode::InvalidField, childPath(path, "version"),
          "effect version must be non-zero");
  output->set_type(value.type);
  output->set_version(value.version);
  output->set_enabled(value.enabled);
  output->set_known(value.known);
  std::size_t index = 0;
  for (const auto& [key, parameter] : value.parameters) {
    const auto parameter_path = indexedPath(path, "parameters", index++);
    require(key == parameter.id, CodecErrorCode::InvalidField, parameter_path,
            "effect parameter map key must match its id");
    encodeParameter(parameter, output->add_parameters(), parameter_path, ids);
  }
  output->set_opaque_payload(value.opaque_payload.data(), value.opaque_payload.size());
}

void encodeTransform(const edit::Transform& value, wire::Transform* output, std::string_view path) {
  encodeVec2(value.position, output->mutable_position(), childPath(path, "position"));
  encodeVec2(value.scale, output->mutable_scale(), childPath(path, "scale"));
  requireFinite(value.rotation_degrees, childPath(path, "rotation_degrees"));
  requireFinite(value.anchor_x, childPath(path, "anchor_x"));
  requireFinite(value.anchor_y, childPath(path, "anchor_y"));
  requireFinite(value.crop_left, childPath(path, "crop_left"));
  requireFinite(value.crop_top, childPath(path, "crop_top"));
  requireFinite(value.crop_right, childPath(path, "crop_right"));
  requireFinite(value.crop_bottom, childPath(path, "crop_bottom"));
  requireFinite(value.opacity, childPath(path, "opacity"));
  require(value.opacity >= 0.0 && value.opacity <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "opacity"), "opacity must be between zero and one");
  require(value.crop_left >= 0.0 && value.crop_left <= 1.0 && value.crop_top >= 0.0 &&
              value.crop_top <= 1.0 && value.crop_right >= 0.0 && value.crop_right <= 1.0 &&
              value.crop_bottom >= 0.0 && value.crop_bottom <= 1.0,
          CodecErrorCode::InvalidField, path, "crop values must be between zero and one");
  output->set_rotation_degrees(value.rotation_degrees);
  output->set_anchor_x(value.anchor_x);
  output->set_anchor_y(value.anchor_y);
  output->set_crop_left(value.crop_left);
  output->set_crop_top(value.crop_top);
  output->set_crop_right(value.crop_right);
  output->set_crop_bottom(value.crop_bottom);
  output->set_opacity(value.opacity);
}

[[nodiscard]] wire::TrackKind encodeTrackKind(edit::TrackKind value) {
  switch (value) {
  case edit::TrackKind::Video:
    return wire::TRACK_KIND_VIDEO;
  case edit::TrackKind::Audio:
    return wire::TRACK_KIND_AUDIO;
  case edit::TrackKind::Caption:
    return wire::TRACK_KIND_CAPTION;
  }
  fail(CodecErrorCode::InvalidField, "track.kind", "unknown track kind");
}

[[nodiscard]] wire::ClipKind encodeClipKind(edit::ClipKind value) {
  switch (value) {
  case edit::ClipKind::Video:
    return wire::CLIP_KIND_VIDEO;
  case edit::ClipKind::Audio:
    return wire::CLIP_KIND_AUDIO;
  case edit::ClipKind::Title:
    return wire::CLIP_KIND_TITLE;
  case edit::ClipKind::NestedSequence:
    return wire::CLIP_KIND_NESTED_SEQUENCE;
  }
  fail(CodecErrorCode::InvalidField, "clip.kind", "unknown clip kind");
}

[[nodiscard]] wire::BlendMode encodeBlendMode(edit::BlendMode value) {
  switch (value) {
  case edit::BlendMode::Normal:
    return wire::BLEND_MODE_NORMAL;
  case edit::BlendMode::Add:
    return wire::BLEND_MODE_ADD;
  case edit::BlendMode::Multiply:
    return wire::BLEND_MODE_MULTIPLY;
  case edit::BlendMode::Screen:
    return wire::BLEND_MODE_SCREEN;
  case edit::BlendMode::Overlay:
    return wire::BLEND_MODE_OVERLAY;
  }
  fail(CodecErrorCode::InvalidField, "clip.blend_mode", "unknown blend mode");
}

void encodeAsset(const edit::Asset& value, wire::Asset* output, std::string_view path,
                 IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.duration.isNegative(), CodecErrorCode::InvalidField, childPath(path, "duration"),
          "asset duration cannot be negative");
  if (value.has_video) {
    require(value.width > 0 && value.height > 0, CodecErrorCode::InvalidField, path,
            "video assets require non-zero dimensions");
  }
  if (value.has_audio) {
    require(value.audio_sample_rate > 0 && value.audio_channels > 0, CodecErrorCode::InvalidField,
            path, "audio assets require a sample rate and channel count");
  }
  output->set_name(value.name);
  output->set_source_uri(value.source_uri);
  output->set_fingerprint(value.fingerprint);
  encodeTime(value.duration, output->mutable_duration());
  output->set_has_video(value.has_video);
  output->set_has_audio(value.has_audio);
  output->set_width(value.width);
  output->set_height(value.height);
  if (value.nominal_frame_rate) {
    encodeRate(*value.nominal_frame_rate, output->mutable_nominal_frame_rate());
  }
  output->set_audio_sample_rate(value.audio_sample_rate);
  output->set_audio_channels(value.audio_channels);
  encodeMetadata(value.metadata, output->mutable_metadata(), childPath(path, "metadata"));
  if (value.bin_id) {
    encodeId(*value.bin_id, output->mutable_bin_id(), childPath(path, "bin_id"));
  }
  if (!value.display_title.empty()) {
    output->set_display_title(value.display_title);
  }
  for (const auto& tag : value.tags) {
    output->add_tags(tag);
  }
  if (!value.notes.empty()) {
    output->set_notes(value.notes);
  }
  if (value.rating != 0) {
    output->set_rating(value.rating);
  }
  if (!value.production.scene.empty()) {
    output->set_production_scene(value.production.scene);
  }
  if (!value.production.shot.empty()) {
    output->set_production_shot(value.production.shot);
  }
  if (!value.production.take.empty()) {
    output->set_production_take(value.production.take);
  }
  if (!value.production.camera.empty()) {
    output->set_production_camera(value.production.camera);
  }
  if (!value.production.reel.empty()) {
    output->set_production_reel(value.production.reel);
  }
  if (!value.production.audio_roll.empty()) {
    output->set_production_audio_roll(value.production.audio_roll);
  }
  if (!value.production.source_timecode.empty()) {
    output->set_production_source_timecode(value.production.source_timecode);
  }
  if (value.production.preferred_take) {
    output->set_production_preferred_take(true);
  }
  for (const auto& channel : value.audio_channel_map) {
    auto* entry = output->add_audio_channel_map();
    entry->set_index(channel.index);
    entry->set_label(channel.label);
  }
  if (value.monitor_left_channel != 0) {
    output->set_monitor_left_channel(value.monitor_left_channel);
  }
  if (value.monitor_right_channel != 1) {
    output->set_monitor_right_channel(value.monitor_right_channel);
  }
}

void encodeClip(const edit::Clip& value, wire::Clip* output, std::string_view path,
                IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.asset_id, output->mutable_asset_id(), childPath(path, "asset_id"), nullptr,
           value.kind == edit::ClipKind::Title || value.kind == edit::ClipKind::NestedSequence);
  output->set_kind(encodeClipKind(value.kind));
  output->set_name(value.name);
  encodeRange(value.timeline_range, output->mutable_timeline_range());
  encodeRange(value.source_range, output->mutable_source_range());
  encodeRate(value.playback_rate, output->mutable_playback_rate());
  output->set_reversed(value.reversed);
  if (value.linked_group) {
    encodeId(*value.linked_group, output->mutable_linked_group(), childPath(path, "linked_group"));
  }
  encodeTransform(value.transform, output->mutable_transform(), childPath(path, "transform"));
  output->set_blend_mode(encodeBlendMode(value.blend_mode));
  requireFinite(value.audio_gain_db, childPath(path, "audio_gain_db"));
  requireFinite(value.audio_pan, childPath(path, "audio_pan"));
  require(value.audio_gain_db >= kMinimumAudioGainDb && value.audio_gain_db <= kMaximumAudioGainDb,
          CodecErrorCode::InvalidField, childPath(path, "audio_gain_db"),
          "audio gain must be within [-96, 24] dB");
  require(value.audio_pan >= -1.0 && value.audio_pan <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "audio_pan"), "audio pan must be between minus one and one");
  require(!value.fade_in.isNegative() && !value.fade_out.isNegative(), CodecErrorCode::InvalidField,
          path, "clip fades cannot be negative");
  require(value.fade_in + value.fade_out <= value.timeline_range.duration,
          CodecErrorCode::InvalidField, path, "clip fades cannot exceed the clip duration");
  output->set_audio_gain_db(value.audio_gain_db);
  output->set_audio_pan(value.audio_pan);
  encodeTime(value.fade_in, output->mutable_fade_in());
  encodeTime(value.fade_out, output->mutable_fade_out());
  for (std::size_t index = 0; index < value.effects.size(); ++index) {
    encodeEffect(value.effects[index], output->add_effects(), indexedPath(path, "effects", index),
                 ids);
  }
  encodeClipTitle(value, output, path);
  encodeClipNestedSequence(value, output, path);
  if (!value.enabled) {
    output->set_enabled(false);
  }
  if (value.label_color.has_value()) {
    encodeColor(*value.label_color, output->mutable_label_color(), childPath(path, "label_color"));
  }
}

void encodeTrack(const edit::Track& value, wire::Track* output, std::string_view path,
                 IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  output->set_kind(encodeTrackKind(value.kind));
  output->set_name(value.name);
  output->set_locked(value.locked);
  output->set_muted(value.muted);
  output->set_solo(value.solo);
  output->set_visible(value.visible);
  output->set_targeted(value.targeted);
  requireFinite(value.audio_gain_db, childPath(path, "audio_gain_db"));
  requireFinite(value.audio_pan, childPath(path, "audio_pan"));
  require(value.audio_gain_db >= kMinimumAudioGainDb && value.audio_gain_db <= kMaximumAudioGainDb,
          CodecErrorCode::InvalidField, childPath(path, "audio_gain_db"),
          "audio gain must be within [-96, 24] dB");
  require(value.audio_pan >= -1.0 && value.audio_pan <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "audio_pan"), "audio pan must be between minus one and one");
  output->set_audio_gain_db(value.audio_gain_db);
  output->set_audio_pan(value.audio_pan);
  const edit::Clip* previous = nullptr;
  for (std::size_t index = 0; index < value.clips.size(); ++index) {
    const auto clip_path = indexedPath(path, "clips", index);
    if (previous != nullptr) {
      require(previous->timeline_range.start <= value.clips[index].timeline_range.start,
              CodecErrorCode::InvalidProject, clip_path, "clips must be sorted by timeline start");
      require(!previous->timeline_range.overlaps(value.clips[index].timeline_range),
              CodecErrorCode::InvalidProject, clip_path, "clips on one track cannot overlap");
    }
    encodeClip(value.clips[index], output->add_clips(), clip_path, ids);
    previous = &value.clips[index];
  }
  for (std::size_t index = 0; index < value.effects.size(); ++index) {
    encodeEffect(value.effects[index], output->add_effects(), indexedPath(path, "effects", index),
                 ids);
  }
}

void encodeMarker(const edit::Marker& value, wire::Marker* output, std::string_view path,
                  IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.range.start.isNegative(), CodecErrorCode::InvalidField,
          childPath(path, "range.start"), "marker start cannot be negative");
  encodeRange(value.range, output->mutable_range());
  output->set_label(value.label);
  encodeColor(value.color, output->mutable_color(), childPath(path, "color"));
}

void encodeCaptionStyle(const edit::CaptionStyle& value, wire::CaptionStyle* output,
                        std::string_view path) {
  require(!value.font_family.empty(), CodecErrorCode::InvalidField, childPath(path, "font_family"),
          "caption font family cannot be empty");
  requireFinite(value.font_size, childPath(path, "font_size"));
  require(value.font_size > 0.0, CodecErrorCode::InvalidField, childPath(path, "font_size"),
          "caption font size must be positive");
  requireFinite(value.vertical_position, childPath(path, "vertical_position"));
  requireFinite(value.safe_margin, childPath(path, "safe_margin"));
  requireFinite(value.outline_width, childPath(path, "outline_width"));
  require(value.vertical_position >= 0.0 && value.vertical_position <= 1.0 &&
              value.safe_margin >= 0.0 && value.safe_margin <= 0.5 && value.outline_width >= 0.0 &&
              value.outline_width <= 128.0,
          CodecErrorCode::InvalidField, path, "caption style geometry is outside supported bounds");
  output->set_font_family(value.font_family);
  output->set_font_size(value.font_size);
  encodeColor(value.text_color, output->mutable_text_color(), childPath(path, "text_color"));
  encodeColor(value.background_color, output->mutable_background_color(),
              childPath(path, "background_color"));
  output->set_bold(value.bold);
  output->set_italic(value.italic);
  switch (value.alignment) {
  case edit::CaptionAlignment::Left:
    output->set_alignment(wire::CAPTION_ALIGNMENT_LEFT);
    break;
  case edit::CaptionAlignment::Center:
    output->set_alignment(wire::CAPTION_ALIGNMENT_CENTER);
    break;
  case edit::CaptionAlignment::Right:
    output->set_alignment(wire::CAPTION_ALIGNMENT_RIGHT);
    break;
  default:
    fail(CodecErrorCode::InvalidField, childPath(path, "alignment"), "unknown caption alignment");
  }
  output->set_vertical_position(value.vertical_position);
  output->set_safe_margin(value.safe_margin);
  output->set_outline_width(value.outline_width);
  encodeColor(value.outline_color, output->mutable_outline_color(),
              childPath(path, "outline_color"));
}

void encodeCaptionWord(const edit::CaptionWord& value, wire::CaptionWord* output,
                       std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.text.empty(), CodecErrorCode::InvalidField, childPath(path, "text"),
          "caption word text cannot be empty");
  require(validUtf8(value.text), CodecErrorCode::InvalidField, childPath(path, "text"),
          "caption word text must be valid UTF-8");
  requireFinite(value.probability, childPath(path, "probability"));
  require(value.probability >= 0.0 && value.probability <= 1.0 && !value.range.start.isNegative() &&
              value.range.duration > edit::Time{},
          CodecErrorCode::InvalidField, path, "caption word range/probability is invalid");
  encodeRange(value.range, output->mutable_range());
  output->set_text(value.text);
  output->set_probability(value.probability);
}

void encodeCaption(const edit::Caption& value, wire::Caption* output, std::string_view path,
                   IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(!value.range.start.isNegative() && value.range.duration > edit::Time{},
          CodecErrorCode::InvalidField, childPath(path, "range"),
          "caption range requires a non-negative start and positive duration");
  encodeRange(value.range, output->mutable_range());
  output->set_text(value.text);
  output->set_language(value.language);
  encodeCaptionStyle(value.style, output->mutable_style(), childPath(path, "style"));
  switch (value.provenance.source) {
  case edit::CaptionWordSource::Unknown:
    output->mutable_provenance()->set_source(wire::CAPTION_WORD_SOURCE_UNSPECIFIED);
    break;
  case edit::CaptionWordSource::Imported:
    output->mutable_provenance()->set_source(wire::CAPTION_WORD_SOURCE_IMPORTED);
    break;
  case edit::CaptionWordSource::LocalTranscription:
    output->mutable_provenance()->set_source(wire::CAPTION_WORD_SOURCE_LOCAL_TRANSCRIPTION);
    break;
  case edit::CaptionWordSource::UserEdited:
    output->mutable_provenance()->set_source(wire::CAPTION_WORD_SOURCE_USER_EDITED);
    break;
  default:
    fail(CodecErrorCode::InvalidField, childPath(path, "provenance.source"), "unknown word source");
  }
  output->mutable_provenance()->set_model_identity(value.provenance.model_identity);
  for (std::size_t index = 0; index < value.words.size(); ++index) {
    encodeCaptionWord(value.words[index], output->add_words(), indexedPath(path, "words", index),
                      ids);
  }
}

void encodeSequence(const edit::Sequence& value, wire::Sequence* output, std::string_view path,
                    IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  require(value.width > 0 && value.height > 0 && value.audio_sample_rate > 0,
          CodecErrorCode::InvalidField, path,
          "sequence dimensions and audio sample rate must be non-zero");
  output->set_name(value.name);
  encodeRate(value.frame_rate, output->mutable_frame_rate());
  output->set_width(value.width);
  output->set_height(value.height);
  output->set_audio_sample_rate(value.audio_sample_rate);
  for (std::size_t index = 0; index < value.tracks.size(); ++index) {
    encodeTrack(value.tracks[index], output->add_tracks(), indexedPath(path, "tracks", index), ids);
  }
  for (std::size_t index = 0; index < value.markers.size(); ++index) {
    encodeMarker(value.markers[index], output->add_markers(), indexedPath(path, "markers", index),
                 ids);
  }
  for (std::size_t index = 0; index < value.captions.size(); ++index) {
    encodeCaption(value.captions[index], output->add_captions(),
                  indexedPath(path, "captions", index), ids);
  }
  encodeTransitions(value, output, path, ids);
  if (value.start_time.value() != 0) {
    encodeTime(value.start_time, output->mutable_start_time());
  }
}

[[nodiscard]] wire::MediaBinKind encodeMediaBinKind(const edit::MediaBinKind value,
                                                    std::string_view path) {
  switch (value) {
  case edit::MediaBinKind::Folder:
    return wire::MEDIA_BIN_KIND_FOLDER;
  case edit::MediaBinKind::Smart:
    return wire::MEDIA_BIN_KIND_SMART;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "unknown media bin kind");
}

void encodeSmartQuery(const edit::SmartQuery& value, wire::SmartQuery* output) {
  for (const auto& tag : value.tags) {
    output->add_tags(tag);
  }
  if (value.min_rating.has_value()) {
    output->set_min_rating(*value.min_rating);
  }
  if (value.notes_contains.has_value()) {
    output->set_notes_contains(*value.notes_contains);
  }
  if (value.has_video.has_value()) {
    output->set_has_video(*value.has_video);
  }
  if (value.has_audio.has_value()) {
    output->set_has_audio(*value.has_audio);
  }
  if (value.name_contains.has_value()) {
    output->set_name_contains(*value.name_contains);
  }
  if (value.scene_equals.has_value()) {
    output->set_scene_equals(*value.scene_equals);
  }
  if (value.shot_equals.has_value()) {
    output->set_shot_equals(*value.shot_equals);
  }
  if (value.take_equals.has_value()) {
    output->set_take_equals(*value.take_equals);
  }
  if (value.preferred_take_only.has_value()) {
    output->set_preferred_take_only(*value.preferred_take_only);
  }
}

void encodeSubclip(const edit::Subclip& value, wire::Subclip* output, std::string_view path,
                   IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.source_asset_id, output->mutable_source_asset_id(),
           childPath(path, "source_asset_id"));
  encodeRange(value.source_range, output->mutable_source_range());
  output->set_name(value.name);
  if (!value.notes.empty()) {
    output->set_notes(value.notes);
  }
}

void encodeSavedMediaView(const edit::SavedMediaView& value, wire::SavedMediaView* output,
                          std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  output->set_name(value.name);
  for (const auto& column : value.visible_columns) {
    output->add_visible_columns(column);
  }
  encodeSmartQuery(value.search, output->mutable_search());
}

void encodeBin(const edit::MediaBin& value, wire::MediaBin* output, std::string_view path,
               IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  output->set_name(value.name);
  if (value.parent_id) {
    encodeId(*value.parent_id, output->mutable_parent_id(), childPath(path, "parent_id"));
  }
  output->set_kind(encodeMediaBinKind(value.kind, childPath(path, "kind")));
  encodeSmartQuery(value.query, output->mutable_query());
}

void encodeMulticamAngle(const edit::MulticamAngle& value, wire::MulticamAngle* output,
                         std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.clip_id, output->mutable_clip_id(), childPath(path, "clip_id"));
  encodeTime(value.sync_offset, output->mutable_sync_offset());
  output->set_label(value.label);
}

void encodeMulticamSwitch(const edit::MulticamSwitch& value, wire::MulticamSwitch* output,
                          std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeTime(value.time, output->mutable_time());
  encodeId(value.angle_id, output->mutable_angle_id(), childPath(path, "angle_id"));
}

void encodeMulticamGroup(const edit::MulticamGroup& value, wire::MulticamGroup* output,
                         std::string_view path, IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.sequence_id, output->mutable_sequence_id(), childPath(path, "sequence_id"));
  output->set_name(value.name);
  for (std::size_t index = 0; index < value.angles.size(); ++index) {
    encodeMulticamAngle(value.angles[index], output->add_angles(),
                        indexedPath(path, "angles", index), ids);
  }
  for (std::size_t index = 0; index < value.switches.size(); ++index) {
    encodeMulticamSwitch(value.switches[index], output->add_switches(),
                         indexedPath(path, "switches", index), ids);
  }
  encodeId(value.active_angle_id, output->mutable_active_angle_id(),
           childPath(path, "active_angle_id"));
  encodeId(value.audio_master_angle_id, output->mutable_audio_master_angle_id(),
           childPath(path, "audio_master_angle_id"));
  encodeTime(value.sync_reference, output->mutable_sync_reference());
}

void encodeProject(const edit::Project& value, wire::Project* output) {
  IdRegistry ids;
  encodeId(value.id, output->mutable_id(), "project.id", &ids);
  output->set_name(value.name);
  for (std::size_t index = 0; index < value.assets.size(); ++index) {
    encodeAsset(value.assets[index], output->add_assets(), indexedPath("project", "assets", index),
                ids);
  }
  for (std::size_t index = 0; index < value.sequences.size(); ++index) {
    encodeSequence(value.sequences[index], output->add_sequences(),
                   indexedPath("project", "sequences", index), ids);
  }
  for (std::size_t index = 0; index < value.bins.size(); ++index) {
    encodeBin(value.bins[index], output->add_bins(), indexedPath("project", "bins", index), ids);
  }
  for (std::size_t index = 0; index < value.multicam_groups.size(); ++index) {
    encodeMulticamGroup(value.multicam_groups[index], output->add_multicam_groups(),
                        indexedPath("project", "multicam_groups", index), ids);
  }
  for (std::size_t index = 0; index < value.subclips.size(); ++index) {
    encodeSubclip(value.subclips[index], output->add_subclips(),
                  indexedPath("project", "subclips", index), ids);
  }
  for (std::size_t index = 0; index < value.saved_media_views.size(); ++index) {
    encodeSavedMediaView(value.saved_media_views[index], output->add_saved_media_views(),
                         indexedPath("project", "saved_media_views", index), ids);
  }
  if (value.active_media_view_id) {
    encodeId(*value.active_media_view_id, output->mutable_active_media_view_id(),
             "project.active_media_view_id");
  }
  encodeMetadata(value.metadata, output->mutable_metadata(), "project.metadata");
}

[[nodiscard]] edit::TitleHorizontalAlignment decodeTitleAlignment(const wire::TitleAlignment value,
                                                                  std::string_view path) {
  switch (value) {
  case wire::TITLE_ALIGNMENT_LEFT:
    return edit::TitleHorizontalAlignment::Left;
  case wire::TITLE_ALIGNMENT_CENTER:
    return edit::TitleHorizontalAlignment::Center;
  case wire::TITLE_ALIGNMENT_RIGHT:
    return edit::TitleHorizontalAlignment::Right;
  case wire::TITLE_ALIGNMENT_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path),
       "title alignment is unspecified or unknown");
}

[[nodiscard]] edit::Title decodeTitle(const wire::Title& value, std::string_view path) {
  requirePresent(value.has_text_color(), childPath(path, "text_color"));
  requirePresent(value.has_background_color(), childPath(path, "background_color"));
  require(value.text().size() <= kMaximumTitleTextBytes, CodecErrorCode::InvalidField,
          childPath(path, "text"), "title text exceeds the byte limit");
  require(!value.font_family().empty(), CodecErrorCode::InvalidField,
          childPath(path, "font_family"), "title font family cannot be empty");
  require(value.font_family().size() <= kMaximumTitleFontFamilyBytes, CodecErrorCode::InvalidField,
          childPath(path, "font_family"), "title font family exceeds the byte limit");
  requireFinite(value.font_size(), childPath(path, "font_size"));
  require(value.font_size() >= kMinimumTitleFontSize && value.font_size() <= kMaximumTitleFontSize,
          CodecErrorCode::InvalidField, childPath(path, "font_size"),
          "title font size must be within the supported bounds");
  edit::Title result;
  result.text = value.text();
  result.font_family = value.font_family();
  result.font_size = value.font_size();
  result.foreground_color = decodeColor(value.text_color(), childPath(path, "text_color"));
  result.background_color =
      decodeColor(value.background_color(), childPath(path, "background_color"));
  result.horizontal_alignment =
      decodeTitleAlignment(value.alignment(), childPath(path, "alignment"));
  result.bold = value.bold();
  result.italic = value.italic();
  return result;
}

void assignDecodedTitle(const wire::Clip& value, const std::uint32_t declared_schema_version,
                        std::string_view path, edit::Clip& result) {
  if (result.kind == edit::ClipKind::Title) {
    if (declared_schema_version == 1) {
      edit::Title upgraded;
      upgraded.text = result.name;
      result.title = std::move(upgraded);
    } else {
      requirePresent(value.has_title(), childPath(path, "title"));
      result.title = decodeTitle(value.title(), childPath(path, "title"));
    }
  } else {
    require(!value.has_title(), CodecErrorCode::InvalidField, childPath(path, "title"),
            "media clips cannot carry a title payload");
    result.title.reset();
  }
}

void assignDecodedNestedSequence(const wire::Clip& value, std::string_view path, edit::Clip& result) {
  if (result.kind == edit::ClipKind::NestedSequence) {
    requirePresent(value.has_nested_sequence_id(), childPath(path, "nested_sequence_id"));
    result.nested_sequence_id =
        decodeId(value.nested_sequence_id(), childPath(path, "nested_sequence_id"));
  } else {
    require(!value.has_nested_sequence_id(), CodecErrorCode::InvalidField,
            childPath(path, "nested_sequence_id"),
            "only nested sequence clips may reference a nested sequence");
    result.nested_sequence_id.reset();
  }
}

[[nodiscard]] edit::TransitionKind decodeTransitionKind(const wire::TransitionKind value,
                                                        std::string_view path) {
  switch (value) {
  case wire::TRANSITION_KIND_CROSS_DISSOLVE:
    return edit::TransitionKind::CrossDissolve;
  case wire::TRANSITION_KIND_DIP_TO_BLACK:
    return edit::TransitionKind::DipToBlack;
  case wire::TRANSITION_KIND_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path),
       "transition kind is unspecified or unknown");
}

[[nodiscard]] edit::Transition decodeTransition(const wire::Transition& value,
                                                std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_outgoing_clip_id(), childPath(path, "outgoing_clip_id"));
  requirePresent(value.has_incoming_clip_id(), childPath(path, "incoming_clip_id"));
  requirePresent(value.has_timeline_range(), childPath(path, "timeline_range"));
  edit::Transition result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.outgoing_clip_id = decodeId(value.outgoing_clip_id(), childPath(path, "outgoing_clip_id"));
  result.incoming_clip_id = decodeId(value.incoming_clip_id(), childPath(path, "incoming_clip_id"));
  result.range = decodeRange(value.timeline_range(), childPath(path, "timeline_range"));
  result.kind = decodeTransitionKind(value.kind(), childPath(path, "kind"));
  result.enabled = value.enabled();
  return result;
}

void assignDecodedTransitions(const wire::Sequence& value, std::string_view path,
                              edit::Sequence& result, IdRegistry& ids) {
  std::size_t index = 0;
  for (const auto& transition : value.transitions()) {
    result.transitions.push_back(
        decodeTransition(transition, indexedPath(path, "transitions", index++), ids));
  }
}

void reject_v2_fields_in_declared_v1(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() != 1) {
    return;
  }

  std::size_t sequence_index = 0;
  for (const auto& sequence : snapshot.project().sequences()) {
    if (!sequence.transitions().empty()) {
      fail(CodecErrorCode::InvalidField,
           indexedPath(indexedPath("project", "sequences", sequence_index), "transitions", 0),
           "declared schema v1 snapshot cannot contain v2 transition fields");
    }

    std::size_t track_index = 0;
    for (const auto& track : sequence.tracks()) {
      if (track.has_visible()) {
        fail(CodecErrorCode::InvalidField,
             childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "tracks",
                                   track_index),
                       "visible"),
             "declared schema v1 snapshot cannot contain v2 track visibility fields");
      }
      if (track.has_targeted()) {
        fail(CodecErrorCode::InvalidField,
             childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "tracks",
                                   track_index),
                       "targeted"),
             "declared schema v1 snapshot cannot contain v2 track targeting fields");
      }
      if (track.has_audio_gain_db()) {
        fail(CodecErrorCode::InvalidField,
             childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "tracks",
                                   track_index),
                       "audio_gain_db"),
             "declared schema v1 snapshot cannot contain v2 track audio fields");
      }
      if (track.has_audio_pan()) {
        fail(CodecErrorCode::InvalidField,
             childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "tracks",
                                   track_index),
                       "audio_pan"),
             "declared schema v1 snapshot cannot contain v2 track audio fields");
      }
      std::size_t clip_index = 0;
      for (const auto& clip : track.clips()) {
        if (clip.has_title()) {
          fail(
              CodecErrorCode::InvalidField,
              childPath(indexedPath(indexedPath(indexedPath("project", "sequences", sequence_index),
                                                "tracks", track_index),
                                    "clips", clip_index),
                        "title"),
              "declared schema v1 snapshot cannot contain v2 title fields");
        }
        ++clip_index;
      }
      ++track_index;
    }
    ++sequence_index;
  }
}

void reject_v3_fields_in_declared_older(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() >= 3U) {
    return;
  }
  std::size_t sequence_index = 0;
  for (const auto& sequence : snapshot.project().sequences()) {
    std::size_t caption_index = 0;
    for (const auto& caption : sequence.captions()) {
      if (!caption.words().empty() || caption.has_provenance()) {
        fail(CodecErrorCode::InvalidField,
             childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "captions",
                                   caption_index),
                       "words"),
             "declared schema older than v3 cannot contain caption transcript fields");
      }
      const auto style_path =
          childPath(indexedPath(indexedPath("project", "sequences", sequence_index), "captions",
                                caption_index),
                    "style");
      if (caption.has_style() &&
          (caption.style().alignment() != wire::CAPTION_ALIGNMENT_UNSPECIFIED ||
           caption.style().has_outline_color() || caption.style().vertical_position() != 0.0 ||
           caption.style().safe_margin() != 0.0 || caption.style().outline_width() != 0.0)) {
        fail(CodecErrorCode::InvalidField, style_path,
             "declared schema older than v3 cannot contain caption style fields");
      }
      ++caption_index;
    }
    ++sequence_index;
  }
}

void reject_v4_fields_in_declared_older(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() >= 4U) {
    return;
  }
  if (!snapshot.project().bins().empty()) {
    fail(CodecErrorCode::InvalidField, indexedPath("project", "bins", 0),
         "declared schema older than v4 cannot contain media bins");
  }
  std::size_t asset_index = 0;
  for (const auto& asset : snapshot.project().assets()) {
    const auto asset_path = indexedPath("project", "assets", asset_index++);
    if (asset.has_bin_id()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "bin_id"),
           "declared schema older than v4 cannot contain asset bin fields");
    }
    if (asset.has_display_title()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "display_title"),
           "declared schema older than v4 cannot contain asset metadata fields");
    }
    if (!asset.tags().empty()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "tags"),
           "declared schema older than v4 cannot contain asset metadata fields");
    }
    if (asset.has_notes()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "notes"),
           "declared schema older than v4 cannot contain asset metadata fields");
    }
    if (asset.has_rating()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "rating"),
           "declared schema older than v4 cannot contain asset metadata fields");
    }
  }
  std::size_t sequence_index = 0;
  for (const auto& sequence : snapshot.project().sequences()) {
    std::size_t track_index = 0;
    for (const auto& track : sequence.tracks()) {
      std::size_t clip_index = 0;
      for (const auto& clip : track.clips()) {
        const auto clip_path = indexedPath(indexedPath(indexedPath("project", "sequences", sequence_index),
                                                      "tracks", track_index),
                                           "clips", clip_index);
        if (clip.kind() == wire::CLIP_KIND_NESTED_SEQUENCE) {
          fail(CodecErrorCode::InvalidField, childPath(clip_path, "kind"),
               "declared schema older than v4 cannot contain nested sequence clips");
        }
        if (clip.has_nested_sequence_id()) {
          fail(CodecErrorCode::InvalidField, childPath(clip_path, "nested_sequence_id"),
               "declared schema older than v4 cannot contain nested sequence fields");
        }
        ++clip_index;
      }
      ++track_index;
    }
    ++sequence_index;
  }
}

void reject_v5_fields_in_declared_older(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() >= 5U) {
    return;
  }
  std::size_t sequence_index = 0;
  for (const auto& sequence : snapshot.project().sequences()) {
    std::size_t track_index = 0;
    for (const auto& track : sequence.tracks()) {
      std::size_t clip_index = 0;
      for (const auto& clip : track.clips()) {
        const auto clip_path = indexedPath(indexedPath(indexedPath("project", "sequences", sequence_index),
                                                      "tracks", track_index),
                                           "clips", clip_index);
        if (clip.has_enabled() && !clip.enabled()) {
          fail(CodecErrorCode::InvalidField, childPath(clip_path, "enabled"),
               "declared schema older than v5 cannot contain clip enabled fields");
        }
        if (clip.has_label_color()) {
          fail(CodecErrorCode::InvalidField, childPath(clip_path, "label_color"),
               "declared schema older than v5 cannot contain clip label color fields");
        }
        ++clip_index;
      }
      ++track_index;
    }
    if (sequence.has_start_time()) {
      fail(CodecErrorCode::InvalidField,
           childPath(indexedPath("project", "sequences", sequence_index), "start_time"),
           "declared schema older than v5 cannot contain sequence start time fields");
    }
    ++sequence_index;
  }
}

void reject_v6_fields_in_declared_older(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() >= 6U) {
    return;
  }
  if (!snapshot.project().multicam_groups().empty()) {
    fail(CodecErrorCode::InvalidField, indexedPath("project", "multicam_groups", 0),
         "declared schema older than v6 cannot contain multicam groups");
  }
}

void reject_v7_fields_in_declared_older(const wire::ProjectSnapshot& snapshot) {
  if (snapshot.schema_version() >= 7U) {
    return;
  }
  std::size_t asset_index = 0;
  for (const auto& asset : snapshot.project().assets()) {
    const auto asset_path = indexedPath("project", "assets", asset_index++);
    if (asset.has_production_scene()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_scene"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_shot()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_shot"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_take()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_take"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_camera()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_camera"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_reel()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_reel"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_audio_roll()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_audio_roll"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_source_timecode()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_source_timecode"),
           "declared schema older than v7 cannot contain production metadata");
    }
    if (asset.has_production_preferred_take()) {
      fail(CodecErrorCode::InvalidField, childPath(asset_path, "production_preferred_take"),
           "declared schema older than v7 cannot contain production metadata");
    }
  }
  if (!snapshot.project().subclips().empty()) {
    fail(CodecErrorCode::InvalidField, indexedPath("project", "subclips", 0),
         "declared schema older than v7 cannot contain subclips");
  }
  if (!snapshot.project().saved_media_views().empty()) {
    fail(CodecErrorCode::InvalidField, indexedPath("project", "saved_media_views", 0),
         "declared schema older than v7 cannot contain saved media views");
  }
  if (snapshot.project().has_active_media_view_id()) {
    fail(CodecErrorCode::InvalidField, "project.active_media_view_id",
         "declared schema older than v7 cannot contain an active media view");
  }
  std::size_t bin_index = 0;
  for (const auto& bin : snapshot.project().bins()) {
    if (!bin.has_query()) {
      ++bin_index;
      continue;
    }
    const auto& query = bin.query();
    const auto query_path =
        childPath(indexedPath("project", "bins", bin_index++), "query");
    if (query.has_scene_equals()) {
      fail(CodecErrorCode::InvalidField, childPath(query_path, "scene_equals"),
           "declared schema older than v7 cannot contain production smart queries");
    }
    if (query.has_shot_equals()) {
      fail(CodecErrorCode::InvalidField, childPath(query_path, "shot_equals"),
           "declared schema older than v7 cannot contain production smart queries");
    }
    if (query.has_take_equals()) {
      fail(CodecErrorCode::InvalidField, childPath(query_path, "take_equals"),
           "declared schema older than v7 cannot contain production smart queries");
    }
    if (query.has_preferred_take_only()) {
      fail(CodecErrorCode::InvalidField, childPath(query_path, "preferred_take_only"),
           "declared schema older than v7 cannot contain production smart queries");
    }
  }
}

[[nodiscard]] std::optional<std::string> findUnknownField(const google::protobuf::Message& message,
                                                          std::string_view path) {
  const auto* reflection = message.GetReflection();
  const auto& unknown = reflection->GetUnknownFields(message);
  if (unknown.field_count() != 0) {
    return std::string(path);
  }

  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields) {
    if (field->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      continue;
    }
    const auto field_path = childPath(path, field->name());
    if (field->is_repeated()) {
      const auto size = reflection->FieldSize(message, field);
      for (int index = 0; index < size; ++index) {
        if (auto nested = findUnknownField(reflection->GetRepeatedMessage(message, field, index),
                                           field_path + "[" + std::to_string(index) + "]")) {
          return nested;
        }
      }
    } else if (reflection->HasField(message, field)) {
      if (auto nested = findUnknownField(reflection->GetMessage(message, field), field_path)) {
        return nested;
      }
    }
  }
  return std::nullopt;
}

void requirePresent(bool present, std::string_view path) {
  require(present, CodecErrorCode::MissingField, path,
          "required protobuf message field is missing");
}

[[nodiscard]] edit::EntityId decodeId(const wire::EntityId& value, std::string_view path,
                                      IdRegistry* registry, bool allow_nil) {
  require(value.value().size() == 16, CodecErrorCode::InvalidField, path,
          "entity id must contain exactly 16 bytes");
  std::array<std::uint8_t, 16> bytes{};
  std::memcpy(bytes.data(), value.value().data(), bytes.size());
  const edit::EntityId result(bytes);
  require(allow_nil || !result.isNil(), CodecErrorCode::InvalidField, path,
          "entity id cannot be nil");
  if (registry != nullptr) {
    registry->add(result, path);
  }
  return result;
}

[[nodiscard]] edit::Time decodeTime(const wire::Time& value, std::string_view path) {
  require(value.timescale() != 0, CodecErrorCode::InvalidField, childPath(path, "timescale"),
          "time timescale must be non-zero");
  return edit::Time(value.value(), value.timescale());
}

[[nodiscard]] edit::TimeRange decodeRange(const wire::TimeRange& value, std::string_view path) {
  requirePresent(value.has_start(), childPath(path, "start"));
  requirePresent(value.has_duration(), childPath(path, "duration"));
  const auto start = decodeTime(value.start(), childPath(path, "start"));
  const auto duration = decodeTime(value.duration(), childPath(path, "duration"));
  require(!duration.isNegative(), CodecErrorCode::InvalidField, childPath(path, "duration"),
          "time range duration cannot be negative");
  return edit::TimeRange(start, duration);
}

[[nodiscard]] edit::Rate decodeRate(const wire::Rate& value, std::string_view path) {
  require(value.numerator() != 0 && value.denominator() != 0, CodecErrorCode::InvalidField, path,
          "rate numerator and denominator must be non-zero");
  return edit::Rate(value.numerator(), value.denominator());
}

[[nodiscard]] edit::Vec2 decodeVec2(const wire::Vec2& value, std::string_view path) {
  requireFinite(value.x(), childPath(path, "x"));
  requireFinite(value.y(), childPath(path, "y"));
  return edit::Vec2{value.x(), value.y()};
}

[[nodiscard]] edit::ColorRgba decodeColor(const wire::ColorRgba& value, std::string_view path) {
  requireFinite(value.red(), childPath(path, "red"));
  requireFinite(value.green(), childPath(path, "green"));
  requireFinite(value.blue(), childPath(path, "blue"));
  requireFinite(value.alpha(), childPath(path, "alpha"));
  return edit::ColorRgba{value.red(), value.green(), value.blue(), value.alpha()};
}

[[nodiscard]] std::map<std::string, std::string, std::less<>>
decodeMetadata(const google::protobuf::RepeatedPtrField<wire::StringEntry>& entries,
               std::string_view path) {
  std::map<std::string, std::string, std::less<>> result;
  std::size_t index = 0;
  for (const auto& entry : entries) {
    const auto entry_path = indexedPath(path, "entries", index++);
    require(!entry.key().empty(), CodecErrorCode::InvalidField, childPath(entry_path, "key"),
            "metadata key cannot be empty");
    if (!result.emplace(entry.key(), entry.value()).second) {
      fail(CodecErrorCode::InvalidField, childPath(entry_path, "key"),
           "metadata key is duplicated");
    }
  }
  return result;
}

[[nodiscard]] edit::KeyframeInterpolation decodeInterpolation(wire::KeyframeInterpolation value,
                                                              std::string_view path) {
  switch (value) {
  case wire::KEYFRAME_INTERPOLATION_HOLD:
    return edit::KeyframeInterpolation::Hold;
  case wire::KEYFRAME_INTERPOLATION_LINEAR:
    return edit::KeyframeInterpolation::Linear;
  case wire::KEYFRAME_INTERPOLATION_BEZIER:
    return edit::KeyframeInterpolation::Bezier;
  case wire::KEYFRAME_INTERPOLATION_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path),
       "keyframe interpolation is unspecified or unknown");
}

[[nodiscard]] edit::EffectValue decodeEffectValue(const wire::EffectValue& value,
                                                  std::string_view path) {
  switch (value.value_case()) {
  case wire::EffectValue::kIntegerValue:
    return static_cast<std::int64_t>(value.integer_value());
  case wire::EffectValue::kDoubleValue:
    requireFinite(value.double_value(), path);
    return value.double_value();
  case wire::EffectValue::kBooleanValue:
    return value.boolean_value();
  case wire::EffectValue::kStringValue:
    return value.string_value();
  case wire::EffectValue::kTimeValue:
    return decodeTime(value.time_value(), path);
  case wire::EffectValue::kVec2Value:
    return decodeVec2(value.vec2_value(), path);
  case wire::EffectValue::kColorValue:
    return decodeColor(value.color_value(), path);
  case wire::EffectValue::VALUE_NOT_SET:
    break;
  }
  fail(CodecErrorCode::MissingField, std::string(path), "effect value oneof is not set");
}

[[nodiscard]] edit::Keyframe decodeKeyframe(const wire::Keyframe& value, std::string_view path,
                                            IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_time(), childPath(path, "time"));
  requirePresent(value.has_value(), childPath(path, "value"));
  requirePresent(value.has_incoming_control(), childPath(path, "incoming_control"));
  requirePresent(value.has_outgoing_control(), childPath(path, "outgoing_control"));
  edit::Keyframe result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.time = decodeTime(value.time(), childPath(path, "time"));
  require(!result.time.isNegative(), CodecErrorCode::InvalidField, childPath(path, "time"),
          "keyframe time cannot be negative");
  result.value = decodeEffectValue(value.value(), childPath(path, "value"));
  result.interpolation =
      decodeInterpolation(value.interpolation(), childPath(path, "interpolation"));
  result.incoming_control =
      decodeVec2(value.incoming_control(), childPath(path, "incoming_control"));
  result.outgoing_control =
      decodeVec2(value.outgoing_control(), childPath(path, "outgoing_control"));
  return result;
}

[[nodiscard]] edit::EffectParameter decodeParameter(const wire::EffectParameter& value,
                                                    std::string_view path, IdRegistry& ids) {
  require(!value.id().empty(), CodecErrorCode::InvalidField, childPath(path, "id"),
          "effect parameter id cannot be empty");
  requirePresent(value.has_value(), childPath(path, "value"));
  edit::EffectParameter result;
  result.id = value.id();
  result.value = decodeEffectValue(value.value(), childPath(path, "value"));
  std::optional<edit::Time> previous_time;
  std::size_t index = 0;
  for (const auto& keyframe : value.keyframes()) {
    const auto keyframe_path = indexedPath(path, "keyframes", index++);
    auto decoded = decodeKeyframe(keyframe, keyframe_path, ids);
    if (previous_time) {
      require(decoded.time > *previous_time, CodecErrorCode::InvalidField,
              childPath(keyframe_path, "time"), "keyframes must be strictly ordered by time");
    }
    previous_time = decoded.time;
    result.keyframes.push_back(std::move(decoded));
  }
  return result;
}

[[nodiscard]] edit::Effect decodeEffect(const wire::Effect& value, std::string_view path,
                                        IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  require(!value.type().empty(), CodecErrorCode::InvalidField, childPath(path, "type"),
          "effect type cannot be empty");
  require(value.version() > 0, CodecErrorCode::InvalidField, childPath(path, "version"),
          "effect version must be non-zero");
  edit::Effect result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.type = value.type();
  result.version = value.version();
  result.enabled = value.enabled();
  result.known = value.known();
  std::size_t index = 0;
  for (const auto& parameter : value.parameters()) {
    const auto parameter_path = indexedPath(path, "parameters", index++);
    auto decoded = decodeParameter(parameter, parameter_path, ids);
    if (!result.parameters.emplace(decoded.id, std::move(decoded)).second) {
      fail(CodecErrorCode::InvalidField, childPath(parameter_path, "id"),
           "effect parameter id is duplicated");
    }
  }
  const auto& payload = value.opaque_payload();
  result.opaque_payload.resize(payload.size());
  if (!payload.empty()) {
    std::memcpy(result.opaque_payload.data(), payload.data(), payload.size());
  }
  return result;
}

[[nodiscard]] edit::Transform decodeTransform(const wire::Transform& value, std::string_view path) {
  requirePresent(value.has_position(), childPath(path, "position"));
  requirePresent(value.has_scale(), childPath(path, "scale"));
  edit::Transform result;
  result.position = decodeVec2(value.position(), childPath(path, "position"));
  result.scale = decodeVec2(value.scale(), childPath(path, "scale"));
  requireFinite(value.rotation_degrees(), childPath(path, "rotation_degrees"));
  requireFinite(value.anchor_x(), childPath(path, "anchor_x"));
  requireFinite(value.anchor_y(), childPath(path, "anchor_y"));
  requireFinite(value.crop_left(), childPath(path, "crop_left"));
  requireFinite(value.crop_top(), childPath(path, "crop_top"));
  requireFinite(value.crop_right(), childPath(path, "crop_right"));
  requireFinite(value.crop_bottom(), childPath(path, "crop_bottom"));
  requireFinite(value.opacity(), childPath(path, "opacity"));
  require(value.opacity() >= 0.0 && value.opacity() <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "opacity"), "opacity must be between zero and one");
  require(value.crop_left() >= 0.0 && value.crop_left() <= 1.0 && value.crop_top() >= 0.0 &&
              value.crop_top() <= 1.0 && value.crop_right() >= 0.0 && value.crop_right() <= 1.0 &&
              value.crop_bottom() >= 0.0 && value.crop_bottom() <= 1.0,
          CodecErrorCode::InvalidField, path, "crop values must be between zero and one");
  result.rotation_degrees = value.rotation_degrees();
  result.anchor_x = value.anchor_x();
  result.anchor_y = value.anchor_y();
  result.crop_left = value.crop_left();
  result.crop_top = value.crop_top();
  result.crop_right = value.crop_right();
  result.crop_bottom = value.crop_bottom();
  result.opacity = value.opacity();
  return result;
}

[[nodiscard]] edit::TrackKind decodeTrackKind(wire::TrackKind value, std::string_view path) {
  switch (value) {
  case wire::TRACK_KIND_VIDEO:
    return edit::TrackKind::Video;
  case wire::TRACK_KIND_AUDIO:
    return edit::TrackKind::Audio;
  case wire::TRACK_KIND_CAPTION:
    return edit::TrackKind::Caption;
  case wire::TRACK_KIND_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "track kind is unspecified or unknown");
}

[[nodiscard]] edit::ClipKind decodeClipKind(wire::ClipKind value, std::string_view path) {
  switch (value) {
  case wire::CLIP_KIND_VIDEO:
    return edit::ClipKind::Video;
  case wire::CLIP_KIND_AUDIO:
    return edit::ClipKind::Audio;
  case wire::CLIP_KIND_TITLE:
    return edit::ClipKind::Title;
  case wire::CLIP_KIND_NESTED_SEQUENCE:
    return edit::ClipKind::NestedSequence;
  case wire::CLIP_KIND_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "clip kind is unspecified or unknown");
}

[[nodiscard]] edit::BlendMode decodeBlendMode(wire::BlendMode value, std::string_view path) {
  switch (value) {
  case wire::BLEND_MODE_NORMAL:
    return edit::BlendMode::Normal;
  case wire::BLEND_MODE_ADD:
    return edit::BlendMode::Add;
  case wire::BLEND_MODE_MULTIPLY:
    return edit::BlendMode::Multiply;
  case wire::BLEND_MODE_SCREEN:
    return edit::BlendMode::Screen;
  case wire::BLEND_MODE_OVERLAY:
    return edit::BlendMode::Overlay;
  case wire::BLEND_MODE_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "blend mode is unspecified or unknown");
}

[[nodiscard]] edit::Asset decodeAsset(const wire::Asset& value, std::string_view path,
                                      IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_duration(), childPath(path, "duration"));
  edit::Asset result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.name = value.name();
  result.source_uri = value.source_uri();
  result.fingerprint = value.fingerprint();
  result.duration = decodeTime(value.duration(), childPath(path, "duration"));
  require(!result.duration.isNegative(), CodecErrorCode::InvalidField, childPath(path, "duration"),
          "asset duration cannot be negative");
  result.has_video = value.has_video();
  result.has_audio = value.has_audio();
  result.width = value.width();
  result.height = value.height();
  if (result.has_video) {
    require(result.width > 0 && result.height > 0, CodecErrorCode::InvalidField, path,
            "video assets require non-zero dimensions");
  }
  if (value.has_nominal_frame_rate()) {
    result.nominal_frame_rate =
        decodeRate(value.nominal_frame_rate(), childPath(path, "nominal_frame_rate"));
  }
  result.audio_sample_rate = value.audio_sample_rate();
  result.audio_channels = value.audio_channels();
  if (result.has_audio) {
    require(result.audio_sample_rate > 0 && result.audio_channels > 0, CodecErrorCode::InvalidField,
            path, "audio assets require a sample rate and channel count");
  }
  result.metadata = decodeMetadata(value.metadata(), childPath(path, "metadata"));
  if (value.has_bin_id()) {
    result.bin_id = decodeId(value.bin_id(), childPath(path, "bin_id"));
  }
  if (value.has_display_title()) {
    result.display_title = value.display_title();
  }
  for (const auto& tag : value.tags()) {
    result.tags.push_back(tag);
  }
  if (value.has_notes()) {
    result.notes = value.notes();
  }
  if (value.has_rating()) {
    result.rating = value.rating();
  }
  if (value.has_production_scene()) {
    result.production.scene = value.production_scene();
  }
  if (value.has_production_shot()) {
    result.production.shot = value.production_shot();
  }
  if (value.has_production_take()) {
    result.production.take = value.production_take();
  }
  if (value.has_production_camera()) {
    result.production.camera = value.production_camera();
  }
  if (value.has_production_reel()) {
    result.production.reel = value.production_reel();
  }
  if (value.has_production_audio_roll()) {
    result.production.audio_roll = value.production_audio_roll();
  }
  if (value.has_production_source_timecode()) {
    result.production.source_timecode = value.production_source_timecode();
  }
  if (value.has_production_preferred_take()) {
    result.production.preferred_take = value.production_preferred_take();
  }
  for (const auto& channel : value.audio_channel_map()) {
    edit::AudioChannelDescriptor descriptor;
    descriptor.index = channel.index();
    descriptor.label = channel.label();
    result.audio_channel_map.push_back(descriptor);
  }
  if (value.has_monitor_left_channel()) {
    result.monitor_left_channel = value.monitor_left_channel();
  }
  if (value.has_monitor_right_channel()) {
    result.monitor_right_channel = value.monitor_right_channel();
  }
  return result;
}

[[nodiscard]] edit::Clip decodeClip(const wire::Clip& value, std::string_view path,
                                    const std::uint32_t declared_schema_version, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_asset_id(), childPath(path, "asset_id"));
  requirePresent(value.has_timeline_range(), childPath(path, "timeline_range"));
  requirePresent(value.has_source_range(), childPath(path, "source_range"));
  requirePresent(value.has_playback_rate(), childPath(path, "playback_rate"));
  requirePresent(value.has_transform(), childPath(path, "transform"));
  requirePresent(value.has_fade_in(), childPath(path, "fade_in"));
  requirePresent(value.has_fade_out(), childPath(path, "fade_out"));
  edit::Clip result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.kind = decodeClipKind(value.kind(), childPath(path, "kind"));
  result.asset_id = decodeId(value.asset_id(), childPath(path, "asset_id"), nullptr,
                             result.kind == edit::ClipKind::Title ||
                                 result.kind == edit::ClipKind::NestedSequence);
  result.name = value.name();
  result.timeline_range = decodeRange(value.timeline_range(), childPath(path, "timeline_range"));
  result.source_range = decodeRange(value.source_range(), childPath(path, "source_range"));
  result.playback_rate = decodeRate(value.playback_rate(), childPath(path, "playback_rate"));
  result.reversed = value.reversed();
  if (value.has_linked_group()) {
    result.linked_group = decodeId(value.linked_group(), childPath(path, "linked_group"));
  }
  result.transform = decodeTransform(value.transform(), childPath(path, "transform"));
  result.blend_mode = decodeBlendMode(value.blend_mode(), childPath(path, "blend_mode"));
  requireFinite(value.audio_gain_db(), childPath(path, "audio_gain_db"));
  requireFinite(value.audio_pan(), childPath(path, "audio_pan"));
  require(value.audio_pan() >= -1.0 && value.audio_pan() <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "audio_pan"), "audio pan must be between minus one and one");
  result.audio_gain_db = value.audio_gain_db();
  result.audio_pan = value.audio_pan();
  result.fade_in = decodeTime(value.fade_in(), childPath(path, "fade_in"));
  result.fade_out = decodeTime(value.fade_out(), childPath(path, "fade_out"));
  require(!result.fade_in.isNegative() && !result.fade_out.isNegative(),
          CodecErrorCode::InvalidField, path, "clip fades cannot be negative");
  require(result.fade_in + result.fade_out <= result.timeline_range.duration,
          CodecErrorCode::InvalidField, path, "clip fades cannot exceed the clip duration");
  std::size_t index = 0;
  for (const auto& effect : value.effects()) {
    result.effects.push_back(decodeEffect(effect, indexedPath(path, "effects", index++), ids));
  }
  assignDecodedTitle(value, declared_schema_version, path, result);
  assignDecodedNestedSequence(value, path, result);
  result.enabled = !value.has_enabled() || value.enabled();
  if (value.has_label_color()) {
    result.label_color = decodeColor(value.label_color(), childPath(path, "label_color"));
  }
  return result;
}

[[nodiscard]] edit::Track decodeTrack(const wire::Track& value, std::string_view path,
                                      const std::uint32_t declared_schema_version,
                                      IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  edit::Track result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.kind = decodeTrackKind(value.kind(), childPath(path, "kind"));
  result.name = value.name();
  result.locked = value.locked();
  result.muted = value.muted();
  result.solo = value.solo();
  // The fields were added during schema v2. Absence in earlier v2 payloads
  // deliberately means the model defaults, rather than proto3's false.
  result.visible = !value.has_visible() || value.visible();
  result.targeted = !value.has_targeted() || value.targeted();
  result.audio_gain_db = value.has_audio_gain_db() ? value.audio_gain_db() : 0.0;
  result.audio_pan = value.has_audio_pan() ? value.audio_pan() : 0.0;
  requireFinite(result.audio_gain_db, childPath(path, "audio_gain_db"));
  requireFinite(result.audio_pan, childPath(path, "audio_pan"));
  require(result.audio_gain_db >= kMinimumAudioGainDb &&
              result.audio_gain_db <= kMaximumAudioGainDb,
          CodecErrorCode::InvalidField, childPath(path, "audio_gain_db"),
          "audio gain must be within [-96, 24] dB");
  require(result.audio_pan >= -1.0 && result.audio_pan <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "audio_pan"), "audio pan must be between minus one and one");
  const edit::Clip* previous = nullptr;
  std::size_t index = 0;
  for (const auto& clip : value.clips()) {
    const auto clip_path = indexedPath(path, "clips", index++);
    auto decoded = decodeClip(clip, clip_path, declared_schema_version, ids);
    if (previous != nullptr) {
      require(previous->timeline_range.start <= decoded.timeline_range.start,
              CodecErrorCode::InvalidProject, clip_path, "clips must be sorted by timeline start");
      require(!previous->timeline_range.overlaps(decoded.timeline_range),
              CodecErrorCode::InvalidProject, clip_path, "clips on one track cannot overlap");
    }
    result.clips.push_back(std::move(decoded));
    previous = &result.clips.back();
  }
  index = 0;
  for (const auto& effect : value.effects()) {
    result.effects.push_back(decodeEffect(effect, indexedPath(path, "effects", index++), ids));
  }
  return result;
}

[[nodiscard]] edit::Marker decodeMarker(const wire::Marker& value, std::string_view path,
                                        IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_range(), childPath(path, "range"));
  requirePresent(value.has_color(), childPath(path, "color"));
  edit::Marker result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.range = decodeRange(value.range(), childPath(path, "range"));
  require(!result.range.start.isNegative(), CodecErrorCode::InvalidField,
          childPath(path, "range.start"), "marker start cannot be negative");
  result.label = value.label();
  result.color = decodeColor(value.color(), childPath(path, "color"));
  return result;
}

[[nodiscard]] edit::CaptionStyle decodeCaptionStyle(const wire::CaptionStyle& value,
                                                    std::string_view path,
                                                    const std::uint32_t declared_schema_version) {
  requirePresent(value.has_text_color(), childPath(path, "text_color"));
  requirePresent(value.has_background_color(), childPath(path, "background_color"));
  require(!value.font_family().empty(), CodecErrorCode::InvalidField,
          childPath(path, "font_family"), "caption font family cannot be empty");
  requireFinite(value.font_size(), childPath(path, "font_size"));
  require(value.font_size() > 0.0, CodecErrorCode::InvalidField, childPath(path, "font_size"),
          "caption font size must be positive");
  edit::CaptionStyle result;
  result.font_family = value.font_family();
  result.font_size = value.font_size();
  result.text_color = decodeColor(value.text_color(), childPath(path, "text_color"));
  result.background_color =
      decodeColor(value.background_color(), childPath(path, "background_color"));
  result.bold = value.bold();
  result.italic = value.italic();
  switch (value.alignment()) {
  case wire::CAPTION_ALIGNMENT_LEFT:
    result.alignment = edit::CaptionAlignment::Left;
    break;
  case wire::CAPTION_ALIGNMENT_CENTER:
    result.alignment = edit::CaptionAlignment::Center;
    break;
  case wire::CAPTION_ALIGNMENT_RIGHT:
    result.alignment = edit::CaptionAlignment::Right;
    break;
  case wire::CAPTION_ALIGNMENT_UNSPECIFIED:
    result.alignment = edit::CaptionAlignment::Center;
    break;
  default:
    fail(CodecErrorCode::InvalidField, childPath(path, "alignment"), "unknown caption alignment");
  }
  if (value.has_vertical_position()) {
    result.vertical_position = value.vertical_position();
  }
  if (value.has_safe_margin()) {
    result.safe_margin = value.safe_margin();
  }
  if (value.has_outline_width()) {
    result.outline_width = value.outline_width();
  }
  requireFinite(result.vertical_position, childPath(path, "vertical_position"));
  requireFinite(result.safe_margin, childPath(path, "safe_margin"));
  requireFinite(result.outline_width, childPath(path, "outline_width"));
  require(result.vertical_position >= 0.0 && result.vertical_position <= 1.0 &&
              result.safe_margin >= 0.0 && result.safe_margin <= 0.5 &&
              result.outline_width >= 0.0 && result.outline_width <= 128.0,
          CodecErrorCode::InvalidField, path, "caption style geometry is outside supported bounds");
  if (value.has_outline_color()) {
    result.outline_color = decodeColor(value.outline_color(), childPath(path, "outline_color"));
  } else if (declared_schema_version >= 3U) {
    // Early v3 writers may omit optional style fields; model defaults remain canonical.
    result.outline_color = edit::ColorRgba{0.0, 0.0, 0.0, 1.0};
  }
  return result;
}

[[nodiscard]] edit::CaptionWord decodeCaptionWord(const wire::CaptionWord& value,
                                                  std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_range(), childPath(path, "range"));
  edit::CaptionWord result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.text = value.text();
  result.range = decodeRange(value.range(), childPath(path, "range"));
  result.probability = value.probability();
  require(!result.text.empty() && result.range.start.isNegative() == false &&
              result.range.duration > edit::Time{},
          CodecErrorCode::InvalidField, path, "caption word text/range is invalid");
  requireFinite(result.probability, childPath(path, "probability"));
  require(result.probability >= 0.0 && result.probability <= 1.0, CodecErrorCode::InvalidField,
          childPath(path, "probability"), "caption word probability must be in [0, 1]");
  return result;
}

[[nodiscard]] edit::Caption decodeCaption(const wire::Caption& value, std::string_view path,
                                          const std::uint32_t declared_schema_version,
                                          IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_range(), childPath(path, "range"));
  requirePresent(value.has_style(), childPath(path, "style"));
  edit::Caption result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.range = decodeRange(value.range(), childPath(path, "range"));
  require(!result.range.start.isNegative() && result.range.duration > edit::Time{},
          CodecErrorCode::InvalidField, childPath(path, "range"),
          "caption range requires a non-negative start and positive duration");
  result.text = value.text();
  result.language = value.language();
  result.style =
      decodeCaptionStyle(value.style(), childPath(path, "style"), declared_schema_version);
  if (value.has_provenance()) {
    result.provenance.model_identity = value.provenance().model_identity();
    require(validUtf8(result.provenance.model_identity), CodecErrorCode::InvalidField,
            childPath(path, "provenance.model_identity"),
            "caption provenance identity must be valid UTF-8");
    switch (value.provenance().source()) {
    case wire::CAPTION_WORD_SOURCE_IMPORTED:
      result.provenance.source = edit::CaptionWordSource::Imported;
      break;
    case wire::CAPTION_WORD_SOURCE_LOCAL_TRANSCRIPTION:
      result.provenance.source = edit::CaptionWordSource::LocalTranscription;
      break;
    case wire::CAPTION_WORD_SOURCE_USER_EDITED:
      result.provenance.source = edit::CaptionWordSource::UserEdited;
      break;
    case wire::CAPTION_WORD_SOURCE_UNSPECIFIED:
      result.provenance.source = edit::CaptionWordSource::Unknown;
      break;
    default:
      fail(CodecErrorCode::InvalidField, childPath(path, "provenance.source"),
           "unknown word source");
    }
  }
  std::optional<edit::Time> previous_end;
  std::size_t index = 0;
  for (const auto& word : value.words()) {
    auto decoded = decodeCaptionWord(word, indexedPath(path, "words", index++), ids);
    require(result.range.contains(decoded.range), CodecErrorCode::InvalidField,
            indexedPath(path, "words", index - 1), "caption word must be contained in caption");
    if (previous_end) {
      require(decoded.range.start >= *previous_end, CodecErrorCode::InvalidField,
              indexedPath(path, "words", index - 1),
              "caption words must be ordered and non-overlapping");
    }
    previous_end = decoded.range.end();
    result.words.push_back(std::move(decoded));
  }
  return result;
}

[[nodiscard]] edit::Sequence decodeSequence(const wire::Sequence& value, std::string_view path,
                                            const std::uint32_t declared_schema_version,
                                            IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_frame_rate(), childPath(path, "frame_rate"));
  require(value.width() > 0 && value.height() > 0 && value.audio_sample_rate() > 0,
          CodecErrorCode::InvalidField, path,
          "sequence dimensions and audio sample rate must be non-zero");
  edit::Sequence result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.name = value.name();
  result.frame_rate = decodeRate(value.frame_rate(), childPath(path, "frame_rate"));
  result.width = value.width();
  result.height = value.height();
  result.audio_sample_rate = value.audio_sample_rate();
  std::size_t index = 0;
  for (const auto& track : value.tracks()) {
    result.tracks.push_back(
        decodeTrack(track, indexedPath(path, "tracks", index++), declared_schema_version, ids));
  }
  index = 0;
  for (const auto& marker : value.markers()) {
    result.markers.push_back(decodeMarker(marker, indexedPath(path, "markers", index++), ids));
  }
  index = 0;
  for (const auto& caption : value.captions()) {
    result.captions.push_back(decodeCaption(caption, indexedPath(path, "captions", index++),
                                            declared_schema_version, ids));
  }
  assignDecodedTransitions(value, path, result, ids);
  if (value.has_start_time()) {
    result.start_time = decodeTime(value.start_time(), childPath(path, "start_time"));
  }
  return result;
}

[[nodiscard]] edit::MediaBinKind decodeMediaBinKind(const wire::MediaBinKind value,
                                                    std::string_view path) {
  switch (value) {
  case wire::MEDIA_BIN_KIND_FOLDER:
    return edit::MediaBinKind::Folder;
  case wire::MEDIA_BIN_KIND_SMART:
    return edit::MediaBinKind::Smart;
  case wire::MEDIA_BIN_KIND_UNSPECIFIED:
    break;
  default:
    break;
  }
  fail(CodecErrorCode::InvalidField, std::string(path), "media bin kind is unspecified or unknown");
}

[[nodiscard]] edit::SmartQuery decodeSmartQuery(const wire::SmartQuery& value) {
  edit::SmartQuery result;
  for (const auto& tag : value.tags()) {
    result.tags.push_back(tag);
  }
  if (value.has_min_rating()) {
    result.min_rating = value.min_rating();
  }
  if (value.has_notes_contains()) {
    result.notes_contains = value.notes_contains();
  }
  if (value.has_has_video()) {
    result.has_video = value.has_video();
  }
  if (value.has_has_audio()) {
    result.has_audio = value.has_audio();
  }
  if (value.has_name_contains()) {
    result.name_contains = value.name_contains();
  }
  if (value.has_scene_equals()) {
    result.scene_equals = value.scene_equals();
  }
  if (value.has_shot_equals()) {
    result.shot_equals = value.shot_equals();
  }
  if (value.has_take_equals()) {
    result.take_equals = value.take_equals();
  }
  if (value.has_preferred_take_only()) {
    result.preferred_take_only = value.preferred_take_only();
  }
  return result;
}

[[nodiscard]] edit::Subclip decodeSubclip(const wire::Subclip& value, std::string_view path,
                                          IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_source_asset_id(), childPath(path, "source_asset_id"));
  requirePresent(value.has_source_range(), childPath(path, "source_range"));
  edit::Subclip result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.source_asset_id =
      decodeId(value.source_asset_id(), childPath(path, "source_asset_id"));
  result.source_range = decodeRange(value.source_range(), childPath(path, "source_range"));
  result.name = value.name();
  if (value.has_notes()) {
    result.notes = value.notes();
  }
  return result;
}

[[nodiscard]] edit::SavedMediaView decodeSavedMediaView(const wire::SavedMediaView& value,
                                                        std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  edit::SavedMediaView result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.name = value.name();
  for (const auto& column : value.visible_columns()) {
    result.visible_columns.push_back(column);
  }
  if (value.has_search()) {
    result.search = decodeSmartQuery(value.search());
  }
  return result;
}

[[nodiscard]] edit::MediaBin decodeBin(const wire::MediaBin& value, std::string_view path,
                                       IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  edit::MediaBin result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.name = value.name();
  if (value.has_parent_id()) {
    result.parent_id = decodeId(value.parent_id(), childPath(path, "parent_id"));
  }
  result.kind = decodeMediaBinKind(value.kind(), childPath(path, "kind"));
  if (value.has_query()) {
    result.query = decodeSmartQuery(value.query());
  }
  return result;
}

[[nodiscard]] edit::MulticamSwitch decodeMulticamSwitch(const wire::MulticamSwitch& value,
                                                        std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_time(), childPath(path, "time"));
  requirePresent(value.has_angle_id(), childPath(path, "angle_id"));
  edit::MulticamSwitch result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.time = decodeTime(value.time(), childPath(path, "time"));
  result.angle_id = decodeId(value.angle_id(), childPath(path, "angle_id"));
  return result;
}

[[nodiscard]] edit::MulticamAngle decodeMulticamAngle(const wire::MulticamAngle& value,
                                                      std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_clip_id(), childPath(path, "clip_id"));
  requirePresent(value.has_sync_offset(), childPath(path, "sync_offset"));
  edit::MulticamAngle result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.clip_id = decodeId(value.clip_id(), childPath(path, "clip_id"));
  result.sync_offset = decodeTime(value.sync_offset(), childPath(path, "sync_offset"));
  result.label = value.label();
  return result;
}

[[nodiscard]] edit::MulticamGroup decodeMulticamGroup(const wire::MulticamGroup& value,
                                                      std::string_view path, IdRegistry& ids) {
  requirePresent(value.has_id(), childPath(path, "id"));
  requirePresent(value.has_sequence_id(), childPath(path, "sequence_id"));
  requirePresent(value.has_active_angle_id(), childPath(path, "active_angle_id"));
  requirePresent(value.has_audio_master_angle_id(), childPath(path, "audio_master_angle_id"));
  requirePresent(value.has_sync_reference(), childPath(path, "sync_reference"));
  edit::MulticamGroup result;
  result.id = decodeId(value.id(), childPath(path, "id"), &ids);
  result.sequence_id = decodeId(value.sequence_id(), childPath(path, "sequence_id"));
  result.name = value.name();
  std::size_t index = 0;
  for (const auto& angle : value.angles()) {
    result.angles.push_back(
        decodeMulticamAngle(angle, indexedPath(path, "angles", index++), ids));
  }
  index = 0;
  for (const auto& entry : value.switches()) {
    result.switches.push_back(
        decodeMulticamSwitch(entry, indexedPath(path, "switches", index++), ids));
  }
  result.active_angle_id =
      decodeId(value.active_angle_id(), childPath(path, "active_angle_id"));
  result.audio_master_angle_id =
      decodeId(value.audio_master_angle_id(), childPath(path, "audio_master_angle_id"));
  result.sync_reference =
      decodeTime(value.sync_reference(), childPath(path, "sync_reference"));
  return result;
}

[[nodiscard]] edit::Project decodeProject(const wire::Project& value,
                                          const std::uint32_t declared_schema_version) {
  requirePresent(value.has_id(), "project.id");
  IdRegistry ids;
  edit::Project result;
  result.id = decodeId(value.id(), "project.id", &ids);
  result.name = value.name();
  std::size_t index = 0;
  for (const auto& asset : value.assets()) {
    result.assets.push_back(decodeAsset(asset, indexedPath("project", "assets", index++), ids));
  }
  index = 0;
  for (const auto& sequence : value.sequences()) {
    result.sequences.push_back(decodeSequence(
        sequence, indexedPath("project", "sequences", index++), declared_schema_version, ids));
  }
  index = 0;
  for (const auto& bin : value.bins()) {
    result.bins.push_back(decodeBin(bin, indexedPath("project", "bins", index++), ids));
  }
  index = 0;
  for (const auto& group : value.multicam_groups()) {
    result.multicam_groups.push_back(
        decodeMulticamGroup(group, indexedPath("project", "multicam_groups", index++), ids));
  }
  index = 0;
  for (const auto& subclip : value.subclips()) {
    result.subclips.push_back(
        decodeSubclip(subclip, indexedPath("project", "subclips", index++), ids));
  }
  index = 0;
  for (const auto& view : value.saved_media_views()) {
    result.saved_media_views.push_back(
        decodeSavedMediaView(view, indexedPath("project", "saved_media_views", index++), ids));
  }
  if (value.has_active_media_view_id()) {
    result.active_media_view_id =
        decodeId(value.active_media_view_id(), "project.active_media_view_id");
  }
  result.metadata = decodeMetadata(value.metadata(), "project.metadata");

  try {
    [[maybe_unused]] edit::TimelineEditor validator(result);
  } catch (const std::invalid_argument& exception) {
    fail(CodecErrorCode::InvalidProject, "project", exception.what());
  }
  return result;
}

} // namespace

CodecException::CodecException(CodecError error)
    : std::runtime_error(error.field_path.empty() ? error.message
                                                  : error.field_path + ": " + error.message),
      error_(std::move(error)) {}

ProjectBytes serialize_project(const edit::Project& project) {
  try {
    // Reuse the edit-model's authoritative cross-entity validation before
    // applying codec-specific ordering, finite-number, and schema checks.
    [[maybe_unused]] edit::TimelineEditor validator(project);

    wire::ProjectSnapshot snapshot;
    snapshot.set_schema_version(kCurrentSchemaVersion);
    snapshot.set_minimum_reader_version(kMinimumReaderVersion);
    encodeProject(project, snapshot.mutable_project());

    const auto size = snapshot.ByteSizeLong();
    require(size <= kMaximumSnapshotBytes, CodecErrorCode::SerializationFailed, "snapshot",
            "serialized project exceeds the snapshot size limit");
    require(size <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
            CodecErrorCode::SerializationFailed, "snapshot",
            "serialized project exceeds the protobuf array size limit");

    ProjectBytes result(size);
    google::protobuf::io::ArrayOutputStream array_output(result.data(), static_cast<int>(size));
    google::protobuf::io::CodedOutputStream coded_output(&array_output);
    coded_output.SetSerializationDeterministic(true);
    if (!snapshot.SerializeToCodedStream(&coded_output) || coded_output.HadError() ||
        coded_output.ByteCount() != static_cast<int>(size)) {
      fail(CodecErrorCode::SerializationFailed, "snapshot",
           "protobuf failed to serialize the project snapshot");
    }
    return result;
  } catch (const CodecException&) {
    throw;
  } catch (const std::invalid_argument& exception) {
    fail(CodecErrorCode::InvalidProject, "project", exception.what());
  } catch (const std::overflow_error& exception) {
    fail(CodecErrorCode::InvalidProject, "project", exception.what());
  }
}

edit::Result<edit::Project, CodecError> deserialize_project(std::span<const std::byte> bytes) {
  try {
    require(bytes.size() <= kMaximumSnapshotBytes, CodecErrorCode::MalformedProtobuf, "snapshot",
            "project snapshot exceeds the configured size limit");
    require(bytes.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
            CodecErrorCode::MalformedProtobuf, "snapshot",
            "project snapshot exceeds the protobuf parser size limit");

    wire::ProjectSnapshot snapshot;
    if (!snapshot.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
      fail(CodecErrorCode::MalformedProtobuf, "snapshot",
           "bytes are not a valid project snapshot protobuf");
    }
    require(snapshot.schema_version() != 0, CodecErrorCode::MissingField, "snapshot.schema_version",
            "schema version is missing");
    require(snapshot.minimum_reader_version() != 0, CodecErrorCode::MissingField,
            "snapshot.minimum_reader_version", "minimum reader version is missing");
    if (snapshot.minimum_reader_version() > kCurrentSchemaVersion) {
      fail(CodecErrorCode::UnsupportedMinimumReaderVersion, "snapshot.minimum_reader_version",
           "snapshot requires a newer project reader");
    }
    if (snapshot.schema_version() < kMinimumReaderVersion ||
        snapshot.schema_version() > kCurrentSchemaVersion) {
      fail(CodecErrorCode::UnsupportedSchemaVersion, "snapshot.schema_version",
           "snapshot schema version is not supported by this reader");
    }
    require(snapshot.schema_version() >= snapshot.minimum_reader_version(),
            CodecErrorCode::InvalidField, "snapshot",
            "schema version cannot be older than its minimum reader version");
    requirePresent(snapshot.has_project(), "snapshot.project");
    reject_v2_fields_in_declared_v1(snapshot);
    reject_v3_fields_in_declared_older(snapshot);
    reject_v4_fields_in_declared_older(snapshot);
    reject_v5_fields_in_declared_older(snapshot);
    reject_v6_fields_in_declared_older(snapshot);
    reject_v7_fields_in_declared_older(snapshot);
    if (const auto unknown = findUnknownField(snapshot, "snapshot")) {
      fail(CodecErrorCode::InvalidField, *unknown,
           "snapshot contains fields not defined by its declared schema version");
    }
    return edit::Result<edit::Project, CodecError>::success(
        decodeProject(snapshot.project(), snapshot.schema_version()));
  } catch (const CodecException& exception) {
    return edit::Result<edit::Project, CodecError>::failure(exception.error());
  } catch (const std::invalid_argument& exception) {
    return edit::Result<edit::Project, CodecError>::failure(
        CodecError{CodecErrorCode::InvalidField, exception.what(), "snapshot"});
  } catch (const std::overflow_error& exception) {
    return edit::Result<edit::Project, CodecError>::failure(
        CodecError{CodecErrorCode::InvalidField, exception.what(), "snapshot"});
  }
}

} // namespace video_editor::project_codec
