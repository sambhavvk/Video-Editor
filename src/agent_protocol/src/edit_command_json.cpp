// SPDX-License-Identifier: MPL-2.0
#include "video_editor/agent_protocol/edit_command_json.h"

#include "video_editor/edit_model/commands.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace video_editor::agent_protocol {
namespace {

using edit::EditCommand;
using edit::EditOperation;

template <typename T>
using DecodeResult = edit::Result<T, CodecError>;

[[nodiscard]] CodecError makeError(const std::string& message) { return CodecError{message}; }

[[nodiscard]] CodecError makeError(const char* message) { return CodecError{message}; }

template <typename T>
[[nodiscard]] DecodeResult<T> fail(const std::string& message) {
  return DecodeResult<T>::failure(makeError(message));
}

template <typename T>
[[nodiscard]] DecodeResult<T> fail(const char* message) {
  return DecodeResult<T>::failure(makeError(message));
}

[[nodiscard]] std::string qstrToStd(const QString& value) {
  return value.toUtf8().toStdString();
}

[[nodiscard]] QString stdToQstr(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

// --- Time / Rate / TimeRange ---

[[nodiscard]] QJsonObject encodeTime(const edit::Time& time) {
  QJsonObject object;
  object.insert(QStringLiteral("value"), static_cast<qint64>(time.value()));
  object.insert(QStringLiteral("timescale"), static_cast<int>(time.timescale()));
  return object;
}

[[nodiscard]] DecodeResult<edit::Time> decodeTime(const QJsonValue& value,
                                                  const char* context) {
  if (!value.isObject()) {
    return fail<edit::Time>(std::string(context) + ": expected time object");
  }
  const QJsonObject object = value.toObject();
  if (!object.contains(QStringLiteral("value")) ||
      !object.contains(QStringLiteral("timescale"))) {
    return fail<edit::Time>(std::string(context) + ": time requires value and timescale");
  }
  const qint64 raw_value = object.value(QStringLiteral("value")).toInteger();
  const int raw_timescale = object.value(QStringLiteral("timescale")).toInt();
  if (raw_timescale <= 0) {
    return fail<edit::Time>(std::string(context) + ": timescale must be positive");
  }
  return DecodeResult<edit::Time>::success(
      edit::Time(raw_value, static_cast<std::uint32_t>(raw_timescale)));
}

[[nodiscard]] QJsonObject encodeRate(const edit::Rate& rate) {
  QJsonObject object;
  object.insert(QStringLiteral("numerator"), static_cast<int>(rate.numerator()));
  object.insert(QStringLiteral("denominator"), static_cast<int>(rate.denominator()));
  return object;
}

[[nodiscard]] DecodeResult<edit::Rate> decodeRate(const QJsonValue& value,
                                                    const char* context) {
  if (!value.isObject()) {
    return fail<edit::Rate>(std::string(context) + ": expected rate object");
  }
  const QJsonObject object = value.toObject();
  if (!object.contains(QStringLiteral("numerator")) ||
      !object.contains(QStringLiteral("denominator"))) {
    return fail<edit::Rate>(std::string(context) + ": rate requires numerator and denominator");
  }
  const int numerator = object.value(QStringLiteral("numerator")).toInt();
  const int denominator = object.value(QStringLiteral("denominator")).toInt();
  if (numerator <= 0 || denominator <= 0) {
    return fail<edit::Rate>(std::string(context) + ": rate components must be positive");
  }
  return DecodeResult<edit::Rate>::success(
      edit::Rate(static_cast<std::uint32_t>(numerator),
                 static_cast<std::uint32_t>(denominator)));
}

[[nodiscard]] QJsonObject encodeTimeRange(const edit::TimeRange& range) {
  QJsonObject object;
  object.insert(QStringLiteral("start"), encodeTime(range.start));
  object.insert(QStringLiteral("duration"), encodeTime(range.duration));
  return object;
}

[[nodiscard]] DecodeResult<edit::TimeRange> decodeTimeRange(const QJsonValue& value,
                                                            const char* context) {
  if (!value.isObject()) {
    return fail<edit::TimeRange>(std::string(context) + ": expected time range object");
  }
  const QJsonObject object = value.toObject();
  const auto start = decodeTime(object.value(QStringLiteral("start")),
                                (std::string(context) + ".start").c_str());
  if (!start) {
    return fail<edit::TimeRange>(start.error().message);
  }
  const auto duration = decodeTime(object.value(QStringLiteral("duration")),
                                   (std::string(context) + ".duration").c_str());
  if (!duration) {
    return fail<edit::TimeRange>(duration.error().message);
  }
  return DecodeResult<edit::TimeRange>::success(
      edit::TimeRange(start.value(), duration.value()));
}

// --- EntityId ---

[[nodiscard]] QJsonValue encodeEntityId(const edit::EntityId& id) {
  return QString::fromStdString(id.toString());
}

[[nodiscard]] DecodeResult<edit::EntityId> decodeEntityIdRequired(const QJsonValue& value,
                                                                  const char* context) {
  if (!value.isString()) {
    return fail<edit::EntityId>(std::string(context) + ": expected entity id string");
  }
  const auto parsed = edit::EntityId::parse(qstrToStd(value.toString()));
  if (!parsed.has_value()) {
    return fail<edit::EntityId>(std::string(context) + ": invalid entity id");
  }
  if (parsed->isNil()) {
    return fail<edit::EntityId>(std::string(context) + ": entity id must not be nil");
  }
  return DecodeResult<edit::EntityId>::success(*parsed);
}

[[nodiscard]] edit::EntityId decodeEntityIdNew(const QJsonObject& object) {
  const QJsonValue value = object.value(QStringLiteral("id"));
  if (!value.isString()) {
    return edit::EntityId::generate();
  }
  const auto parsed = edit::EntityId::parse(qstrToStd(value.toString()));
  if (!parsed.has_value() || parsed->isNil()) {
    return edit::EntityId::generate();
  }
  return *parsed;
}

[[nodiscard]] DecodeResult<std::optional<edit::EntityId>> decodeOptionalEntityId(
    const QJsonValue& value, const char* context) {
  if (value.isUndefined() || value.isNull()) {
    return DecodeResult<std::optional<edit::EntityId>>::success(std::nullopt);
  }
  const auto required = decodeEntityIdRequired(value, context);
  if (!required) {
    return fail<std::optional<edit::EntityId>>(required.error().message);
  }
  return DecodeResult<std::optional<edit::EntityId>>::success(required.value());
}

void insertOptionalEntityId(QJsonObject& object, const QString& key,
                                        const std::optional<edit::EntityId>& id) {
  if (id.has_value()) {
    object.insert(key, encodeEntityId(*id));
  }
}

// --- Vec2 / Color ---

[[nodiscard]] QJsonObject encodeVec2(const edit::Vec2& vec) {
  QJsonObject object;
  object.insert(QStringLiteral("x"), vec.x);
  object.insert(QStringLiteral("y"), vec.y);
  return object;
}

[[nodiscard]] DecodeResult<edit::Vec2> decodeVec2(const QJsonValue& value,
                                                  const char* context) {
  if (!value.isObject()) {
    return fail<edit::Vec2>(std::string(context) + ": expected vec2 object");
  }
  const QJsonObject object = value.toObject();
  edit::Vec2 vec;
  vec.x = object.value(QStringLiteral("x")).toDouble();
  vec.y = object.value(QStringLiteral("y")).toDouble();
  return DecodeResult<edit::Vec2>::success(vec);
}

[[nodiscard]] QJsonObject encodeColor(const edit::ColorRgba& color) {
  QJsonObject object;
  object.insert(QStringLiteral("red"), color.red);
  object.insert(QStringLiteral("green"), color.green);
  object.insert(QStringLiteral("blue"), color.blue);
  object.insert(QStringLiteral("alpha"), color.alpha);
  return object;
}

[[nodiscard]] DecodeResult<edit::ColorRgba> decodeColor(const QJsonValue& value,
                                                         const char* context) {
  if (!value.isObject()) {
    return fail<edit::ColorRgba>(std::string(context) + ": expected color object");
  }
  const QJsonObject object = value.toObject();
  edit::ColorRgba color;
  color.red = object.value(QStringLiteral("red")).toDouble();
  color.green = object.value(QStringLiteral("green")).toDouble();
  color.blue = object.value(QStringLiteral("blue")).toDouble();
  color.alpha = object.value(QStringLiteral("alpha")).toDouble(1.0);
  return DecodeResult<edit::ColorRgba>::success(color);
}

// --- String map ---

[[nodiscard]] QJsonObject encodeStringMap(
    const std::map<std::string, std::string, std::less<>>& map) {
  QJsonObject object;
  for (const auto& [key, value] : map) {
    object.insert(stdToQstr(key), stdToQstr(value));
  }
  return object;
}

[[nodiscard]] DecodeResult<std::map<std::string, std::string, std::less<>>> decodeStringMap(
    const QJsonValue& value, const char* context) {
  if (!value.isObject()) {
    return fail<std::map<std::string, std::string, std::less<>>>(
        std::string(context) + ": expected object map");
  }
  std::map<std::string, std::string, std::less<>> map;
  const QJsonObject object = value.toObject();
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!it.value().isString()) {
      return fail<std::map<std::string, std::string, std::less<>>>(
          std::string(context) + ": map values must be strings");
    }
    map.emplace(qstrToStd(it.key()), qstrToStd(it.value().toString()));
  }
  return DecodeResult<std::map<std::string, std::string, std::less<>>>::success(
      std::move(map));
}

// --- Enums ---

[[nodiscard]] QString encodeInsertMode(edit::InsertMode mode) {
  switch (mode) {
  case edit::InsertMode::RejectOverlap:
    return QStringLiteral("reject_overlap");
  case edit::InsertMode::Overwrite:
    return QStringLiteral("overwrite");
  case edit::InsertMode::Ripple:
    return QStringLiteral("ripple");
  }
  return QStringLiteral("reject_overlap");
}

[[nodiscard]] DecodeResult<edit::InsertMode> decodeInsertMode(const QJsonValue& value,
                                                              const char* context) {
  if (!value.isString()) {
    return fail<edit::InsertMode>(std::string(context) + ": expected insert mode string");
  }
  const QString mode = value.toString();
  if (mode == QStringLiteral("reject_overlap")) {
    return DecodeResult<edit::InsertMode>::success(edit::InsertMode::RejectOverlap);
  }
  if (mode == QStringLiteral("overwrite")) {
    return DecodeResult<edit::InsertMode>::success(edit::InsertMode::Overwrite);
  }
  if (mode == QStringLiteral("ripple")) {
    return DecodeResult<edit::InsertMode>::success(edit::InsertMode::Ripple);
  }
  return fail<edit::InsertMode>(std::string(context) + ": unknown insert mode");
}

[[nodiscard]] QString encodeClipKind(edit::ClipKind kind) {
  switch (kind) {
  case edit::ClipKind::Video:
    return QStringLiteral("video");
  case edit::ClipKind::Audio:
    return QStringLiteral("audio");
  case edit::ClipKind::Title:
    return QStringLiteral("title");
  case edit::ClipKind::NestedSequence:
    return QStringLiteral("nested_sequence");
  }
  return QStringLiteral("video");
}

[[nodiscard]] DecodeResult<edit::ClipKind> decodeClipKind(const QJsonValue& value,
                                                          const char* context) {
  if (!value.isString()) {
    return fail<edit::ClipKind>(std::string(context) + ": expected clip kind string");
  }
  const QString kind = value.toString();
  if (kind == QStringLiteral("video")) {
    return DecodeResult<edit::ClipKind>::success(edit::ClipKind::Video);
  }
  if (kind == QStringLiteral("audio")) {
    return DecodeResult<edit::ClipKind>::success(edit::ClipKind::Audio);
  }
  if (kind == QStringLiteral("title")) {
    return DecodeResult<edit::ClipKind>::success(edit::ClipKind::Title);
  }
  if (kind == QStringLiteral("nested_sequence")) {
    return DecodeResult<edit::ClipKind>::success(edit::ClipKind::NestedSequence);
  }
  return fail<edit::ClipKind>(std::string(context) + ": unknown clip kind");
}

[[nodiscard]] QString encodeTrackKind(edit::TrackKind kind) {
  switch (kind) {
  case edit::TrackKind::Video:
    return QStringLiteral("video");
  case edit::TrackKind::Audio:
    return QStringLiteral("audio");
  case edit::TrackKind::Caption:
    return QStringLiteral("caption");
  }
  return QStringLiteral("video");
}

[[nodiscard]] DecodeResult<edit::TrackKind> decodeTrackKind(const QJsonValue& value,
                                                            const char* context) {
  if (!value.isString()) {
    return fail<edit::TrackKind>(std::string(context) + ": expected track kind string");
  }
  const QString kind = value.toString();
  if (kind == QStringLiteral("video")) {
    return DecodeResult<edit::TrackKind>::success(edit::TrackKind::Video);
  }
  if (kind == QStringLiteral("audio")) {
    return DecodeResult<edit::TrackKind>::success(edit::TrackKind::Audio);
  }
  if (kind == QStringLiteral("caption")) {
    return DecodeResult<edit::TrackKind>::success(edit::TrackKind::Caption);
  }
  return fail<edit::TrackKind>(std::string(context) + ": unknown track kind");
}

[[nodiscard]] QString encodeBlendMode(edit::BlendMode mode) {
  switch (mode) {
  case edit::BlendMode::Normal:
    return QStringLiteral("normal");
  case edit::BlendMode::Add:
    return QStringLiteral("add");
  case edit::BlendMode::Multiply:
    return QStringLiteral("multiply");
  case edit::BlendMode::Screen:
    return QStringLiteral("screen");
  case edit::BlendMode::Overlay:
    return QStringLiteral("overlay");
  }
  return QStringLiteral("normal");
}

[[nodiscard]] DecodeResult<edit::BlendMode> decodeBlendMode(const QJsonValue& value,
                                                            const char* context) {
  if (!value.isString()) {
    return fail<edit::BlendMode>(std::string(context) + ": expected blend mode string");
  }
  const QString mode = value.toString();
  if (mode == QStringLiteral("normal")) {
    return DecodeResult<edit::BlendMode>::success(edit::BlendMode::Normal);
  }
  if (mode == QStringLiteral("add")) {
    return DecodeResult<edit::BlendMode>::success(edit::BlendMode::Add);
  }
  if (mode == QStringLiteral("multiply")) {
    return DecodeResult<edit::BlendMode>::success(edit::BlendMode::Multiply);
  }
  if (mode == QStringLiteral("screen")) {
    return DecodeResult<edit::BlendMode>::success(edit::BlendMode::Screen);
  }
  if (mode == QStringLiteral("overlay")) {
    return DecodeResult<edit::BlendMode>::success(edit::BlendMode::Overlay);
  }
  return fail<edit::BlendMode>(std::string(context) + ": unknown blend mode");
}

