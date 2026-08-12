// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"
#include "video_editor/edit_model/time.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace video_editor::edit {

struct Revision final {
  std::uint64_t value{0};
  friend bool operator==(const Revision&, const Revision&) = default;
  friend auto operator<=>(const Revision&, const Revision&) = default;
};

struct Vec2 final {
  double x{0.0};
  double y{0.0};
  friend bool operator==(const Vec2&, const Vec2&) = default;
};

struct ColorRgba final {
  double red{0.0};
  double green{0.0};
  double blue{0.0};
  double alpha{1.0};
  friend bool operator==(const ColorRgba&, const ColorRgba&) = default;
};

enum class TitleHorizontalAlignment { Left, Center, Right };

struct Title final {
  std::string text;
  std::string font_family{"sans-serif"};
  double font_size{96.0};
  ColorRgba foreground_color{1.0, 1.0, 1.0, 1.0};
  ColorRgba background_color{0.0, 0.0, 0.0, 0.0};
  TitleHorizontalAlignment horizontal_alignment{TitleHorizontalAlignment::Center};
  bool bold{false};
  bool italic{false};
  friend bool operator==(const Title&, const Title&) = default;
};

using EffectValue = std::variant<std::int64_t, double, bool, std::string, Time, Vec2, ColorRgba>;

enum class KeyframeInterpolation { Hold, Linear, Bezier };

struct Keyframe final {
  EntityId id{EntityId::generate()};
  Time time{};
  EffectValue value{0.0};
  KeyframeInterpolation interpolation{KeyframeInterpolation::Linear};
  Vec2 incoming_control{};
  Vec2 outgoing_control{};
  friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

struct EffectParameter final {
  std::string id;
  EffectValue value{0.0};
  std::vector<Keyframe> keyframes;
  friend bool operator==(const EffectParameter&, const EffectParameter&) = default;
};

struct Effect final {
  EntityId id{EntityId::generate()};
  std::string type;
  std::uint32_t version{1};
  bool enabled{true};
  bool known{true};
  std::map<std::string, EffectParameter, std::less<>> parameters;
  std::vector<std::uint8_t> opaque_payload;
  friend bool operator==(const Effect&, const Effect&) = default;
};

struct Transform final {
  Vec2 position{};
  Vec2 scale{1.0, 1.0};
  double rotation_degrees{0.0};
  double anchor_x{0.5};
  double anchor_y{0.5};
  double crop_left{0.0};
  double crop_top{0.0};
  double crop_right{0.0};
  double crop_bottom{0.0};
  double opacity{1.0};
  friend bool operator==(const Transform&, const Transform&) = default;
};

struct Asset final {
  EntityId id{EntityId::generate()};
  std::string name;
  std::string source_uri;
  std::string fingerprint;
  Time duration{};
  bool has_video{false};
  bool has_audio{false};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::optional<Rate> nominal_frame_rate;
  std::uint32_t audio_sample_rate{0};
  std::uint32_t audio_channels{0};
  std::map<std::string, std::string, std::less<>> metadata;
  friend bool operator==(const Asset&, const Asset&) = default;
};

enum class TrackKind { Video, Audio, Caption };
enum class ClipKind { Video, Audio, Title };
enum class BlendMode { Normal, Add, Multiply, Screen, Overlay };
enum class TransitionKind { CrossDissolve, DipToBlack };

struct Clip final {
  EntityId id{EntityId::generate()};
  EntityId asset_id{};
  ClipKind kind{ClipKind::Video};
  std::string name;
  TimeRange timeline_range{};
  TimeRange source_range{};
  Rate playback_rate{1, 1};
  bool reversed{false};
  std::optional<EntityId> linked_group;
  Transform transform{};
  BlendMode blend_mode{BlendMode::Normal};
  double audio_gain_db{0.0};
  double audio_pan{0.0};
  Time fade_in{};
  Time fade_out{};
  std::vector<Effect> effects;
  std::optional<Title> title;
  friend bool operator==(const Clip&, const Clip&) = default;
};

struct Gap final {
  TimeRange timeline_range{};
  friend bool operator==(const Gap&, const Gap&) = default;
};

struct Track final {
  EntityId id{EntityId::generate()};
  TrackKind kind{TrackKind::Video};
  std::string name;
  bool locked{false};
  bool muted{false};
  bool solo{false};
  // Visibility controls visual compositing only. Targeting is an editorial
  // routing hint for the UI and never changes rendering.
  bool visible{true};
  bool targeted{true};
  std::vector<Clip> clips;
  std::vector<Effect> effects;
  // Mixer strip gain and pan for audio tracks. Defaults are unity (0 dB,
  // center). Clip-level gain/pan are applied first; track-level gain/pan are
  // applied to the accumulated track mix. Non-audio tracks ignore these.
  double audio_gain_db{0.0};
  double audio_pan{0.0};
  friend bool operator==(const Track&, const Track&) = default;
};

struct Marker final {
  EntityId id{EntityId::generate()};
  TimeRange range{};
  std::string label;
  ColorRgba color{1.0, 0.75, 0.0, 1.0};
  friend bool operator==(const Marker&, const Marker&) = default;
};

struct CaptionStyle final {
  std::string font_family{"sans-serif"};
  double font_size{48.0};
  ColorRgba text_color{1.0, 1.0, 1.0, 1.0};
  ColorRgba background_color{0.0, 0.0, 0.0, 0.7};
  bool bold{false};
  bool italic{false};
  friend bool operator==(const CaptionStyle&, const CaptionStyle&) = default;
};

struct Caption final {
  EntityId id{EntityId::generate()};
  TimeRange range{};
  std::string text;
  std::string language;
  CaptionStyle style{};
  friend bool operator==(const Caption&, const Caption&) = default;
};

struct Transition final {
  EntityId id{EntityId::generate()};
  EntityId outgoing_clip_id{};
  EntityId incoming_clip_id{};
  TimeRange range{};
  TransitionKind kind{TransitionKind::CrossDissolve};
  bool enabled{true};
  friend bool operator==(const Transition&, const Transition&) = default;
};

struct Sequence final {
  EntityId id{EntityId::generate()};
  std::string name;
  Rate frame_rate{30, 1};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t audio_sample_rate{48'000};
  std::vector<Track> tracks;
  std::vector<Marker> markers;
  std::vector<Caption> captions;
  std::vector<Transition> transitions;
  friend bool operator==(const Sequence&, const Sequence&) = default;
};

struct Project final {
  EntityId id{EntityId::generate()};
  std::string name{"Untitled project"};
  std::vector<Asset> assets;
  std::vector<Sequence> sequences;
  std::map<std::string, std::string, std::less<>> metadata;
  friend bool operator==(const Project&, const Project&) = default;
};

[[nodiscard]] const Asset* findAsset(const Project& project, EntityId id) noexcept;
[[nodiscard]] const Sequence* findSequence(const Project& project, EntityId id) noexcept;
[[nodiscard]] const Track* findTrack(const Sequence& sequence, EntityId id) noexcept;
[[nodiscard]] const Clip* findClip(const Sequence& sequence, EntityId id) noexcept;
[[nodiscard]] const Transition* findTransition(const Sequence& sequence, EntityId id) noexcept;
[[nodiscard]] Time sequenceDuration(const Sequence& sequence);

} // namespace video_editor::edit
