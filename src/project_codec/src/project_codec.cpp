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
}

void encodeClip(const edit::Clip& value, wire::Clip* output, std::string_view path,
                IdRegistry& ids) {
  encodeId(value.id, output->mutable_id(), childPath(path, "id"), &ids);
  encodeId(value.asset_id, output->mutable_asset_id(), childPath(path, "asset_id"), nullptr,
           value.kind == edit::ClipKind::Title);
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
                             result.kind == edit::ClipKind::Title);
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