[[nodiscard]] QString encodeTransitionKind(edit::TransitionKind kind) {
  switch (kind) {
  case edit::TransitionKind::CrossDissolve:
    return QStringLiteral("cross_dissolve");
  case edit::TransitionKind::DipToBlack:
    return QStringLiteral("dip_to_black");
  }
  return QStringLiteral("cross_dissolve");
}

[[nodiscard]] DecodeResult<edit::TransitionKind> decodeTransitionKind(
    const QJsonValue& value, const char* context) {
  if (!value.isString()) {
    return fail<edit::TransitionKind>(std::string(context) +
                                        ": expected transition kind string");
  }
  const QString kind = value.toString();
  if (kind == QStringLiteral("cross_dissolve")) {
    return DecodeResult<edit::TransitionKind>::success(edit::TransitionKind::CrossDissolve);
  }
  if (kind == QStringLiteral("dip_to_black")) {
    return DecodeResult<edit::TransitionKind>::success(edit::TransitionKind::DipToBlack);
  }
  return fail<edit::TransitionKind>(std::string(context) + ": unknown transition kind");
}

[[nodiscard]] QString encodeMediaBinKind(edit::MediaBinKind kind) {
  switch (kind) {
  case edit::MediaBinKind::Folder:
    return QStringLiteral("folder");
  case edit::MediaBinKind::Smart:
    return QStringLiteral("smart");
  }
  return QStringLiteral("folder");
}

[[nodiscard]] DecodeResult<edit::MediaBinKind> decodeMediaBinKind(const QJsonValue& value,
                                                                  const char* context) {
  if (!value.isString()) {
    return fail<edit::MediaBinKind>(std::string(context) + ": expected media bin kind string");
  }
  const QString kind = value.toString();
  if (kind == QStringLiteral("folder")) {
    return DecodeResult<edit::MediaBinKind>::success(edit::MediaBinKind::Folder);
  }
  if (kind == QStringLiteral("smart")) {
    return DecodeResult<edit::MediaBinKind>::success(edit::MediaBinKind::Smart);
  }
  return fail<edit::MediaBinKind>(std::string(context) + ": unknown media bin kind");
}

[[nodiscard]] QString encodeCaptionAlignment(edit::CaptionAlignment alignment) {
  switch (alignment) {
  case edit::CaptionAlignment::Left:
    return QStringLiteral("left");
  case edit::CaptionAlignment::Center:
    return QStringLiteral("center");
  case edit::CaptionAlignment::Right:
    return QStringLiteral("right");
  }
  return QStringLiteral("center");
}

[[nodiscard]] DecodeResult<edit::CaptionAlignment> decodeCaptionAlignment(
    const QJsonValue& value, const char* context) {
  if (!value.isString()) {
    return fail<edit::CaptionAlignment>(std::string(context) +
                                        ": expected caption alignment string");
  }
  const QString alignment = value.toString();
  if (alignment == QStringLiteral("left")) {
    return DecodeResult<edit::CaptionAlignment>::success(edit::CaptionAlignment::Left);
  }
  if (alignment == QStringLiteral("center")) {
    return DecodeResult<edit::CaptionAlignment>::success(edit::CaptionAlignment::Center);
  }
  if (alignment == QStringLiteral("right")) {
    return DecodeResult<edit::CaptionAlignment>::success(edit::CaptionAlignment::Right);
  }
  return fail<edit::CaptionAlignment>(std::string(context) + ": unknown caption alignment");
}

[[nodiscard]] QString encodeCaptionWordSource(edit::CaptionWordSource source) {
  switch (source) {
  case edit::CaptionWordSource::Unknown:
    return QStringLiteral("unknown");
  case edit::CaptionWordSource::Imported:
    return QStringLiteral("imported");
  case edit::CaptionWordSource::LocalTranscription:
    return QStringLiteral("local_transcription");
  case edit::CaptionWordSource::UserEdited:
    return QStringLiteral("user_edited");
  }
  return QStringLiteral("unknown");
}

[[nodiscard]] DecodeResult<edit::CaptionWordSource> decodeCaptionWordSource(
    const QJsonValue& value, const char* context) {
  if (!value.isString()) {
    return fail<edit::CaptionWordSource>(std::string(context) +
                                         ": expected caption word source string");
  }
  const QString source = value.toString();
  if (source == QStringLiteral("unknown")) {
    return DecodeResult<edit::CaptionWordSource>::success(edit::CaptionWordSource::Unknown);
  }
  if (source == QStringLiteral("imported")) {
    return DecodeResult<edit::CaptionWordSource>::success(edit::CaptionWordSource::Imported);
  }
  if (source == QStringLiteral("local_transcription")) {
    return DecodeResult<edit::CaptionWordSource>::success(
        edit::CaptionWordSource::LocalTranscription);
  }
  if (source == QStringLiteral("user_edited")) {
    return DecodeResult<edit::CaptionWordSource>::success(edit::CaptionWordSource::UserEdited);
  }
  return fail<edit::CaptionWordSource>(std::string(context) + ": unknown caption word source");
}

[[nodiscard]] QString encodeKeyframeInterpolation(edit::KeyframeInterpolation interpolation) {
  switch (interpolation) {
  case edit::KeyframeInterpolation::Hold:
    return QStringLiteral("hold");
  case edit::KeyframeInterpolation::Linear:
    return QStringLiteral("linear");
  case edit::KeyframeInterpolation::Bezier:
    return QStringLiteral("bezier");
  }
  return QStringLiteral("linear");
}

[[nodiscard]] DecodeResult<edit::KeyframeInterpolation> decodeKeyframeInterpolation(
    const QJsonValue& value, const char* context) {
  if (!value.isString()) {
    return fail<edit::KeyframeInterpolation>(
        std::string(context) + ": expected keyframe interpolation string");
  }
  const QString interpolation = value.toString();
  if (interpolation == QStringLiteral("hold")) {
    return DecodeResult<edit::KeyframeInterpolation>::success(
        edit::KeyframeInterpolation::Hold);
  }
  if (interpolation == QStringLiteral("linear")) {
    return DecodeResult<edit::KeyframeInterpolation>::success(
        edit::KeyframeInterpolation::Linear);
  }
  if (interpolation == QStringLiteral("bezier")) {
    return DecodeResult<edit::KeyframeInterpolation>::success(
        edit::KeyframeInterpolation::Bezier);
  }
  return fail<edit::KeyframeInterpolation>(std::string(context) +
                                           ": unknown keyframe interpolation");
}

[[nodiscard]] QString encodeTitleHorizontalAlignment(edit::TitleHorizontalAlignment alignment) {
  switch (alignment) {
  case edit::TitleHorizontalAlignment::Left:
    return QStringLiteral("left");
  case edit::TitleHorizontalAlignment::Center:
    return QStringLiteral("center");
  case edit::TitleHorizontalAlignment::Right:
    return QStringLiteral("right");
  }
  return QStringLiteral("center");
}

[[nodiscard]] DecodeResult<edit::TitleHorizontalAlignment> decodeTitleHorizontalAlignment(
    const QJsonValue& value, const char* context) {
  if (!value.isString()) {
    return fail<edit::TitleHorizontalAlignment>(
        std::string(context) + ": expected title horizontal alignment string");
  }
  const QString alignment = value.toString();
  if (alignment == QStringLiteral("left")) {
    return DecodeResult<edit::TitleHorizontalAlignment>::success(
        edit::TitleHorizontalAlignment::Left);
  }
  if (alignment == QStringLiteral("center")) {
    return DecodeResult<edit::TitleHorizontalAlignment>::success(
        edit::TitleHorizontalAlignment::Center);
  }
  if (alignment == QStringLiteral("right")) {
    return DecodeResult<edit::TitleHorizontalAlignment>::success(
        edit::TitleHorizontalAlignment::Right);
  }
  return fail<edit::TitleHorizontalAlignment>(std::string(context) +
                                              ": unknown title horizontal alignment");
}

// --- EffectValue ---

[[nodiscard]] QJsonObject encodeEffectValue(const edit::EffectValue& value) {
  QJsonObject object;
  return std::visit(
      [&object](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("int64"));
          object.insert(QStringLiteral("value"), static_cast<qint64>(typed));
        } else if constexpr (std::is_same_v<T, double>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("double"));
          object.insert(QStringLiteral("value"), typed);
        } else if constexpr (std::is_same_v<T, bool>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("bool"));
          object.insert(QStringLiteral("value"), typed);
        } else if constexpr (std::is_same_v<T, std::string>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("string"));
          object.insert(QStringLiteral("value"), stdToQstr(typed));
        } else if constexpr (std::is_same_v<T, edit::Time>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("time"));
          object.insert(QStringLiteral("value"), encodeTime(typed));
        } else if constexpr (std::is_same_v<T, edit::Vec2>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("vec2"));
          object.insert(QStringLiteral("x"), typed.x);
          object.insert(QStringLiteral("y"), typed.y);
        } else if constexpr (std::is_same_v<T, edit::ColorRgba>) {
          object.insert(QStringLiteral("kind"), QStringLiteral("color"));
          object.insert(QStringLiteral("red"), typed.red);
          object.insert(QStringLiteral("green"), typed.green);
          object.insert(QStringLiteral("blue"), typed.blue);
          object.insert(QStringLiteral("alpha"), typed.alpha);
        }
        return object;
      },
      value);
}

[[nodiscard]] DecodeResult<edit::EffectValue> decodeEffectValue(const QJsonValue& value,
                                                                  const char* context) {
  if (!value.isObject()) {
    return fail<edit::EffectValue>(std::string(context) + ": expected effect value object");
  }
  const QJsonObject object = value.toObject();
  const QString kind = object.value(QStringLiteral("kind")).toString();
  if (kind == QStringLiteral("int64")) {
    return DecodeResult<edit::EffectValue>::success(
        static_cast<std::int64_t>(object.value(QStringLiteral("value")).toInteger()));
  }
  if (kind == QStringLiteral("double")) {
    return DecodeResult<edit::EffectValue>::success(object.value(QStringLiteral("value")).toDouble());
  }
  if (kind == QStringLiteral("bool")) {
    return DecodeResult<edit::EffectValue>::success(object.value(QStringLiteral("value")).toBool());
  }
  if (kind == QStringLiteral("string")) {
    return DecodeResult<edit::EffectValue>::success(
        qstrToStd(object.value(QStringLiteral("value")).toString()));
  }
  if (kind == QStringLiteral("time")) {
    const auto time = decodeTime(object.value(QStringLiteral("value")),
                                 (std::string(context) + ".time").c_str());
    if (!time) {
      return fail<edit::EffectValue>(time.error().message);
    }
    return DecodeResult<edit::EffectValue>::success(time.value());
  }
  if (kind == QStringLiteral("vec2")) {
    edit::Vec2 vec;
    vec.x = object.value(QStringLiteral("x")).toDouble();
    vec.y = object.value(QStringLiteral("y")).toDouble();
    return DecodeResult<edit::EffectValue>::success(vec);
  }
  if (kind == QStringLiteral("color")) {
    edit::ColorRgba color;
    color.red = object.value(QStringLiteral("red")).toDouble();
    color.green = object.value(QStringLiteral("green")).toDouble();
    color.blue = object.value(QStringLiteral("blue")).toDouble();
    color.alpha = object.value(QStringLiteral("alpha")).toDouble(1.0);
    return DecodeResult<edit::EffectValue>::success(color);
  }
  return fail<edit::EffectValue>(std::string(context) + ": unknown effect value kind");
}

// --- Keyframe / EffectParameter / Effect ---

[[nodiscard]] QJsonObject encodeKeyframe(const edit::Keyframe& keyframe) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(keyframe.id));
  object.insert(QStringLiteral("time"), encodeTime(keyframe.time));
  object.insert(QStringLiteral("value"), encodeEffectValue(keyframe.value));
  object.insert(QStringLiteral("interpolation"), encodeKeyframeInterpolation(keyframe.interpolation));
  object.insert(QStringLiteral("incoming_control"), encodeVec2(keyframe.incoming_control));
  object.insert(QStringLiteral("outgoing_control"), encodeVec2(keyframe.outgoing_control));
  return object;
}

[[nodiscard]] DecodeResult<edit::Keyframe> decodeKeyframe(const QJsonValue& value,
                                                          const char* context) {
  if (!value.isObject()) {
    return fail<edit::Keyframe>(std::string(context) + ": expected keyframe object");
  }
  const QJsonObject object = value.toObject();
  edit::Keyframe keyframe;
  keyframe.id = decodeEntityIdNew(object);
  const auto time = decodeTime(object.value(QStringLiteral("time")),
                               (std::string(context) + ".time").c_str());
  if (!time) {
    return fail<edit::Keyframe>(time.error().message);
  }
  keyframe.time = time.value();
  const auto effect_value = decodeEffectValue(object.value(QStringLiteral("value")),
                                              (std::string(context) + ".value").c_str());
  if (!effect_value) {
    return fail<edit::Keyframe>(effect_value.error().message);
  }
  keyframe.value = effect_value.value();
  const auto interpolation = decodeKeyframeInterpolation(
      object.value(QStringLiteral("interpolation")),
      (std::string(context) + ".interpolation").c_str());
  if (!interpolation) {
    return fail<edit::Keyframe>(interpolation.error().message);
  }
  keyframe.interpolation = interpolation.value();
  const auto incoming = decodeVec2(object.value(QStringLiteral("incoming_control")),
                                   (std::string(context) + ".incoming_control").c_str());
  if (!incoming) {
    return fail<edit::Keyframe>(incoming.error().message);
  }
  keyframe.incoming_control = incoming.value();
  const auto outgoing = decodeVec2(object.value(QStringLiteral("outgoing_control")),
                                   (std::string(context) + ".outgoing_control").c_str());
  if (!outgoing) {
    return fail<edit::Keyframe>(outgoing.error().message);
  }
  keyframe.outgoing_control = outgoing.value();
  return DecodeResult<edit::Keyframe>::success(keyframe);
}

[[nodiscard]] QJsonObject encodeEffectParameter(const edit::EffectParameter& parameter) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(parameter.id));
  object.insert(QStringLiteral("value"), encodeEffectValue(parameter.value));
  QJsonArray keyframes;
  for (const edit::Keyframe& keyframe : parameter.keyframes) {
    keyframes.append(encodeKeyframe(keyframe));
  }
  object.insert(QStringLiteral("keyframes"), keyframes);
  return object;
}

[[nodiscard]] DecodeResult<edit::EffectParameter> decodeEffectParameter(
    const QJsonValue& value, const char* context) {
  if (!value.isObject()) {
    return fail<edit::EffectParameter>(std::string(context) + ": expected effect parameter object");
  }
  const QJsonObject object = value.toObject();
  edit::EffectParameter parameter;
  parameter.id = qstrToStd(object.value(QStringLiteral("id")).toString());
  const auto effect_value = decodeEffectValue(object.value(QStringLiteral("value")),
                                              (std::string(context) + ".value").c_str());
  if (!effect_value) {
    return fail<edit::EffectParameter>(effect_value.error().message);
  }
  parameter.value = effect_value.value();
  const QJsonArray keyframes = object.value(QStringLiteral("keyframes")).toArray();
  for (int index = 0; index < keyframes.size(); ++index) {
    const auto keyframe = decodeKeyframe(
        keyframes.at(index),
        (std::string(context) + ".keyframes[" + std::to_string(index) + "]").c_str());
    if (!keyframe) {
      return fail<edit::EffectParameter>(keyframe.error().message);
    }
    parameter.keyframes.push_back(keyframe.value());
  }
  return DecodeResult<edit::EffectParameter>::success(parameter);
}

[[nodiscard]] QJsonObject encodeEffect(const edit::Effect& effect) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(effect.id));
  object.insert(QStringLiteral("type"), stdToQstr(effect.type));
  object.insert(QStringLiteral("version"), static_cast<int>(effect.version));
  object.insert(QStringLiteral("enabled"), effect.enabled);
  object.insert(QStringLiteral("known"), effect.known);
  QJsonObject parameters;
  for (const auto& [key, parameter] : effect.parameters) {
    parameters.insert(stdToQstr(key), encodeEffectParameter(parameter));
  }
  object.insert(QStringLiteral("parameters"), parameters);
  if (!effect.opaque_payload.empty()) {
    object.insert(QStringLiteral("opaque_payload_base64"),
                  QString::fromLatin1(
                      QByteArray(reinterpret_cast<const char*>(effect.opaque_payload.data()),
                                 static_cast<int>(effect.opaque_payload.size()))
                          .toBase64()));
  }
  return object;
}

[[nodiscard]] DecodeResult<edit::Effect> decodeEffect(const QJsonValue& value,
                                                      const char* context) {
  if (!value.isObject()) {
    return fail<edit::Effect>(std::string(context) + ": expected effect object");
  }
  const QJsonObject object = value.toObject();
  edit::Effect effect;
  effect.id = decodeEntityIdNew(object);
  effect.type = qstrToStd(object.value(QStringLiteral("type")).toString());
  effect.version = static_cast<std::uint32_t>(object.value(QStringLiteral("version")).toInt(1));
  effect.enabled = object.value(QStringLiteral("enabled")).toBool(true);
  effect.known = object.value(QStringLiteral("known")).toBool(true);
  const QJsonObject parameters = object.value(QStringLiteral("parameters")).toObject();
  for (auto it = parameters.begin(); it != parameters.end(); ++it) {
    const auto parameter = decodeEffectParameter(
        it.value(), (std::string(context) + ".parameters." + qstrToStd(it.key())).c_str());
    if (!parameter) {
      return fail<edit::Effect>(parameter.error().message);
    }
    effect.parameters.emplace(qstrToStd(it.key()), parameter.value());
  }
  const QString payload_base64 = object.value(QStringLiteral("opaque_payload_base64")).toString();
  if (!payload_base64.isEmpty()) {
    const QByteArray decoded = QByteArray::fromBase64(payload_base64.toLatin1());
    effect.opaque_payload.assign(decoded.begin(), decoded.end());
  }
  return DecodeResult<edit::Effect>::success(effect);
}

// --- Title / Transform ---

[[nodiscard]] QJsonObject encodeTitle(const edit::Title& title) {
  QJsonObject object;
  object.insert(QStringLiteral("text"), stdToQstr(title.text));
  object.insert(QStringLiteral("font_family"), stdToQstr(title.font_family));
  object.insert(QStringLiteral("font_size"), title.font_size);
  object.insert(QStringLiteral("foreground_color"), encodeColor(title.foreground_color));
  object.insert(QStringLiteral("background_color"), encodeColor(title.background_color));
  object.insert(QStringLiteral("horizontal_alignment"),
                encodeTitleHorizontalAlignment(title.horizontal_alignment));
  object.insert(QStringLiteral("bold"), title.bold);
  object.insert(QStringLiteral("italic"), title.italic);
  return object;
}

[[nodiscard]] DecodeResult<edit::Title> decodeTitle(const QJsonValue& value,
                                                    const char* context) {
  if (!value.isObject()) {
    return fail<edit::Title>(std::string(context) + ": expected title object");
  }
  const QJsonObject object = value.toObject();
  edit::Title title;
  title.text = qstrToStd(object.value(QStringLiteral("text")).toString());
  title.font_family = qstrToStd(object.value(QStringLiteral("font_family")).toString("sans-serif"));
  title.font_size = object.value(QStringLiteral("font_size")).toDouble(96.0);
  const auto foreground = decodeColor(object.value(QStringLiteral("foreground_color")),
                                      (std::string(context) + ".foreground_color").c_str());
  if (!foreground) {
    return fail<edit::Title>(foreground.error().message);
  }
  title.foreground_color = foreground.value();
  const auto background = decodeColor(object.value(QStringLiteral("background_color")),
                                      (std::string(context) + ".background_color").c_str());
  if (!background) {
    return fail<edit::Title>(background.error().message);
  }
  title.background_color = background.value();
  const auto alignment = decodeTitleHorizontalAlignment(
      object.value(QStringLiteral("horizontal_alignment")),
      (std::string(context) + ".horizontal_alignment").c_str());
  if (!alignment) {
    return fail<edit::Title>(alignment.error().message);
  }
  title.horizontal_alignment = alignment.value();
  title.bold = object.value(QStringLiteral("bold")).toBool();
  title.italic = object.value(QStringLiteral("italic")).toBool();
  return DecodeResult<edit::Title>::success(title);
}

[[nodiscard]] QJsonObject encodeTransform(const edit::Transform& transform) {
  QJsonObject object;
  object.insert(QStringLiteral("position"), encodeVec2(transform.position));
  object.insert(QStringLiteral("scale"), encodeVec2(transform.scale));
  object.insert(QStringLiteral("rotation_degrees"), transform.rotation_degrees);
  object.insert(QStringLiteral("anchor_x"), transform.anchor_x);
  object.insert(QStringLiteral("anchor_y"), transform.anchor_y);
  object.insert(QStringLiteral("crop_left"), transform.crop_left);
  object.insert(QStringLiteral("crop_top"), transform.crop_top);
  object.insert(QStringLiteral("crop_right"), transform.crop_right);
  object.insert(QStringLiteral("crop_bottom"), transform.crop_bottom);
  object.insert(QStringLiteral("opacity"), transform.opacity);
  return object;
}

[[nodiscard]] DecodeResult<edit::Transform> decodeTransform(const QJsonValue& value,
                                                            const char* context) {
  if (!value.isObject()) {
    return fail<edit::Transform>(std::string(context) + ": expected transform object");
  }
  const QJsonObject object = value.toObject();
  edit::Transform transform;
  const auto position = decodeVec2(object.value(QStringLiteral("position")),
                                   (std::string(context) + ".position").c_str());
  if (!position) {
    return fail<edit::Transform>(position.error().message);
  }
  transform.position = position.value();
  const auto scale = decodeVec2(object.value(QStringLiteral("scale")),
                                (std::string(context) + ".scale").c_str());
  if (!scale) {
    return fail<edit::Transform>(scale.error().message);
  }
  transform.scale = scale.value();
  transform.rotation_degrees = object.value(QStringLiteral("rotation_degrees")).toDouble();
  transform.anchor_x = object.value(QStringLiteral("anchor_x")).toDouble(0.5);
  transform.anchor_y = object.value(QStringLiteral("anchor_y")).toDouble(0.5);
  transform.crop_left = object.value(QStringLiteral("crop_left")).toDouble();
  transform.crop_top = object.value(QStringLiteral("crop_top")).toDouble();
  transform.crop_right = object.value(QStringLiteral("crop_right")).toDouble();
  transform.crop_bottom = object.value(QStringLiteral("crop_bottom")).toDouble();
  transform.opacity = object.value(QStringLiteral("opacity")).toDouble(1.0);
  return DecodeResult<edit::Transform>::success(transform);
}

// --- Clip / Track ---

[[nodiscard]] QJsonObject encodeClip(const edit::Clip& clip) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(clip.id));
  object.insert(QStringLiteral("asset_id"), encodeEntityId(clip.asset_id));
  object.insert(QStringLiteral("kind"), encodeClipKind(clip.kind));
  object.insert(QStringLiteral("name"), stdToQstr(clip.name));
  object.insert(QStringLiteral("timeline_range"), encodeTimeRange(clip.timeline_range));
  object.insert(QStringLiteral("source_range"), encodeTimeRange(clip.source_range));
  object.insert(QStringLiteral("playback_rate"), encodeRate(clip.playback_rate));
  object.insert(QStringLiteral("reversed"), clip.reversed);
  insertOptionalEntityId(object, QStringLiteral("linked_group"), clip.linked_group);
  object.insert(QStringLiteral("transform"), encodeTransform(clip.transform));
  object.insert(QStringLiteral("blend_mode"), encodeBlendMode(clip.blend_mode));
  object.insert(QStringLiteral("audio_gain_db"), clip.audio_gain_db);
  object.insert(QStringLiteral("audio_pan"), clip.audio_pan);
  object.insert(QStringLiteral("fade_in"), encodeTime(clip.fade_in));
  object.insert(QStringLiteral("fade_out"), encodeTime(clip.fade_out));
  QJsonArray effects;
  for (const edit::Effect& effect : clip.effects) {
    effects.append(encodeEffect(effect));
  }
  object.insert(QStringLiteral("effects"), effects);
  if (clip.title.has_value()) {
    object.insert(QStringLiteral("title"), encodeTitle(*clip.title));
  }
  insertOptionalEntityId(object, QStringLiteral("nested_sequence_id"), clip.nested_sequence_id);
  return object;
}

[[nodiscard]] DecodeResult<edit::Clip> decodeClip(const QJsonValue& value, const char* context) {
  if (!value.isObject()) {
    return fail<edit::Clip>(std::string(context) + ": expected clip object");
  }
  const QJsonObject object = value.toObject();
  edit::Clip clip;
  clip.id = decodeEntityIdNew(object);
  const auto asset_id = decodeEntityIdRequired(object.value(QStringLiteral("asset_id")),
                                               (std::string(context) + ".asset_id").c_str());
  if (!asset_id) {
    return fail<edit::Clip>(asset_id.error().message);
  }
  clip.asset_id = asset_id.value();
  const auto kind = decodeClipKind(object.value(QStringLiteral("kind")),
                                   (std::string(context) + ".kind").c_str());
  if (!kind) {
    return fail<edit::Clip>(kind.error().message);
  }
  clip.kind = kind.value();
  clip.name = qstrToStd(object.value(QStringLiteral("name")).toString());
  const auto timeline_range = decodeTimeRange(object.value(QStringLiteral("timeline_range")),
                                              (std::string(context) + ".timeline_range").c_str());
  if (!timeline_range) {
    return fail<edit::Clip>(timeline_range.error().message);
  }
  clip.timeline_range = timeline_range.value();
  const auto source_range = decodeTimeRange(object.value(QStringLiteral("source_range")),
                                            (std::string(context) + ".source_range").c_str());
  if (!source_range) {
    return fail<edit::Clip>(source_range.error().message);
  }
  clip.source_range = source_range.value();
  const auto playback_rate = decodeRate(object.value(QStringLiteral("playback_rate")),
                                        (std::string(context) + ".playback_rate").c_str());
  if (!playback_rate) {
    return fail<edit::Clip>(playback_rate.error().message);
  }
  clip.playback_rate = playback_rate.value();
  clip.reversed = object.value(QStringLiteral("reversed")).toBool();
  const auto linked_group = decodeOptionalEntityId(
      object.value(QStringLiteral("linked_group")),
      (std::string(context) + ".linked_group").c_str());
  if (!linked_group) {
    return fail<edit::Clip>(linked_group.error().message);
  }
  clip.linked_group = linked_group.value();
  const auto transform = decodeTransform(object.value(QStringLiteral("transform")),
                                       (std::string(context) + ".transform").c_str());
  if (!transform) {
    return fail<edit::Clip>(transform.error().message);
  }
  clip.transform = transform.value();
  const auto blend_mode = decodeBlendMode(object.value(QStringLiteral("blend_mode")),
                                          (std::string(context) + ".blend_mode").c_str());
  if (!blend_mode) {
    return fail<edit::Clip>(blend_mode.error().message);
  }
  clip.blend_mode = blend_mode.value();
  clip.audio_gain_db = object.value(QStringLiteral("audio_gain_db")).toDouble();
  clip.audio_pan = object.value(QStringLiteral("audio_pan")).toDouble();
  const auto fade_in = decodeTime(object.value(QStringLiteral("fade_in")),
                                  (std::string(context) + ".fade_in").c_str());
  if (!fade_in) {
    return fail<edit::Clip>(fade_in.error().message);
  }
  clip.fade_in = fade_in.value();
  const auto fade_out = decodeTime(object.value(QStringLiteral("fade_out")),
                                   (std::string(context) + ".fade_out").c_str());
  if (!fade_out) {
    return fail<edit::Clip>(fade_out.error().message);
  }
  clip.fade_out = fade_out.value();
  const QJsonArray effects = object.value(QStringLiteral("effects")).toArray();
  for (int index = 0; index < effects.size(); ++index) {
    const auto effect = decodeEffect(
        effects.at(index),
        (std::string(context) + ".effects[" + std::to_string(index) + "]").c_str());
    if (!effect) {
      return fail<edit::Clip>(effect.error().message);
    }
    clip.effects.push_back(effect.value());
  }
  if (object.contains(QStringLiteral("title"))) {
    const auto title = decodeTitle(object.value(QStringLiteral("title")),
                                   (std::string(context) + ".title").c_str());
    if (!title) {
      return fail<edit::Clip>(title.error().message);
    }
    clip.title = title.value();
  }
  const auto nested_sequence_id = decodeOptionalEntityId(
      object.value(QStringLiteral("nested_sequence_id")),
      (std::string(context) + ".nested_sequence_id").c_str());
  if (!nested_sequence_id) {
    return fail<edit::Clip>(nested_sequence_id.error().message);
  }
  clip.nested_sequence_id = nested_sequence_id.value();
  return DecodeResult<edit::Clip>::success(clip);
}

[[nodiscard]] QJsonObject encodeTrack(const edit::Track& track) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(track.id));
  object.insert(QStringLiteral("kind"), encodeTrackKind(track.kind));
  object.insert(QStringLiteral("name"), stdToQstr(track.name));
  object.insert(QStringLiteral("locked"), track.locked);
  object.insert(QStringLiteral("muted"), track.muted);
  object.insert(QStringLiteral("solo"), track.solo);
  object.insert(QStringLiteral("visible"), track.visible);
  object.insert(QStringLiteral("targeted"), track.targeted);
  QJsonArray clips;
  for (const edit::Clip& clip : track.clips) {
    clips.append(encodeClip(clip));
  }
  object.insert(QStringLiteral("clips"), clips);
  QJsonArray effects;
  for (const edit::Effect& effect : track.effects) {
    effects.append(encodeEffect(effect));
  }
  object.insert(QStringLiteral("effects"), effects);
  object.insert(QStringLiteral("audio_gain_db"), track.audio_gain_db);
  object.insert(QStringLiteral("audio_pan"), track.audio_pan);
  return object;
}

[[nodiscard]] DecodeResult<edit::Track> decodeTrack(const QJsonValue& value, const char* context) {
  if (!value.isObject()) {
    return fail<edit::Track>(std::string(context) + ": expected track object");
  }
  const QJsonObject object = value.toObject();
  edit::Track track;
  track.id = decodeEntityIdNew(object);
  const auto kind = decodeTrackKind(object.value(QStringLiteral("kind")),
                                    (std::string(context) + ".kind").c_str());
  if (!kind) {
    return fail<edit::Track>(kind.error().message);
  }
  track.kind = kind.value();
  track.name = qstrToStd(object.value(QStringLiteral("name")).toString());
  track.locked = object.value(QStringLiteral("locked")).toBool();
  track.muted = object.value(QStringLiteral("muted")).toBool();
  track.solo = object.value(QStringLiteral("solo")).toBool();
  track.visible = object.value(QStringLiteral("visible")).toBool(true);
  track.targeted = object.value(QStringLiteral("targeted")).toBool(true);
  const QJsonArray clips = object.value(QStringLiteral("clips")).toArray();
  for (int index = 0; index < clips.size(); ++index) {
    const auto clip = decodeClip(
        clips.at(index),
        (std::string(context) + ".clips[" + std::to_string(index) + "]").c_str());
    if (!clip) {
      return fail<edit::Track>(clip.error().message);
    }
    track.clips.push_back(clip.value());
  }
  const QJsonArray effects = object.value(QStringLiteral("effects")).toArray();
  for (int index = 0; index < effects.size(); ++index) {
    const auto effect = decodeEffect(
        effects.at(index),
        (std::string(context) + ".effects[" + std::to_string(index) + "]").c_str());
    if (!effect) {
      return fail<edit::Track>(effect.error().message);
    }
    track.effects.push_back(effect.value());
  }
  track.audio_gain_db = object.value(QStringLiteral("audio_gain_db")).toDouble();
  track.audio_pan = object.value(QStringLiteral("audio_pan")).toDouble();
  return DecodeResult<edit::Track>::success(track);
}

// --- Marker / Caption / Transition ---

[[nodiscard]] QJsonObject encodeMarker(const edit::Marker& marker) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(marker.id));
  object.insert(QStringLiteral("range"), encodeTimeRange(marker.range));
  object.insert(QStringLiteral("label"), stdToQstr(marker.label));
  object.insert(QStringLiteral("color"), encodeColor(marker.color));
  return object;
}

[[nodiscard]] DecodeResult<edit::Marker> decodeMarker(const QJsonValue& value,
                                                      const char* context) {
  if (!value.isObject()) {
    return fail<edit::Marker>(std::string(context) + ": expected marker object");
  }
  const QJsonObject object = value.toObject();
  edit::Marker marker;
  marker.id = decodeEntityIdNew(object);
  const auto range = decodeTimeRange(object.value(QStringLiteral("range")),
                                     (std::string(context) + ".range").c_str());
  if (!range) {
    return fail<edit::Marker>(range.error().message);
  }
  marker.range = range.value();
  marker.label = qstrToStd(object.value(QStringLiteral("label")).toString());
  const auto color = decodeColor(object.value(QStringLiteral("color")),
                                 (std::string(context) + ".color").c_str());
  if (!color) {
    return fail<edit::Marker>(color.error().message);
  }
  marker.color = color.value();
  return DecodeResult<edit::Marker>::success(marker);
}

[[nodiscard]] QJsonObject encodeCaptionStyle(const edit::CaptionStyle& style) {
  QJsonObject object;
  object.insert(QStringLiteral("font_family"), stdToQstr(style.font_family));
  object.insert(QStringLiteral("font_size"), style.font_size);
  object.insert(QStringLiteral("text_color"), encodeColor(style.text_color));
  object.insert(QStringLiteral("background_color"), encodeColor(style.background_color));
  object.insert(QStringLiteral("bold"), style.bold);
  object.insert(QStringLiteral("italic"), style.italic);
  object.insert(QStringLiteral("alignment"), encodeCaptionAlignment(style.alignment));
  object.insert(QStringLiteral("vertical_position"), style.vertical_position);
  object.insert(QStringLiteral("safe_margin"), style.safe_margin);
  object.insert(QStringLiteral("outline_width"), style.outline_width);
  object.insert(QStringLiteral("outline_color"), encodeColor(style.outline_color));
  return object;
}

[[nodiscard]] DecodeResult<edit::CaptionStyle> decodeCaptionStyle(const QJsonValue& value,
                                                                const char* context) {
  if (!value.isObject()) {
    return fail<edit::CaptionStyle>(std::string(context) + ": expected caption style object");
  }
  const QJsonObject object = value.toObject();
  edit::CaptionStyle style;
  style.font_family = qstrToStd(object.value(QStringLiteral("font_family")).toString("sans-serif"));
  style.font_size = object.value(QStringLiteral("font_size")).toDouble(48.0);
  const auto text_color = decodeColor(object.value(QStringLiteral("text_color")),
                                      (std::string(context) + ".text_color").c_str());
  if (!text_color) {
    return fail<edit::CaptionStyle>(text_color.error().message);
  }
  style.text_color = text_color.value();
  const auto background_color = decodeColor(object.value(QStringLiteral("background_color")),
                                            (std::string(context) + ".background_color").c_str());
  if (!background_color) {
    return fail<edit::CaptionStyle>(background_color.error().message);
  }
  style.background_color = background_color.value();
  style.bold = object.value(QStringLiteral("bold")).toBool();
  style.italic = object.value(QStringLiteral("italic")).toBool();
  const auto alignment = decodeCaptionAlignment(object.value(QStringLiteral("alignment")),
                                                (std::string(context) + ".alignment").c_str());
  if (!alignment) {
    return fail<edit::CaptionStyle>(alignment.error().message);
  }
  style.alignment = alignment.value();
  style.vertical_position = object.value(QStringLiteral("vertical_position")).toDouble(0.9);
  style.safe_margin = object.value(QStringLiteral("safe_margin")).toDouble(0.05);
  style.outline_width = object.value(QStringLiteral("outline_width")).toDouble();
  const auto outline_color = decodeColor(object.value(QStringLiteral("outline_color")),
                                         (std::string(context) + ".outline_color").c_str());
  if (!outline_color) {
    return fail<edit::CaptionStyle>(outline_color.error().message);
  }
  style.outline_color = outline_color.value();
  return DecodeResult<edit::CaptionStyle>::success(style);
}

[[nodiscard]] QJsonObject encodeCaptionWord(const edit::CaptionWord& word) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(word.id));
  object.insert(QStringLiteral("text"), stdToQstr(word.text));
  object.insert(QStringLiteral("range"), encodeTimeRange(word.range));
  object.insert(QStringLiteral("probability"), word.probability);
  return object;
}

[[nodiscard]] DecodeResult<edit::CaptionWord> decodeCaptionWord(const QJsonValue& value,
                                                              const char* context) {
  if (!value.isObject()) {
    return fail<edit::CaptionWord>(std::string(context) + ": expected caption word object");
  }
  const QJsonObject object = value.toObject();
  edit::CaptionWord word;
  word.id = decodeEntityIdNew(object);
  word.text = qstrToStd(object.value(QStringLiteral("text")).toString());
  const auto range = decodeTimeRange(object.value(QStringLiteral("range")),
                                     (std::string(context) + ".range").c_str());
  if (!range) {
    return fail<edit::CaptionWord>(range.error().message);
  }
  word.range = range.value();
  word.probability = object.value(QStringLiteral("probability")).toDouble(1.0);
  return DecodeResult<edit::CaptionWord>::success(word);
}

[[nodiscard]] QJsonObject encodeCaption(const edit::Caption& caption) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(caption.id));
  object.insert(QStringLiteral("range"), encodeTimeRange(caption.range));
  object.insert(QStringLiteral("text"), stdToQstr(caption.text));
  object.insert(QStringLiteral("language"), stdToQstr(caption.language));
  object.insert(QStringLiteral("style"), encodeCaptionStyle(caption.style));
  QJsonObject provenance;
  provenance.insert(QStringLiteral("source"),
                    encodeCaptionWordSource(caption.provenance.source));
  provenance.insert(QStringLiteral("model_identity"),
                    stdToQstr(caption.provenance.model_identity));
  object.insert(QStringLiteral("provenance"), provenance);
  QJsonArray words;
  for (const edit::CaptionWord& word : caption.words) {
    words.append(encodeCaptionWord(word));
  }
  object.insert(QStringLiteral("words"), words);
  return object;
}

[[nodiscard]] DecodeResult<edit::Caption> decodeCaption(const QJsonValue& value,
                                                        const char* context) {
  if (!value.isObject()) {
    return fail<edit::Caption>(std::string(context) + ": expected caption object");
  }
  const QJsonObject object = value.toObject();
  edit::Caption caption;
  caption.id = decodeEntityIdNew(object);
  const auto range = decodeTimeRange(object.value(QStringLiteral("range")),
                                     (std::string(context) + ".range").c_str());
  if (!range) {
    return fail<edit::Caption>(range.error().message);
  }
  caption.range = range.value();
  caption.text = qstrToStd(object.value(QStringLiteral("text")).toString());
  caption.language = qstrToStd(object.value(QStringLiteral("language")).toString());
  const auto style = decodeCaptionStyle(object.value(QStringLiteral("style")),
                                        (std::string(context) + ".style").c_str());
  if (!style) {
    return fail<edit::Caption>(style.error().message);
  }
  caption.style = style.value();
  const QJsonObject provenance = object.value(QStringLiteral("provenance")).toObject();
  const auto source = decodeCaptionWordSource(
      provenance.value(QStringLiteral("source")),
      (std::string(context) + ".provenance.source").c_str());
  if (!source) {
    return fail<edit::Caption>(source.error().message);
  }
  caption.provenance.source = source.value();
  caption.provenance.model_identity =
      qstrToStd(provenance.value(QStringLiteral("model_identity")).toString());
  const QJsonArray words = object.value(QStringLiteral("words")).toArray();
  for (int index = 0; index < words.size(); ++index) {
    const auto word = decodeCaptionWord(
        words.at(index),
        (std::string(context) + ".words[" + std::to_string(index) + "]").c_str());
    if (!word) {
      return fail<edit::Caption>(word.error().message);
    }
    caption.words.push_back(word.value());
  }
  return DecodeResult<edit::Caption>::success(caption);
}

[[nodiscard]] QJsonObject encodeTransition(const edit::Transition& transition) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(transition.id));
  object.insert(QStringLiteral("outgoing_clip_id"), encodeEntityId(transition.outgoing_clip_id));
  object.insert(QStringLiteral("incoming_clip_id"), encodeEntityId(transition.incoming_clip_id));
  object.insert(QStringLiteral("range"), encodeTimeRange(transition.range));
  object.insert(QStringLiteral("kind"), encodeTransitionKind(transition.kind));
  object.insert(QStringLiteral("enabled"), transition.enabled);
  return object;
}

[[nodiscard]] DecodeResult<edit::Transition> decodeTransition(const QJsonValue& value,
                                                              const char* context) {
  if (!value.isObject()) {
    return fail<edit::Transition>(std::string(context) + ": expected transition object");
  }
  const QJsonObject object = value.toObject();
  edit::Transition transition;
  transition.id = decodeEntityIdNew(object);
  const auto outgoing_clip_id = decodeEntityIdRequired(
      object.value(QStringLiteral("outgoing_clip_id")),
      (std::string(context) + ".outgoing_clip_id").c_str());
  if (!outgoing_clip_id) {
    return fail<edit::Transition>(outgoing_clip_id.error().message);
  }
  transition.outgoing_clip_id = outgoing_clip_id.value();
  const auto incoming_clip_id = decodeEntityIdRequired(
      object.value(QStringLiteral("incoming_clip_id")),
      (std::string(context) + ".incoming_clip_id").c_str());
  if (!incoming_clip_id) {
    return fail<edit::Transition>(incoming_clip_id.error().message);
  }
  transition.incoming_clip_id = incoming_clip_id.value();
  const auto range = decodeTimeRange(object.value(QStringLiteral("range")),
                                     (std::string(context) + ".range").c_str());
  if (!range) {
    return fail<edit::Transition>(range.error().message);
  }
  transition.range = range.value();
  const auto kind = decodeTransitionKind(object.value(QStringLiteral("kind")),
                                         (std::string(context) + ".kind").c_str());
  if (!kind) {
    return fail<edit::Transition>(kind.error().message);
  }
  transition.kind = kind.value();
  transition.enabled = object.value(QStringLiteral("enabled")).toBool(true);
  return DecodeResult<edit::Transition>::success(transition);
}

// --- Asset / Sequence / MediaBin / SmartQuery ---

[[nodiscard]] QJsonObject encodeAsset(const edit::Asset& asset) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(asset.id));
  object.insert(QStringLiteral("name"), stdToQstr(asset.name));
  object.insert(QStringLiteral("source_uri"), stdToQstr(asset.source_uri));
  object.insert(QStringLiteral("fingerprint"), stdToQstr(asset.fingerprint));
  object.insert(QStringLiteral("duration"), encodeTime(asset.duration));
  object.insert(QStringLiteral("has_video"), asset.has_video);
  object.insert(QStringLiteral("has_audio"), asset.has_audio);
  object.insert(QStringLiteral("width"), static_cast<int>(asset.width));
  object.insert(QStringLiteral("height"), static_cast<int>(asset.height));
  if (asset.nominal_frame_rate.has_value()) {
    object.insert(QStringLiteral("nominal_frame_rate"), encodeRate(*asset.nominal_frame_rate));
  }
  object.insert(QStringLiteral("audio_sample_rate"), static_cast<int>(asset.audio_sample_rate));
  object.insert(QStringLiteral("audio_channels"), static_cast<int>(asset.audio_channels));
  object.insert(QStringLiteral("metadata"), encodeStringMap(asset.metadata));
  insertOptionalEntityId(object, QStringLiteral("bin_id"), asset.bin_id);
  object.insert(QStringLiteral("display_title"), stdToQstr(asset.display_title));
  QJsonArray tags;
  for (const std::string& tag : asset.tags) {
    tags.append(stdToQstr(tag));
  }
  object.insert(QStringLiteral("tags"), tags);
  object.insert(QStringLiteral("notes"), stdToQstr(asset.notes));
  object.insert(QStringLiteral("rating"), asset.rating);
  return object;
}

[[nodiscard]] DecodeResult<edit::Asset> decodeAsset(const QJsonValue& value, const char* context) {
  if (!value.isObject()) {
    return fail<edit::Asset>(std::string(context) + ": expected asset object");
  }
  const QJsonObject object = value.toObject();
  edit::Asset asset;
  asset.id = decodeEntityIdNew(object);
  asset.name = qstrToStd(object.value(QStringLiteral("name")).toString());
  asset.source_uri = qstrToStd(object.value(QStringLiteral("source_uri")).toString());
  asset.fingerprint = qstrToStd(object.value(QStringLiteral("fingerprint")).toString());
  const auto duration = decodeTime(object.value(QStringLiteral("duration")),
                                 (std::string(context) + ".duration").c_str());
  if (!duration) {
    return fail<edit::Asset>(duration.error().message);
  }
  asset.duration = duration.value();
  asset.has_video = object.value(QStringLiteral("has_video")).toBool();
  asset.has_audio = object.value(QStringLiteral("has_audio")).toBool();
  asset.width = static_cast<std::uint32_t>(object.value(QStringLiteral("width")).toInt());
  asset.height = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt());
  if (object.contains(QStringLiteral("nominal_frame_rate"))) {
    const auto rate = decodeRate(object.value(QStringLiteral("nominal_frame_rate")),
                                 (std::string(context) + ".nominal_frame_rate").c_str());
    if (!rate) {
      return fail<edit::Asset>(rate.error().message);
    }
    asset.nominal_frame_rate = rate.value();
  }
  asset.audio_sample_rate =
      static_cast<std::uint32_t>(object.value(QStringLiteral("audio_sample_rate")).toInt());
  asset.audio_channels =
      static_cast<std::uint32_t>(object.value(QStringLiteral("audio_channels")).toInt());
  const auto metadata = decodeStringMap(object.value(QStringLiteral("metadata")),
                                        (std::string(context) + ".metadata").c_str());
  if (!metadata) {
    return fail<edit::Asset>(metadata.error().message);
  }
  asset.metadata = metadata.value();
  const auto bin_id = decodeOptionalEntityId(object.value(QStringLiteral("bin_id")),
                                             (std::string(context) + ".bin_id").c_str());
  if (!bin_id) {
    return fail<edit::Asset>(bin_id.error().message);
  }
  asset.bin_id = bin_id.value();
  asset.display_title = qstrToStd(object.value(QStringLiteral("display_title")).toString());
  const QJsonArray tags = object.value(QStringLiteral("tags")).toArray();
  for (const QJsonValue& tag : tags) {
    asset.tags.push_back(qstrToStd(tag.toString()));
  }
  asset.notes = qstrToStd(object.value(QStringLiteral("notes")).toString());
  asset.rating = object.value(QStringLiteral("rating")).toInt();
  return DecodeResult<edit::Asset>::success(asset);
}

[[nodiscard]] QJsonObject encodeSequence(const edit::Sequence& sequence) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(sequence.id));
  object.insert(QStringLiteral("name"), stdToQstr(sequence.name));
  object.insert(QStringLiteral("frame_rate"), encodeRate(sequence.frame_rate));
  object.insert(QStringLiteral("width"), static_cast<int>(sequence.width));
  object.insert(QStringLiteral("height"), static_cast<int>(sequence.height));
  object.insert(QStringLiteral("audio_sample_rate"), static_cast<int>(sequence.audio_sample_rate));
  QJsonArray tracks;
  for (const edit::Track& track : sequence.tracks) {
    tracks.append(encodeTrack(track));
  }
  object.insert(QStringLiteral("tracks"), tracks);
  QJsonArray markers;
  for (const edit::Marker& marker : sequence.markers) {
    markers.append(encodeMarker(marker));
  }
  object.insert(QStringLiteral("markers"), markers);
  QJsonArray captions;
  for (const edit::Caption& caption : sequence.captions) {
    captions.append(encodeCaption(caption));
  }
  object.insert(QStringLiteral("captions"), captions);
  QJsonArray transitions;
  for (const edit::Transition& transition : sequence.transitions) {
    transitions.append(encodeTransition(transition));
  }
  object.insert(QStringLiteral("transitions"), transitions);
  return object;
}

[[nodiscard]] DecodeResult<edit::Sequence> decodeSequence(const QJsonValue& value,
                                                          const char* context) {
  if (!value.isObject()) {
    return fail<edit::Sequence>(std::string(context) + ": expected sequence object");
  }
  const QJsonObject object = value.toObject();
  edit::Sequence sequence;
  sequence.id = decodeEntityIdNew(object);
  sequence.name = qstrToStd(object.value(QStringLiteral("name")).toString());
  const auto frame_rate = decodeRate(object.value(QStringLiteral("frame_rate")),
                                     (std::string(context) + ".frame_rate").c_str());
  if (!frame_rate) {
    return fail<edit::Sequence>(frame_rate.error().message);
  }
  sequence.frame_rate = frame_rate.value();
  sequence.width = static_cast<std::uint32_t>(object.value(QStringLiteral("width")).toInt(1920));
  sequence.height = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt(1080));
  sequence.audio_sample_rate =
      static_cast<std::uint32_t>(object.value(QStringLiteral("audio_sample_rate")).toInt(48'000));
  const QJsonArray tracks = object.value(QStringLiteral("tracks")).toArray();
  for (int index = 0; index < tracks.size(); ++index) {
    const auto track = decodeTrack(
        tracks.at(index),
        (std::string(context) + ".tracks[" + std::to_string(index) + "]").c_str());
    if (!track) {
      return fail<edit::Sequence>(track.error().message);
    }
    sequence.tracks.push_back(track.value());
  }
  const QJsonArray markers = object.value(QStringLiteral("markers")).toArray();
  for (int index = 0; index < markers.size(); ++index) {
    const auto marker = decodeMarker(
        markers.at(index),
        (std::string(context) + ".markers[" + std::to_string(index) + "]").c_str());
    if (!marker) {
      return fail<edit::Sequence>(marker.error().message);
    }
    sequence.markers.push_back(marker.value());
  }
  const QJsonArray captions = object.value(QStringLiteral("captions")).toArray();
  for (int index = 0; index < captions.size(); ++index) {
    const auto caption = decodeCaption(
        captions.at(index),
        (std::string(context) + ".captions[" + std::to_string(index) + "]").c_str());
    if (!caption) {
      return fail<edit::Sequence>(caption.error().message);
    }
    sequence.captions.push_back(caption.value());
  }
  const QJsonArray transitions = object.value(QStringLiteral("transitions")).toArray();
  for (int index = 0; index < transitions.size(); ++index) {
    const auto transition = decodeTransition(
        transitions.at(index),
        (std::string(context) + ".transitions[" + std::to_string(index) + "]").c_str());
    if (!transition) {
      return fail<edit::Sequence>(transition.error().message);
    }
    sequence.transitions.push_back(transition.value());
  }
  return DecodeResult<edit::Sequence>::success(sequence);
}

[[nodiscard]] QJsonObject encodeSmartQuery(const edit::SmartQuery& query) {
  QJsonObject object;
  QJsonArray tags;
  for (const std::string& tag : query.tags) {
    tags.append(stdToQstr(tag));
  }
  object.insert(QStringLiteral("tags"), tags);
  if (query.min_rating.has_value()) {
    object.insert(QStringLiteral("min_rating"), *query.min_rating);
  }
  if (query.notes_contains.has_value()) {
    object.insert(QStringLiteral("notes_contains"), stdToQstr(*query.notes_contains));
  }
  if (query.has_video.has_value()) {
    object.insert(QStringLiteral("has_video"), *query.has_video);
  }
  if (query.has_audio.has_value()) {
    object.insert(QStringLiteral("has_audio"), *query.has_audio);
  }
  if (query.name_contains.has_value()) {
    object.insert(QStringLiteral("name_contains"), stdToQstr(*query.name_contains));
  }
  return object;
}

[[nodiscard]] DecodeResult<edit::SmartQuery> decodeSmartQuery(const QJsonValue& value,
                                                              const char* context) {
  if (!value.isObject()) {
    return fail<edit::SmartQuery>(std::string(context) + ": expected smart query object");
  }
  const QJsonObject object = value.toObject();
  edit::SmartQuery query;
  const QJsonArray tags = object.value(QStringLiteral("tags")).toArray();
  for (const QJsonValue& tag : tags) {
    query.tags.push_back(qstrToStd(tag.toString()));
  }
  if (object.contains(QStringLiteral("min_rating"))) {
    query.min_rating = object.value(QStringLiteral("min_rating")).toInt();
  }
  if (object.contains(QStringLiteral("notes_contains"))) {
    query.notes_contains = qstrToStd(object.value(QStringLiteral("notes_contains")).toString());
  }
  if (object.contains(QStringLiteral("has_video"))) {
    query.has_video = object.value(QStringLiteral("has_video")).toBool();
  }
  if (object.contains(QStringLiteral("has_audio"))) {
    query.has_audio = object.value(QStringLiteral("has_audio")).toBool();
  }
  if (object.contains(QStringLiteral("name_contains"))) {
    query.name_contains = qstrToStd(object.value(QStringLiteral("name_contains")).toString());
  }
  return DecodeResult<edit::SmartQuery>::success(query);
}

[[nodiscard]] QJsonObject encodeMediaBin(const edit::MediaBin& bin) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), encodeEntityId(bin.id));
  object.insert(QStringLiteral("name"), stdToQstr(bin.name));
  insertOptionalEntityId(object, QStringLiteral("parent_id"), bin.parent_id);
  object.insert(QStringLiteral("kind"), encodeMediaBinKind(bin.kind));
  object.insert(QStringLiteral("query"), encodeSmartQuery(bin.query));
  return object;
}

[[nodiscard]] DecodeResult<edit::MediaBin> decodeMediaBin(const QJsonValue& value,
                                                        const char* context) {
  if (!value.isObject()) {
    return fail<edit::MediaBin>(std::string(context) + ": expected media bin object");
  }
  const QJsonObject object = value.toObject();
  edit::MediaBin bin;
  bin.id = decodeEntityIdNew(object);
  bin.name = qstrToStd(object.value(QStringLiteral("name")).toString());
  const auto parent_id = decodeOptionalEntityId(object.value(QStringLiteral("parent_id")),
                                              (std::string(context) + ".parent_id").c_str());
  if (!parent_id) {
    return fail<edit::MediaBin>(parent_id.error().message);
  }
  bin.parent_id = parent_id.value();
  const auto kind = decodeMediaBinKind(object.value(QStringLiteral("kind")),
                                       (std::string(context) + ".kind").c_str());
  if (!kind) {
    return fail<edit::MediaBin>(kind.error().message);
  }
  bin.kind = kind.value();
  const auto query = decodeSmartQuery(object.value(QStringLiteral("query")),
                                      (std::string(context) + ".query").c_str());
  if (!query) {
    return fail<edit::MediaBin>(query.error().message);
  }
  bin.query = query.value();
  return DecodeResult<edit::MediaBin>::success(bin);
}

// --- Command operation encode/decode ---

void encodeOperationFields(QJsonObject& object, const EditOperation& operation) {
  std::visit(
      [&object](const auto& command) {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, edit::AddAssetCommand>) {
          object.insert(QStringLiteral("asset"), encodeAsset(command.asset));
        } else if constexpr (std::is_same_v<T, edit::RemoveAssetCommand>) {
          object.insert(QStringLiteral("asset_id"), encodeEntityId(command.asset_id));
        } else if constexpr (std::is_same_v<T, edit::RelinkAssetCommand>) {
          object.insert(QStringLiteral("asset_id"), encodeEntityId(command.asset_id));
          object.insert(QStringLiteral("source_uri"), stdToQstr(command.source_uri));
          object.insert(QStringLiteral("fingerprint"), stdToQstr(command.fingerprint));
          object.insert(QStringLiteral("duration"), encodeTime(command.duration));
          object.insert(QStringLiteral("has_video"), command.has_video);
          object.insert(QStringLiteral("has_audio"), command.has_audio);
          object.insert(QStringLiteral("width"), static_cast<int>(command.width));
          object.insert(QStringLiteral("height"), static_cast<int>(command.height));
          if (command.nominal_frame_rate.has_value()) {
            object.insert(QStringLiteral("nominal_frame_rate"),
                          encodeRate(*command.nominal_frame_rate));
          }
          object.insert(QStringLiteral("audio_sample_rate"),
                        static_cast<int>(command.audio_sample_rate));
          object.insert(QStringLiteral("audio_channels"), static_cast<int>(command.audio_channels));
          object.insert(QStringLiteral("metadata"), encodeStringMap(command.metadata));
        } else if constexpr (std::is_same_v<T, edit::AddSequenceCommand>) {
          object.insert(QStringLiteral("sequence"), encodeSequence(command.sequence));
        } else if constexpr (std::is_same_v<T, edit::RemoveSequenceCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
        } else if constexpr (std::is_same_v<T, edit::SetSequenceFormatCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("frame_rate"), encodeRate(command.frame_rate));
          object.insert(QStringLiteral("width"), static_cast<int>(command.width));
          object.insert(QStringLiteral("height"), static_cast<int>(command.height));
        } else if constexpr (std::is_same_v<T, edit::AddTrackCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track"), encodeTrack(command.track));
          if (command.index.has_value()) {
            object.insert(QStringLiteral("index"), static_cast<qint64>(*command.index));
          }
        } else if constexpr (std::is_same_v<T, edit::RemoveTrackCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
        } else if constexpr (std::is_same_v<T, edit::RenameTrackCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("name"), stdToQstr(command.name));
        } else if constexpr (std::is_same_v<T, edit::ReorderTrackCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("index"), static_cast<qint64>(command.index));
        } else if constexpr (std::is_same_v<T, edit::SetTrackLockedCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("locked"), command.locked);
        } else if constexpr (std::is_same_v<T, edit::SetTrackVisibilityCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("visible"), command.visible);
        } else if constexpr (std::is_same_v<T, edit::SetTrackTargetedCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("targeted"), command.targeted);
        } else if constexpr (std::is_same_v<T, edit::InsertClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("clip"), encodeClip(command.clip));
          object.insert(QStringLiteral("mode"), encodeInsertMode(command.mode));
        } else if constexpr (std::is_same_v<T, edit::MoveClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("destination_track_id"),
                        encodeEntityId(command.destination_track_id));
          object.insert(QStringLiteral("new_start"), encodeTime(command.new_start));
          object.insert(QStringLiteral("mode"), encodeInsertMode(command.mode));
          object.insert(QStringLiteral("include_linked"), command.include_linked);
        } else if constexpr (std::is_same_v<T, edit::TrimClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("timeline_range"), encodeTimeRange(command.timeline_range));
          object.insert(QStringLiteral("source_range"), encodeTimeRange(command.source_range));
          object.insert(QStringLiteral("include_linked"), command.include_linked);
          object.insert(QStringLiteral("mode"), encodeInsertMode(command.mode));
        } else if constexpr (std::is_same_v<T, edit::SplitClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("split_time"), encodeTime(command.split_time));
          object.insert(QStringLiteral("right_clip_id"), encodeEntityId(command.right_clip_id));
          object.insert(QStringLiteral("include_linked"), command.include_linked);
          QJsonArray linked;
          for (const edit::LinkedSplitId& entry : command.linked_right_clip_ids) {
            QJsonObject item;
            item.insert(QStringLiteral("clip_id"), encodeEntityId(entry.clip_id));
            item.insert(QStringLiteral("right_clip_id"), encodeEntityId(entry.right_clip_id));
            linked.append(item);
          }
          object.insert(QStringLiteral("linked_right_clip_ids"), linked);
        } else if constexpr (std::is_same_v<T, edit::RemoveClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("ripple"), command.ripple);
          object.insert(QStringLiteral("include_linked"), command.include_linked);
        } else if constexpr (std::is_same_v<T, edit::CloseGapCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("gap"), encodeTimeRange(command.gap));
        } else if constexpr (std::is_same_v<T, edit::RollEditCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("left_clip_id"), encodeEntityId(command.left_clip_id));
          object.insert(QStringLiteral("right_clip_id"), encodeEntityId(command.right_clip_id));
          object.insert(QStringLiteral("new_cut_time"), encodeTime(command.new_cut_time));
        } else if constexpr (std::is_same_v<T, edit::SlipClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("new_source_start"), encodeTime(command.new_source_start));
          object.insert(QStringLiteral("include_linked"), command.include_linked);
        } else if constexpr (std::is_same_v<T, edit::SlideClipCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("new_start"), encodeTime(command.new_start));
        } else if constexpr (std::is_same_v<T, edit::AddMarkerCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("marker"), encodeMarker(command.marker));
        } else if constexpr (std::is_same_v<T, edit::UpdateMarkerCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("marker"), encodeMarker(command.marker));
        } else if constexpr (std::is_same_v<T, edit::RemoveMarkerCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("marker_id"), encodeEntityId(command.marker_id));
        } else if constexpr (std::is_same_v<T, edit::AddCaptionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("caption"), encodeCaption(command.caption));
        } else if constexpr (std::is_same_v<T, edit::UpdateCaptionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("caption"), encodeCaption(command.caption));
        } else if constexpr (std::is_same_v<T, edit::RemoveCaptionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("caption_id"), encodeEntityId(command.caption_id));
        } else if constexpr (std::is_same_v<T, edit::ApplyCaptionChangeSetCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          QJsonArray added;
          for (const edit::Caption& caption : command.added) {
            added.append(encodeCaption(caption));
          }
          object.insert(QStringLiteral("added"), added);
          QJsonArray updated;
          for (const edit::Caption& caption : command.updated) {
            updated.append(encodeCaption(caption));
          }
          object.insert(QStringLiteral("updated"), updated);
          QJsonArray removed;
          for (const edit::EntityId& id : command.removed) {
            removed.append(encodeEntityId(id));
          }
          object.insert(QStringLiteral("removed"), removed);
        } else if constexpr (std::is_same_v<T, edit::ApplyTimelineCutChangeSetCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          QJsonArray tracks;
          for (const edit::TrackClipReplacement& replacement : command.tracks) {
            QJsonObject item;
            item.insert(QStringLiteral("track_id"), encodeEntityId(replacement.track_id));
            item.insert(QStringLiteral("kind"), encodeTrackKind(replacement.kind));
            QJsonArray clips;
            for (const edit::Clip& clip : replacement.clips) {
              clips.append(encodeClip(clip));
            }
            item.insert(QStringLiteral("clips"), clips);
            tracks.append(item);
          }
          object.insert(QStringLiteral("tracks"), tracks);
          if (command.transitions.has_value()) {
            QJsonArray transitions;
            for (const edit::Transition& transition : *command.transitions) {
              transitions.append(encodeTransition(transition));
            }
            object.insert(QStringLiteral("transitions"), transitions);
          }
        } else if constexpr (std::is_same_v<T, edit::AddClipEffectCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("effect"), encodeEffect(command.effect));
        } else if constexpr (std::is_same_v<T, edit::RemoveClipEffectCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("effect_id"), encodeEntityId(command.effect_id));
        } else if constexpr (std::is_same_v<T, edit::SetClipEffectParameterCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("effect_id"), encodeEntityId(command.effect_id));
          object.insert(QStringLiteral("parameter"), encodeEffectParameter(command.parameter));
        } else if constexpr (std::is_same_v<T, edit::SetClipTransformCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("transform"), encodeTransform(command.transform));
        } else if constexpr (std::is_same_v<T, edit::SetClipBlendModeCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("blend_mode"), encodeBlendMode(command.blend_mode));
        } else if constexpr (std::is_same_v<T, edit::SetClipAudioPropertiesCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("gain_db"), command.gain_db);
          object.insert(QStringLiteral("pan"), command.pan);
          object.insert(QStringLiteral("fade_in"), encodeTime(command.fade_in));
          object.insert(QStringLiteral("fade_out"), encodeTime(command.fade_out));
        } else if constexpr (std::is_same_v<T, edit::SetClipTitleCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("title"), encodeTitle(command.title));
        } else if constexpr (std::is_same_v<T, edit::SetClipSpeedCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("playback_rate"), encodeRate(command.playback_rate));
          object.insert(QStringLiteral("reversed"), command.reversed);
        } else if constexpr (std::is_same_v<T, edit::SetTrackAudioStateCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("muted"), command.muted);
          object.insert(QStringLiteral("solo"), command.solo);
        } else if constexpr (std::is_same_v<T, edit::SetTrackAudioMixCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("gain_db"), command.gain_db);
          object.insert(QStringLiteral("pan"), command.pan);
        } else if constexpr (std::is_same_v<T, edit::AddTrackEffectCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("effect"), encodeEffect(command.effect));
        } else if constexpr (std::is_same_v<T, edit::RemoveTrackEffectCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("effect_id"), encodeEntityId(command.effect_id));
        } else if constexpr (std::is_same_v<T, edit::SetTrackEffectParameterCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("track_id"), encodeEntityId(command.track_id));
          object.insert(QStringLiteral("effect_id"), encodeEntityId(command.effect_id));
          object.insert(QStringLiteral("parameter"), encodeEffectParameter(command.parameter));
        } else if constexpr (std::is_same_v<T, edit::AddTransitionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("transition"), encodeTransition(command.transition));
        } else if constexpr (std::is_same_v<T, edit::UpdateTransitionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("transition"), encodeTransition(command.transition));
        } else if constexpr (std::is_same_v<T, edit::RemoveTransitionCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("transition_id"), encodeEntityId(command.transition_id));
        } else if constexpr (std::is_same_v<T, edit::CreateBinCommand>) {
          object.insert(QStringLiteral("bin"), encodeMediaBin(command.bin));
        } else if constexpr (std::is_same_v<T, edit::RenameBinCommand>) {
          object.insert(QStringLiteral("bin_id"), encodeEntityId(command.bin_id));
          object.insert(QStringLiteral("name"), stdToQstr(command.name));
        } else if constexpr (std::is_same_v<T, edit::MoveBinCommand>) {
          object.insert(QStringLiteral("bin_id"), encodeEntityId(command.bin_id));
          insertOptionalEntityId(object, QStringLiteral("parent_id"), command.parent_id);
        } else if constexpr (std::is_same_v<T, edit::RemoveBinCommand>) {
          object.insert(QStringLiteral("bin_id"), encodeEntityId(command.bin_id));
        } else if constexpr (std::is_same_v<T, edit::SetAssetBinCommand>) {
          object.insert(QStringLiteral("asset_id"), encodeEntityId(command.asset_id));
          insertOptionalEntityId(object, QStringLiteral("bin_id"), command.bin_id);
        } else if constexpr (std::is_same_v<T, edit::SetAssetMetadataCommand>) {
          object.insert(QStringLiteral("asset_id"), encodeEntityId(command.asset_id));
          object.insert(QStringLiteral("display_title"), stdToQstr(command.display_title));
          QJsonArray tags;
          for (const std::string& tag : command.tags) {
            tags.append(stdToQstr(tag));
          }
          object.insert(QStringLiteral("tags"), tags);
          object.insert(QStringLiteral("notes"), stdToQstr(command.notes));
          object.insert(QStringLiteral("rating"), command.rating);
        } else if constexpr (std::is_same_v<T, edit::SetSmartQueryCommand>) {
          object.insert(QStringLiteral("bin_id"), encodeEntityId(command.bin_id));
          object.insert(QStringLiteral("query"), encodeSmartQuery(command.query));
        } else if constexpr (std::is_same_v<T, edit::ReplaceClipMediaCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("asset_id"), encodeEntityId(command.asset_id));
          object.insert(QStringLiteral("source_range"), encodeTimeRange(command.source_range));
        } else if constexpr (std::is_same_v<T, edit::SetClipNameCommand>) {
          object.insert(QStringLiteral("sequence_id"), encodeEntityId(command.sequence_id));
          object.insert(QStringLiteral("clip_id"), encodeEntityId(command.clip_id));
          object.insert(QStringLiteral("name"), stdToQstr(command.name));
        }
      },
      operation);
}

[[nodiscard]] DecodeResult<edit::EntityId> decodeNewEntityIdField(
    const QJsonObject& object, const QString& key) {
  const QJsonValue value = object.value(key);
  if (!value.isString()) {
    return DecodeResult<edit::EntityId>::success(edit::EntityId::generate());
  }
  const auto parsed = edit::EntityId::parse(qstrToStd(value.toString()));
  if (!parsed.has_value() || parsed->isNil()) {
    return DecodeResult<edit::EntityId>::success(edit::EntityId::generate());
  }
  return DecodeResult<edit::EntityId>::success(*parsed);
}

[[nodiscard]] DecodeResult<EditOperation> decodeOperation(const QString& type,
                                                          const QJsonObject& object) {
  if (type == QStringLiteral("add_asset")) {
    const auto asset = decodeAsset(object.value(QStringLiteral("asset")), "add_asset.asset");
    if (!asset) {
      return fail<EditOperation>(asset.error().message);
    }
    return DecodeResult<EditOperation>::success(edit::AddAssetCommand{asset.value()});
  }
  if (type == QStringLiteral("remove_asset")) {
    const auto asset_id = decodeEntityIdRequired(object.value(QStringLiteral("asset_id")),
                                                 "remove_asset.asset_id");
    if (!asset_id) {
      return fail<EditOperation>(asset_id.error().message);
    }
    return DecodeResult<EditOperation>::success(edit::RemoveAssetCommand{asset_id.value()});
  }
  if (type == QStringLiteral("relink_asset")) {
    edit::RelinkAssetCommand command;
    const auto asset_id = decodeEntityIdRequired(object.value(QStringLiteral("asset_id")),
                                                 "relink_asset.asset_id");
    if (!asset_id) {
      return fail<EditOperation>(asset_id.error().message);
    }
    command.asset_id = asset_id.value();
    command.source_uri = qstrToStd(object.value(QStringLiteral("source_uri")).toString());
    command.fingerprint = qstrToStd(object.value(QStringLiteral("fingerprint")).toString());
    const auto duration = decodeTime(object.value(QStringLiteral("duration")),
                                     "relink_asset.duration");
    if (!duration) {
      return fail<EditOperation>(duration.error().message);
    }
    command.duration = duration.value();
    command.has_video = object.value(QStringLiteral("has_video")).toBool();
    command.has_audio = object.value(QStringLiteral("has_audio")).toBool();
    command.width = static_cast<std::uint32_t>(object.value(QStringLiteral("width")).toInt());
    command.height = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt());
    if (object.contains(QStringLiteral("nominal_frame_rate"))) {
      const auto rate = decodeRate(object.value(QStringLiteral("nominal_frame_rate")),
                                   "relink_asset.nominal_frame_rate");
      if (!rate) {
        return fail<EditOperation>(rate.error().message);
      }
      command.nominal_frame_rate = rate.value();
    }
    command.audio_sample_rate =
        static_cast<std::uint32_t>(object.value(QStringLiteral("audio_sample_rate")).toInt());
    command.audio_channels =
        static_cast<std::uint32_t>(object.value(QStringLiteral("audio_channels")).toInt());
    const auto metadata = decodeStringMap(object.value(QStringLiteral("metadata")),
                                          "relink_asset.metadata");
    if (!metadata) {
      return fail<EditOperation>(metadata.error().message);
    }
    command.metadata = metadata.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("add_sequence")) {
    const auto sequence = decodeSequence(object.value(QStringLiteral("sequence")),
                                         "add_sequence.sequence");
    if (!sequence) {
      return fail<EditOperation>(sequence.error().message);
    }
    return DecodeResult<EditOperation>::success(edit::AddSequenceCommand{sequence.value()});
  }
  if (type == QStringLiteral("remove_sequence")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                    "remove_sequence.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    return DecodeResult<EditOperation>::success(
        edit::RemoveSequenceCommand{sequence_id.value()});
  }
  if (type == QStringLiteral("set_sequence_format")) {
    edit::SetSequenceFormatCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                    "set_sequence_format.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    command.sequence_id = sequence_id.value();
    const auto frame_rate = decodeRate(object.value(QStringLiteral("frame_rate")),
                                       "set_sequence_format.frame_rate");
    if (!frame_rate) {
      return fail<EditOperation>(frame_rate.error().message);
    }
    command.frame_rate = frame_rate.value();
    command.width = static_cast<std::uint32_t>(object.value(QStringLiteral("width")).toInt(1920));
    command.height = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt(1080));
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("add_track")) {
    edit::AddTrackCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                    "add_track.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    command.sequence_id = sequence_id.value();
    const auto track = decodeTrack(object.value(QStringLiteral("track")), "add_track.track");
    if (!track) {
      return fail<EditOperation>(track.error().message);
    }
    command.track = track.value();
    if (object.contains(QStringLiteral("index"))) {
      command.index = static_cast<std::size_t>(object.value(QStringLiteral("index")).toInteger());
    }
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("remove_track")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "remove_track.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                                 "remove_track.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    return DecodeResult<EditOperation>::success(
        edit::RemoveTrackCommand{sequence_id.value(), track_id.value()});
  }
  if (type == QStringLiteral("rename_track")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "rename_track.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                                 "rename_track.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    return DecodeResult<EditOperation>::success(edit::RenameTrackCommand{
        sequence_id.value(), track_id.value(),
        qstrToStd(object.value(QStringLiteral("name")).toString())});
  }
  if (type == QStringLiteral("reorder_track")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "reorder_track.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                                 "reorder_track.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    return DecodeResult<EditOperation>::success(edit::ReorderTrackCommand{
        sequence_id.value(), track_id.value(),
        static_cast<std::size_t>(object.value(QStringLiteral("index")).toInteger())});
  }
  if (type == QStringLiteral("set_track_locked")) {
    edit::SetTrackLockedCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "set_track_locked.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                               "set_track_locked.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    command.track_id = track_id.value();
    command.locked = object.value(QStringLiteral("locked")).toBool();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_track_visibility")) {
    edit::SetTrackVisibilityCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "set_track_visibility.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                               "set_track_visibility.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    command.track_id = track_id.value();
    command.visible = object.value(QStringLiteral("visible")).toBool(true);
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_track_targeted")) {
    edit::SetTrackTargetedCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "set_track_targeted.sequence_id");
    if (!sequence_id) {
      return fail<EditOperation>(sequence_id.error().message);
    }
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                               "set_track_targeted.track_id");
    if (!track_id) {
      return fail<EditOperation>(track_id.error().message);
    }
    command.track_id = track_id.value();
    command.targeted = object.value(QStringLiteral("targeted")).toBool(true);
    return DecodeResult<EditOperation>::success(command);
  }

  if (type == QStringLiteral("insert_clip")) {
    edit::InsertClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")),
                                                  "insert_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")),
                                                "insert_clip.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    command.track_id = track_id.value();
    const auto clip = decodeClip(object.value(QStringLiteral("clip")), "insert_clip.clip");
    if (!clip) return fail<EditOperation>(clip.error().message);
    command.clip = clip.value();
    const auto mode = decodeInsertMode(object.value(QStringLiteral("mode")), "insert_clip.mode");
    if (!mode) return fail<EditOperation>(mode.error().message);
    command.mode = mode.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("move_clip")) {
    edit::MoveClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "move_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "move_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto destination_track_id = decodeEntityIdRequired(object.value(QStringLiteral("destination_track_id")), "move_clip.destination_track_id");
    if (!destination_track_id) return fail<EditOperation>(destination_track_id.error().message);
    command.destination_track_id = destination_track_id.value();
    const auto new_start = decodeTime(object.value(QStringLiteral("new_start")), "move_clip.new_start");
    if (!new_start) return fail<EditOperation>(new_start.error().message);
    command.new_start = new_start.value();
    const auto mode = decodeInsertMode(object.value(QStringLiteral("mode")), "move_clip.mode");
    if (!mode) return fail<EditOperation>(mode.error().message);
    command.mode = mode.value();
    command.include_linked = object.value(QStringLiteral("include_linked")).toBool();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("trim_clip")) {
    edit::TrimClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "trim_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "trim_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto timeline_range = decodeTimeRange(object.value(QStringLiteral("timeline_range")), "trim_clip.timeline_range");
    if (!timeline_range) return fail<EditOperation>(timeline_range.error().message);
    command.timeline_range = timeline_range.value();
    const auto source_range = decodeTimeRange(object.value(QStringLiteral("source_range")), "trim_clip.source_range");
    if (!source_range) return fail<EditOperation>(source_range.error().message);
    command.source_range = source_range.value();
    command.include_linked = object.value(QStringLiteral("include_linked")).toBool();
    const auto mode = decodeInsertMode(object.value(QStringLiteral("mode")), "trim_clip.mode");
    if (!mode) return fail<EditOperation>(mode.error().message);
    command.mode = mode.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("split_clip")) {
    edit::SplitClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "split_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "split_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto split_time = decodeTime(object.value(QStringLiteral("split_time")), "split_clip.split_time");
    if (!split_time) return fail<EditOperation>(split_time.error().message);
    command.split_time = split_time.value();
    const auto right_clip_id = decodeNewEntityIdField(object, QStringLiteral("right_clip_id"));
    if (!right_clip_id) return fail<EditOperation>(right_clip_id.error().message);
    command.right_clip_id = right_clip_id.value();
    command.include_linked = object.value(QStringLiteral("include_linked")).toBool();
    const QJsonArray linked = object.value(QStringLiteral("linked_right_clip_ids")).toArray();
    for (int index = 0; index < linked.size(); ++index) {
      const QJsonObject item = linked.at(index).toObject();
      const auto left_id = decodeEntityIdRequired(item.value(QStringLiteral("clip_id")), "split_clip.linked_right_clip_ids.clip_id");
      if (!left_id) return fail<EditOperation>(left_id.error().message);
      const auto right_id = decodeEntityIdRequired(item.value(QStringLiteral("right_clip_id")), "split_clip.linked_right_clip_ids.right_clip_id");
      if (!right_id) return fail<EditOperation>(right_id.error().message);
      command.linked_right_clip_ids.push_back(edit::LinkedSplitId{left_id.value(), right_id.value()});
    }
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("remove_clip")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "remove_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveClipCommand{
        sequence_id.value(), clip_id.value(),
        object.value(QStringLiteral("ripple")).toBool(),
        object.value(QStringLiteral("include_linked")).toBool()});
  }
  if (type == QStringLiteral("close_gap")) {
    edit::CloseGapCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "close_gap.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "close_gap.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    command.track_id = track_id.value();
    const auto gap = decodeTimeRange(object.value(QStringLiteral("gap")), "close_gap.gap");
    if (!gap) return fail<EditOperation>(gap.error().message);
    command.gap = gap.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("roll_edit")) {
    edit::RollEditCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "roll_edit.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto left_clip_id = decodeEntityIdRequired(object.value(QStringLiteral("left_clip_id")), "roll_edit.left_clip_id");
    if (!left_clip_id) return fail<EditOperation>(left_clip_id.error().message);
    command.left_clip_id = left_clip_id.value();
    const auto right_clip_id = decodeEntityIdRequired(object.value(QStringLiteral("right_clip_id")), "roll_edit.right_clip_id");
    if (!right_clip_id) return fail<EditOperation>(right_clip_id.error().message);
    command.right_clip_id = right_clip_id.value();
    const auto new_cut_time = decodeTime(object.value(QStringLiteral("new_cut_time")), "roll_edit.new_cut_time");
    if (!new_cut_time) return fail<EditOperation>(new_cut_time.error().message);
    command.new_cut_time = new_cut_time.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("slip_clip")) {
    edit::SlipClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "slip_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "slip_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto new_source_start = decodeTime(object.value(QStringLiteral("new_source_start")), "slip_clip.new_source_start");
    if (!new_source_start) return fail<EditOperation>(new_source_start.error().message);
    command.new_source_start = new_source_start.value();
    command.include_linked = object.value(QStringLiteral("include_linked")).toBool();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("slide_clip")) {
    edit::SlideClipCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "slide_clip.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "slide_clip.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto new_start = decodeTime(object.value(QStringLiteral("new_start")), "slide_clip.new_start");
    if (!new_start) return fail<EditOperation>(new_start.error().message);
    command.new_start = new_start.value();
    return DecodeResult<EditOperation>::success(command);
  }

  if (type == QStringLiteral("add_marker")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "add_marker.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto marker = decodeMarker(object.value(QStringLiteral("marker")), "add_marker.marker");
    if (!marker) return fail<EditOperation>(marker.error().message);
    return DecodeResult<EditOperation>::success(edit::AddMarkerCommand{sequence_id.value(), marker.value()});
  }
  if (type == QStringLiteral("update_marker")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "update_marker.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto marker = decodeMarker(object.value(QStringLiteral("marker")), "update_marker.marker");
    if (!marker) return fail<EditOperation>(marker.error().message);
    return DecodeResult<EditOperation>::success(edit::UpdateMarkerCommand{sequence_id.value(), marker.value()});
  }
  if (type == QStringLiteral("remove_marker")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_marker.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto marker_id = decodeEntityIdRequired(object.value(QStringLiteral("marker_id")), "remove_marker.marker_id");
    if (!marker_id) return fail<EditOperation>(marker_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveMarkerCommand{sequence_id.value(), marker_id.value()});
  }
  if (type == QStringLiteral("add_caption")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "add_caption.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto caption = decodeCaption(object.value(QStringLiteral("caption")), "add_caption.caption");
    if (!caption) return fail<EditOperation>(caption.error().message);
    return DecodeResult<EditOperation>::success(edit::AddCaptionCommand{sequence_id.value(), caption.value()});
  }
  if (type == QStringLiteral("update_caption")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "update_caption.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto caption = decodeCaption(object.value(QStringLiteral("caption")), "update_caption.caption");
    if (!caption) return fail<EditOperation>(caption.error().message);
    return DecodeResult<EditOperation>::success(edit::UpdateCaptionCommand{sequence_id.value(), caption.value()});
  }
  if (type == QStringLiteral("remove_caption")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_caption.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto caption_id = decodeEntityIdRequired(object.value(QStringLiteral("caption_id")), "remove_caption.caption_id");
    if (!caption_id) return fail<EditOperation>(caption_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveCaptionCommand{sequence_id.value(), caption_id.value()});
  }
  if (type == QStringLiteral("apply_caption_change_set")) {
    edit::ApplyCaptionChangeSetCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "apply_caption_change_set.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    for (const QJsonValue& value : object.value(QStringLiteral("added")).toArray()) {
      const auto caption = decodeCaption(value, "apply_caption_change_set.added");
      if (!caption) return fail<EditOperation>(caption.error().message);
      command.added.push_back(caption.value());
    }
    for (const QJsonValue& value : object.value(QStringLiteral("updated")).toArray()) {
      const auto caption = decodeCaption(value, "apply_caption_change_set.updated");
      if (!caption) return fail<EditOperation>(caption.error().message);
      command.updated.push_back(caption.value());
    }
    for (const QJsonValue& value : object.value(QStringLiteral("removed")).toArray()) {
      const auto id = decodeEntityIdRequired(value, "apply_caption_change_set.removed");
      if (!id) return fail<EditOperation>(id.error().message);
      command.removed.push_back(id.value());
    }
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("apply_timeline_cut_change_set")) {
    edit::ApplyTimelineCutChangeSetCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "apply_timeline_cut_change_set.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    for (const QJsonValue& value : object.value(QStringLiteral("tracks")).toArray()) {
      const QJsonObject item = value.toObject();
      edit::TrackClipReplacement replacement;
      const auto track_id = decodeEntityIdRequired(item.value(QStringLiteral("track_id")), "apply_timeline_cut_change_set.track_id");
      if (!track_id) return fail<EditOperation>(track_id.error().message);
      replacement.track_id = track_id.value();
      const auto kind = decodeTrackKind(item.value(QStringLiteral("kind")), "apply_timeline_cut_change_set.kind");
      if (!kind) return fail<EditOperation>(kind.error().message);
      replacement.kind = kind.value();
      for (const QJsonValue& clip_value : item.value(QStringLiteral("clips")).toArray()) {
        const auto clip = decodeClip(clip_value, "apply_timeline_cut_change_set.clips");
        if (!clip) return fail<EditOperation>(clip.error().message);
        replacement.clips.push_back(clip.value());
      }
      command.tracks.push_back(replacement);
    }
    if (object.contains(QStringLiteral("transitions"))) {
      std::vector<edit::Transition> transitions;
      for (const QJsonValue& value : object.value(QStringLiteral("transitions")).toArray()) {
        const auto transition = decodeTransition(value, "apply_timeline_cut_change_set.transitions");
        if (!transition) return fail<EditOperation>(transition.error().message);
        transitions.push_back(transition.value());
      }
      command.transitions = std::move(transitions);
    }
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("add_clip_effect")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "add_clip_effect.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "add_clip_effect.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto effect = decodeEffect(object.value(QStringLiteral("effect")), "add_clip_effect.effect");
    if (!effect) return fail<EditOperation>(effect.error().message);
    return DecodeResult<EditOperation>::success(edit::AddClipEffectCommand{sequence_id.value(), clip_id.value(), effect.value()});
  }
  if (type == QStringLiteral("remove_clip_effect")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_clip_effect.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "remove_clip_effect.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto effect_id = decodeEntityIdRequired(object.value(QStringLiteral("effect_id")), "remove_clip_effect.effect_id");
    if (!effect_id) return fail<EditOperation>(effect_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveClipEffectCommand{sequence_id.value(), clip_id.value(), effect_id.value()});
  }
  if (type == QStringLiteral("set_clip_effect_parameter")) {
    edit::SetClipEffectParameterCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_effect_parameter.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_effect_parameter.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto effect_id = decodeEntityIdRequired(object.value(QStringLiteral("effect_id")), "set_clip_effect_parameter.effect_id");
    if (!effect_id) return fail<EditOperation>(effect_id.error().message);
    command.effect_id = effect_id.value();
    const auto parameter = decodeEffectParameter(object.value(QStringLiteral("parameter")), "set_clip_effect_parameter.parameter");
    if (!parameter) return fail<EditOperation>(parameter.error().message);
    command.parameter = parameter.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_clip_transform")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_transform.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_transform.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto transform = decodeTransform(object.value(QStringLiteral("transform")), "set_clip_transform.transform");
    if (!transform) return fail<EditOperation>(transform.error().message);
    return DecodeResult<EditOperation>::success(edit::SetClipTransformCommand{sequence_id.value(), clip_id.value(), transform.value()});
  }
  if (type == QStringLiteral("set_clip_blend_mode")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_blend_mode.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_blend_mode.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto blend_mode = decodeBlendMode(object.value(QStringLiteral("blend_mode")), "set_clip_blend_mode.blend_mode");
    if (!blend_mode) return fail<EditOperation>(blend_mode.error().message);
    return DecodeResult<EditOperation>::success(edit::SetClipBlendModeCommand{sequence_id.value(), clip_id.value(), blend_mode.value()});
  }
  if (type == QStringLiteral("set_clip_audio_properties")) {
    edit::SetClipAudioPropertiesCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_audio_properties.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_audio_properties.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    command.gain_db = object.value(QStringLiteral("gain_db")).toDouble();
    command.pan = object.value(QStringLiteral("pan")).toDouble();
    const auto fade_in = decodeTime(object.value(QStringLiteral("fade_in")), "set_clip_audio_properties.fade_in");
    if (!fade_in) return fail<EditOperation>(fade_in.error().message);
    command.fade_in = fade_in.value();
    const auto fade_out = decodeTime(object.value(QStringLiteral("fade_out")), "set_clip_audio_properties.fade_out");
    if (!fade_out) return fail<EditOperation>(fade_out.error().message);
    command.fade_out = fade_out.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_clip_title")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_title.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_title.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto title = decodeTitle(object.value(QStringLiteral("title")), "set_clip_title.title");
    if (!title) return fail<EditOperation>(title.error().message);
    return DecodeResult<EditOperation>::success(edit::SetClipTitleCommand{sequence_id.value(), clip_id.value(), title.value()});
  }
  if (type == QStringLiteral("set_clip_speed")) {
    edit::SetClipSpeedCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_speed.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto clip_id = decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_speed.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    command.clip_id = clip_id.value();
    const auto playback_rate = decodeRate(object.value(QStringLiteral("playback_rate")), "set_clip_speed.playback_rate");
    if (!playback_rate) return fail<EditOperation>(playback_rate.error().message);
    command.playback_rate = playback_rate.value();
    command.reversed = object.value(QStringLiteral("reversed")).toBool();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_track_audio_state")) {
    edit::SetTrackAudioStateCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_track_audio_state.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "set_track_audio_state.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    command.track_id = track_id.value();
    command.muted = object.value(QStringLiteral("muted")).toBool();
    command.solo = object.value(QStringLiteral("solo")).toBool();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_track_audio_mix")) {
    edit::SetTrackAudioMixCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_track_audio_mix.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "set_track_audio_mix.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    command.track_id = track_id.value();
    command.gain_db = object.value(QStringLiteral("gain_db")).toDouble();
    command.pan = object.value(QStringLiteral("pan")).toDouble();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("add_track_effect")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "add_track_effect.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "add_track_effect.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    const auto effect = decodeEffect(object.value(QStringLiteral("effect")), "add_track_effect.effect");
    if (!effect) return fail<EditOperation>(effect.error().message);
    return DecodeResult<EditOperation>::success(edit::AddTrackEffectCommand{sequence_id.value(), track_id.value(), effect.value()});
  }
  if (type == QStringLiteral("remove_track_effect")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_track_effect.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "remove_track_effect.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    const auto effect_id = decodeEntityIdRequired(object.value(QStringLiteral("effect_id")), "remove_track_effect.effect_id");
    if (!effect_id) return fail<EditOperation>(effect_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveTrackEffectCommand{sequence_id.value(), track_id.value(), effect_id.value()});
  }
  if (type == QStringLiteral("set_track_effect_parameter")) {
    edit::SetTrackEffectParameterCommand command;
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_track_effect_parameter.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    command.sequence_id = sequence_id.value();
    const auto track_id = decodeEntityIdRequired(object.value(QStringLiteral("track_id")), "set_track_effect_parameter.track_id");
    if (!track_id) return fail<EditOperation>(track_id.error().message);
    command.track_id = track_id.value();
    const auto effect_id = decodeEntityIdRequired(object.value(QStringLiteral("effect_id")), "set_track_effect_parameter.effect_id");
    if (!effect_id) return fail<EditOperation>(effect_id.error().message);
    command.effect_id = effect_id.value();
    const auto parameter = decodeEffectParameter(object.value(QStringLiteral("parameter")), "set_track_effect_parameter.parameter");
    if (!parameter) return fail<EditOperation>(parameter.error().message);
    command.parameter = parameter.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("add_transition")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "add_transition.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto transition = decodeTransition(object.value(QStringLiteral("transition")), "add_transition.transition");
    if (!transition) return fail<EditOperation>(transition.error().message);
    return DecodeResult<EditOperation>::success(edit::AddTransitionCommand{sequence_id.value(), transition.value()});
  }
  if (type == QStringLiteral("update_transition")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "update_transition.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto transition = decodeTransition(object.value(QStringLiteral("transition")), "update_transition.transition");
    if (!transition) return fail<EditOperation>(transition.error().message);
    return DecodeResult<EditOperation>::success(edit::UpdateTransitionCommand{sequence_id.value(), transition.value()});
  }
  if (type == QStringLiteral("remove_transition")) {
    const auto sequence_id = decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "remove_transition.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto transition_id = decodeEntityIdRequired(object.value(QStringLiteral("transition_id")), "remove_transition.transition_id");
    if (!transition_id) return fail<EditOperation>(transition_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveTransitionCommand{sequence_id.value(), transition_id.value()});
  }
  if (type == QStringLiteral("create_bin")) {
    const auto bin = decodeMediaBin(object.value(QStringLiteral("bin")), "create_bin.bin");
    if (!bin) return fail<EditOperation>(bin.error().message);
    return DecodeResult<EditOperation>::success(edit::CreateBinCommand{bin.value()});
  }
  if (type == QStringLiteral("rename_bin")) {
    const auto bin_id = decodeEntityIdRequired(object.value(QStringLiteral("bin_id")), "rename_bin.bin_id");
    if (!bin_id) return fail<EditOperation>(bin_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RenameBinCommand{bin_id.value(), qstrToStd(object.value(QStringLiteral("name")).toString())});
  }
  if (type == QStringLiteral("move_bin")) {
    edit::MoveBinCommand command;
    const auto bin_id = decodeEntityIdRequired(object.value(QStringLiteral("bin_id")), "move_bin.bin_id");
    if (!bin_id) return fail<EditOperation>(bin_id.error().message);
    command.bin_id = bin_id.value();
    const auto parent_id = decodeOptionalEntityId(object.value(QStringLiteral("parent_id")), "move_bin.parent_id");
    if (!parent_id) return fail<EditOperation>(parent_id.error().message);
    command.parent_id = parent_id.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("remove_bin")) {
    const auto bin_id = decodeEntityIdRequired(object.value(QStringLiteral("bin_id")), "remove_bin.bin_id");
    if (!bin_id) return fail<EditOperation>(bin_id.error().message);
    return DecodeResult<EditOperation>::success(edit::RemoveBinCommand{bin_id.value()});
  }
  if (type == QStringLiteral("set_asset_bin")) {
    edit::SetAssetBinCommand command;
    const auto asset_id = decodeEntityIdRequired(object.value(QStringLiteral("asset_id")), "set_asset_bin.asset_id");
    if (!asset_id) return fail<EditOperation>(asset_id.error().message);
    command.asset_id = asset_id.value();
    const auto bin_id = decodeOptionalEntityId(object.value(QStringLiteral("bin_id")), "set_asset_bin.bin_id");
    if (!bin_id) return fail<EditOperation>(bin_id.error().message);
    command.bin_id = bin_id.value();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_asset_metadata")) {
    edit::SetAssetMetadataCommand command;
    const auto asset_id = decodeEntityIdRequired(object.value(QStringLiteral("asset_id")), "set_asset_metadata.asset_id");
    if (!asset_id) return fail<EditOperation>(asset_id.error().message);
    command.asset_id = asset_id.value();
    command.display_title = qstrToStd(object.value(QStringLiteral("display_title")).toString());
    for (const QJsonValue& tag : object.value(QStringLiteral("tags")).toArray()) {
      command.tags.push_back(qstrToStd(tag.toString()));
    }
    command.notes = qstrToStd(object.value(QStringLiteral("notes")).toString());
    command.rating = object.value(QStringLiteral("rating")).toInt();
    return DecodeResult<EditOperation>::success(command);
  }
  if (type == QStringLiteral("set_smart_query")) {
    const auto bin_id = decodeEntityIdRequired(object.value(QStringLiteral("bin_id")), "set_smart_query.bin_id");
    if (!bin_id) return fail<EditOperation>(bin_id.error().message);
    const auto query = decodeSmartQuery(object.value(QStringLiteral("query")), "set_smart_query.query");
    if (!query) return fail<EditOperation>(query.error().message);
    return DecodeResult<EditOperation>::success(edit::SetSmartQueryCommand{bin_id.value(), query.value()});
  }
  if (type == QStringLiteral("replace_clip_media")) {
    const auto sequence_id =
        decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "replace_clip_media.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id =
        decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "replace_clip_media.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    const auto asset_id =
        decodeEntityIdRequired(object.value(QStringLiteral("asset_id")), "replace_clip_media.asset_id");
    if (!asset_id) return fail<EditOperation>(asset_id.error().message);
    const auto source_range =
        decodeTimeRange(object.value(QStringLiteral("source_range")), "replace_clip_media.source_range");
    if (!source_range) return fail<EditOperation>(source_range.error().message);
    return DecodeResult<EditOperation>::success(
        edit::ReplaceClipMediaCommand{sequence_id.value(), clip_id.value(), asset_id.value(),
                                      source_range.value()});
  }
  if (type == QStringLiteral("set_clip_name")) {
    const auto sequence_id =
        decodeEntityIdRequired(object.value(QStringLiteral("sequence_id")), "set_clip_name.sequence_id");
    if (!sequence_id) return fail<EditOperation>(sequence_id.error().message);
    const auto clip_id =
        decodeEntityIdRequired(object.value(QStringLiteral("clip_id")), "set_clip_name.clip_id");
    if (!clip_id) return fail<EditOperation>(clip_id.error().message);
    return DecodeResult<EditOperation>::success(
        edit::SetClipNameCommand{sequence_id.value(), clip_id.value(),
                                 qstrToStd(object.value(QStringLiteral("name")).toString())});
  }
  return fail<EditOperation>("unknown command type: " + qstrToStd(type));
}


}  // namespace

QJsonObject encode_command(const EditCommand& command) {
  QJsonObject object;
  object.insert(QStringLiteral("type"), stdToQstr(edit::commandType(command)));
  object.insert(QStringLiteral("coalescing_key"), stdToQstr(command.coalescing_key));
  encodeOperationFields(object, command.operation);
  return object;
}

QByteArray encode_command_bytes(const EditCommand& command) {
  return QJsonDocument(encode_command(command)).toJson(QJsonDocument::Compact);
}

QJsonArray encode_commands(const std::vector<EditCommand>& commands) {
  QJsonArray array;
  for (const EditCommand& command : commands) {
    array.append(encode_command(command));
  }
  return array;
}


edit::Result<EditCommand, CodecError> decode_command(const QJsonObject& object) {
  const QString type = object.value(QStringLiteral("type")).toString();
  if (type.isEmpty()) {
    return edit::Result<EditCommand, CodecError>::failure(makeError("missing command type"));
  }
  const auto operation = decodeOperation(type, object);
  if (!operation) {
    return edit::Result<EditCommand, CodecError>::failure(operation.error());
  }
  EditCommand command;
  command.operation = operation.value();
  command.coalescing_key = qstrToStd(object.value(QStringLiteral("coalescing_key")).toString());
  return edit::Result<EditCommand, CodecError>::success(std::move(command));
}

edit::Result<EditCommand, CodecError> decode_command_bytes(const QByteArray& bytes) {
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    return edit::Result<EditCommand, CodecError>::failure(
        makeError("JSON parse error: " + qstrToStd(parse_error.errorString())));
  }
  if (!document.isObject()) {
    return edit::Result<EditCommand, CodecError>::failure(makeError("expected JSON object"));
  }
  return decode_command(document.object());
}

edit::Result<std::vector<EditCommand>, CodecError> decode_commands(const QJsonArray& array) {
  std::vector<EditCommand> commands;
  for (int index = 0; index < array.size(); ++index) {
    if (!array.at(index).isObject()) {
      return edit::Result<std::vector<EditCommand>, CodecError>::failure(
          makeError("command array element must be an object"));
    }
    const auto command = decode_command(array.at(index).toObject());
    if (!command) {
      return edit::Result<std::vector<EditCommand>, CodecError>::failure(command.error());
    }
    commands.push_back(command.value());
  }
  return edit::Result<std::vector<EditCommand>, CodecError>::success(std::move(commands));
}


}  // namespace video_editor::agent_protocol
