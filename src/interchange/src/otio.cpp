// SPDX-License-Identifier: MPL-2.0
#include "video_editor/interchange/otio.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace video_editor::interchange {
namespace {

constexpr const char* kSchemaKey = "OTIO_SCHEMA";
constexpr const char* kVideoEditorMetaKey = "video_editor";

void appendWarning(OtioReport* report, std::string message) {
  if (report != nullptr) {
    report->warnings.push_back(std::move(message));
  }
}

void appendSkipped(OtioReport* report, std::string message) {
  if (report != nullptr) {
    report->skipped.push_back(std::move(message));
  }
}

[[nodiscard]] double rateToDouble(const edit::Rate& rate) {
  return static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
}

[[nodiscard]] std::uint32_t preferredTimescale(const edit::Rate& frame_rate,
                                               edit::TrackKind track_kind) {
  if (track_kind == edit::TrackKind::Audio) {
    return 48'000U;
  }
  return frame_rate.numerator() / std::max(1U, frame_rate.denominator());
}

[[nodiscard]] std::int64_t roundToInt64(const double value) {
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(std::llround(value));
}

[[nodiscard]] QJsonObject makeRationalTimeJson(const edit::Time& time, const double otio_rate) {
  const double seconds =
      static_cast<double>(time.value()) / static_cast<double>(time.timescale());
  const auto otio_value = roundToInt64(seconds * otio_rate);
  QJsonObject object;
  object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("RationalTime.1"));
  object.insert(QStringLiteral("value"), static_cast<double>(otio_value));
  object.insert(QStringLiteral("rate"), otio_rate);
  return object;
}

[[nodiscard]] edit::Result<edit::Time, std::string> parseRationalTime(
    const QJsonObject& object, const std::uint32_t target_timescale, const char* context) {
  if (object.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("RationalTime.1")) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": expected RationalTime.1");
  }
  if (!object.contains(QStringLiteral("value")) || !object.contains(QStringLiteral("rate"))) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": RationalTime requires value and rate");
  }
  const double value = object.value(QStringLiteral("value")).toDouble();
  const double rate = object.value(QStringLiteral("rate")).toDouble();
  if (rate == 0.0) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": RationalTime rate must be non-zero");
  }
  const double seconds = value / rate;
  const auto scaled = roundToInt64(seconds * static_cast<double>(target_timescale));
  return edit::Result<edit::Time, std::string>::success(
      edit::Time(scaled, target_timescale));
}

[[nodiscard]] QJsonObject makeTimeRangeJson(const edit::TimeRange& range, const double otio_rate) {
  QJsonObject object;
  object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("TimeRange.1"));
  object.insert(QStringLiteral("start_time"), makeRationalTimeJson(range.start, otio_rate));
  object.insert(QStringLiteral("duration"), makeRationalTimeJson(range.duration, otio_rate));
  return object;
}

[[nodiscard]] edit::Result<edit::TimeRange, std::string> parseTimeRange(
    const QJsonObject& object, const std::uint32_t target_timescale, const char* context) {
  if (object.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("TimeRange.1")) {
    return edit::Result<edit::TimeRange, std::string>::failure(
        std::string(context) + ": expected TimeRange.1");
  }
  const auto start = parseRationalTime(
      object.value(QStringLiteral("start_time")).toObject(), target_timescale,
      (std::string(context) + ".start_time").c_str());
  if (!start) {
    return edit::Result<edit::TimeRange, std::string>::failure(start.error());
  }
  const auto duration = parseRationalTime(
      object.value(QStringLiteral("duration")).toObject(), target_timescale,
      (std::string(context) + ".duration").c_str());
  if (!duration) {
    return edit::Result<edit::TimeRange, std::string>::failure(duration.error());
  }
  return edit::Result<edit::TimeRange, std::string>::success(
      edit::TimeRange(start.value(), duration.value()));
}

[[nodiscard]] QString clipKindToString(const edit::ClipKind kind) {
  switch (kind) {
  case edit::ClipKind::Video:
    return QStringLiteral("Video");
  case edit::ClipKind::Audio:
    return QStringLiteral("Audio");
  case edit::ClipKind::Title:
    return QStringLiteral("Title");
  case edit::ClipKind::NestedSequence:
    return QStringLiteral("NestedSequence");
  }
  return QStringLiteral("Video");
}

[[nodiscard]] QString trackKindToOtio(const edit::TrackKind kind) {
  switch (kind) {
  case edit::TrackKind::Video:
    return QStringLiteral("Video");
  case edit::TrackKind::Audio:
    return QStringLiteral("Audio");
  case edit::TrackKind::Caption:
    return QStringLiteral("Video");
  }
  return QStringLiteral("Video");
}

[[nodiscard]] edit::TrackKind otioKindToTrackKind(const QString& kind, OtioReport* report) {
  if (kind == QStringLiteral("Audio")) {
    return edit::TrackKind::Audio;
  }
  if (kind != QStringLiteral("Video")) {
    appendWarning(report, "Unknown OTIO track kind '" + kind.toStdString() + "'; using Video");
  }
  return edit::TrackKind::Video;
}

[[nodiscard]] QJsonObject effectToJson(const edit::Effect& effect) {
  QJsonObject object;
  object.insert(QStringLiteral("type"), QString::fromStdString(effect.type));
  object.insert(QStringLiteral("version"), static_cast<int>(effect.version));
  object.insert(QStringLiteral("enabled"), effect.enabled);
  object.insert(QStringLiteral("known"), effect.known);
  return object;
}

[[nodiscard]] QJsonObject clipMetadata(const edit::Clip& clip) {
  QJsonObject meta;
  meta.insert(QStringLiteral("clip_kind"), clipKindToString(clip.kind));
  if (!clip.effects.empty()) {
    QJsonArray effects;
    for (const edit::Effect& effect : clip.effects) {
      effects.append(effectToJson(effect));
    }
    meta.insert(QStringLiteral("effects"), effects);
  }
  if (clip.audio_gain_db != 0.0) {
    meta.insert(QStringLiteral("audio_gain_db"), clip.audio_gain_db);
  }
  if (clip.audio_pan != 0.0) {
    meta.insert(QStringLiteral("audio_pan"), clip.audio_pan);
  }
  return meta;
}

[[nodiscard]] QJsonObject trackMetadata(const edit::Track& track) {
  QJsonObject meta;
  if (!track.effects.empty()) {
    QJsonArray effects;
    for (const edit::Effect& effect : track.effects) {
      effects.append(effectToJson(effect));
    }
    meta.insert(QStringLiteral("track_effects"), effects);
  }
  if (track.audio_gain_db != 0.0) {
    meta.insert(QStringLiteral("audio_gain_db"), track.audio_gain_db);
  }
  if (track.audio_pan != 0.0) {
    meta.insert(QStringLiteral("audio_pan"), track.audio_pan);
  }
  meta.insert(QStringLiteral("locked"), track.locked);
  meta.insert(QStringLiteral("muted"), track.muted);
  meta.insert(QStringLiteral("solo"), track.solo);
  meta.insert(QStringLiteral("visible"), track.visible);
  meta.insert(QStringLiteral("targeted"), track.targeted);
  return meta;
}

[[nodiscard]] QJsonObject makeExternalReference(const edit::Asset& asset) {
  QJsonObject reference;
  reference.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("ExternalReference.1"));
  reference.insert(QStringLiteral("target_url"), QString::fromStdString(asset.source_uri));
  if (!asset.duration.isZero()) {
    const double rate = asset.nominal_frame_rate.has_value()
                            ? rateToDouble(*asset.nominal_frame_rate)
                            : 30.0;
    reference.insert(QStringLiteral("available_range"),
                     makeTimeRangeJson(edit::TimeRange(edit::Time{}, asset.duration), rate));
  }
  return reference;
}

struct ExportContext final {
  const edit::Project& project;
  OtioReport* report{nullptr};
};

[[nodiscard]] edit::Result<QJsonArray, std::string> exportTrackChildren(
    const ExportContext& context, const edit::Sequence& sequence, const edit::Track& track);

[[nodiscard]] edit::Result<QJsonObject, std::string> exportNestedStack(
    const ExportContext& context, const edit::Sequence& parent_sequence, const edit::Clip& clip) {
  if (!clip.nested_sequence_id.has_value()) {
    return edit::Result<QJsonObject, std::string>::failure("nested clip missing sequence id");
  }
  const edit::Sequence* nested = edit::findSequence(context.project, *clip.nested_sequence_id);
  if (nested == nullptr) {
    return edit::Result<QJsonObject, std::string>::failure("nested sequence not found in project");
  }

  const double otio_rate = rateToDouble(parent_sequence.frame_rate);
  QJsonObject stack;
  stack.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Stack.1"));
  stack.insert(QStringLiteral("name"), QString::fromStdString(clip.name.empty() ? nested->name
                                                                                : clip.name));
  stack.insert(QStringLiteral("source_range"),
               makeTimeRangeJson(clip.timeline_range, otio_rate));

  QJsonObject meta;
  meta.insert(QStringLiteral("clip_kind"), QStringLiteral("NestedSequence"));
  meta.insert(QStringLiteral("nested_sequence_name"), QString::fromStdString(nested->name));
  stack.insert(QStringLiteral("metadata"),
               QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), meta}});

  QJsonArray nested_tracks;
  for (const edit::Track& nested_track : nested->tracks) {
    QJsonObject track_object;
    track_object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Track.1"));
    track_object.insert(QStringLiteral("name"), QString::fromStdString(nested_track.name));
    track_object.insert(QStringLiteral("kind"), trackKindToOtio(nested_track.kind));
    const QJsonObject track_meta = trackMetadata(nested_track);
    if (!track_meta.isEmpty()) {
      track_object.insert(QStringLiteral("metadata"),
                          QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), track_meta}});
    }
    const auto children = exportTrackChildren(context, *nested, nested_track);
    if (!children) {
      return edit::Result<QJsonObject, std::string>::failure(children.error());
    }
    track_object.insert(QStringLiteral("children"), children.value());
    nested_tracks.append(track_object);
  }
  stack.insert(QStringLiteral("children"), nested_tracks);
  return edit::Result<QJsonObject, std::string>::success(std::move(stack));
}

[[nodiscard]] edit::Result<QJsonObject, std::string> exportMediaClip(
    const ExportContext& context, const edit::Sequence& sequence, const edit::Track& track,
    const edit::Clip& clip) {
  const edit::Asset* asset = edit::findAsset(context.project, clip.asset_id);
  if (asset == nullptr) {
    return edit::Result<QJsonObject, std::string>::failure("clip references missing asset");
  }

  const double otio_rate =
      track.kind == edit::TrackKind::Audio ? 48'000.0 : rateToDouble(sequence.frame_rate);

  QJsonObject object;
  object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Clip.1"));
  object.insert(QStringLiteral("name"),
                QString::fromStdString(clip.name.empty() ? asset->name : clip.name));
  object.insert(QStringLiteral("source_range"), makeTimeRangeJson(clip.source_range, otio_rate));
  object.insert(QStringLiteral("media_reference"), makeExternalReference(*asset));

  const QJsonObject meta = clipMetadata(clip);
  if (!meta.isEmpty()) {
    object.insert(QStringLiteral("metadata"),
                  QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), meta}});
  }
  return edit::Result<QJsonObject, std::string>::success(std::move(object));
}

[[nodiscard]] edit::Result<QJsonArray, std::string> exportTrackChildren(
    const ExportContext& context, const edit::Sequence& sequence, const edit::Track& track) {
  const double otio_rate =
      track.kind == edit::TrackKind::Audio ? 48'000.0 : rateToDouble(sequence.frame_rate);
  const std::uint32_t timescale = preferredTimescale(sequence.frame_rate, track.kind);

  std::vector<edit::Clip> sorted = track.clips;
  std::sort(sorted.begin(), sorted.end(), [](const edit::Clip& lhs, const edit::Clip& rhs) {
    return lhs.timeline_range.start < rhs.timeline_range.start;
  });

  QJsonArray children;
  edit::Time cursor{0, timescale};
  for (const edit::Clip& clip : sorted) {
    if (clip.timeline_range.start > cursor) {
      QJsonObject gap;
      gap.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Gap.1"));
      gap.insert(QStringLiteral("source_range"),
                 makeTimeRangeJson(edit::TimeRange(cursor, clip.timeline_range.start - cursor),
                                   otio_rate));
      children.append(gap);
    }

    if (clip.kind == edit::ClipKind::NestedSequence) {
      const auto stack = exportNestedStack(context, sequence, clip);
      if (!stack) {
        return edit::Result<QJsonArray, std::string>::failure(stack.error());
      }
      children.append(stack.value());
    } else {
      const auto exported = exportMediaClip(context, sequence, track, clip);
      if (!exported) {
        return edit::Result<QJsonArray, std::string>::failure(exported.error());
      }
      children.append(exported.value());
    }
    cursor = clip.timeline_range.end();
  }
  return edit::Result<QJsonArray, std::string>::success(std::move(children));
}

[[nodiscard]] QJsonArray exportMarkers(const edit::Sequence& sequence) {
  const double otio_rate = rateToDouble(sequence.frame_rate);
  QJsonArray markers;
  for (const edit::Marker& marker : sequence.markers) {
    QJsonObject object;
    object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Marker.1"));
    object.insert(QStringLiteral("name"), QString::fromStdString(marker.label));
    object.insert(QStringLiteral("color"), QStringLiteral("YELLOW"));
    object.insert(QStringLiteral("marked_range"), makeTimeRangeJson(marker.range, otio_rate));
    QJsonObject meta;
    meta.insert(QStringLiteral("red"), marker.color.red);
    meta.insert(QStringLiteral("green"), marker.color.green);
    meta.insert(QStringLiteral("blue"), marker.color.blue);
    meta.insert(QStringLiteral("alpha"), marker.color.alpha);
    object.insert(QStringLiteral("metadata"),
                  QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), meta}});
    markers.append(object);
  }
  return markers;
}

struct ImportContext final {
  OtioReport* report{nullptr};
  std::unordered_map<std::string, edit::EntityId> assets_by_uri;
  edit::Project project;
};

[[nodiscard]] bool isKnownComposableSchema(const QString& schema) {
  return schema == QStringLiteral("Clip.1") || schema == QStringLiteral("Gap.1") ||
         schema == QStringLiteral("Stack.1");
}

[[nodiscard]] bool isKnownEffectSchema(const QString& schema) {
  return schema.endsWith(QStringLiteral(".Effect.1")) ||
         schema.endsWith(QStringLiteral(".Filter.1"));
}

[[nodiscard]] edit::Result<edit::Asset, std::string> assetFromReference(
    ImportContext& context, const QJsonObject& reference, edit::TrackKind track_kind,
    const char* context_label) {
  if (reference.value(QString::fromUtf8(kSchemaKey)).toString() !=
      QStringLiteral("ExternalReference.1")) {
    return edit::Result<edit::Asset, std::string>::failure(
        std::string(context_label) + ": expected ExternalReference.1");
  }
  const QString target_url = reference.value(QStringLiteral("target_url")).toString();
  if (target_url.isEmpty()) {
    return edit::Result<edit::Asset, std::string>::failure(
        std::string(context_label) + ": ExternalReference missing target_url");
  }
  const std::string uri = target_url.toStdString();
  const auto found = context.assets_by_uri.find(uri);
  if (found != context.assets_by_uri.end()) {
    const edit::Asset* existing = edit::findAsset(context.project, found->second);
    if (existing != nullptr) {
      return edit::Result<edit::Asset, std::string>::success(*existing);
    }
  }

  edit::Asset asset;
  asset.id = edit::EntityId::generate();
  asset.source_uri = uri;
  asset.name = uri;
  const auto slash = uri.find_last_of("/\\");
  if (slash != std::string::npos && slash + 1 < uri.size()) {
    asset.name = uri.substr(slash + 1);
  }
  asset.has_video = track_kind != edit::TrackKind::Audio;
  asset.has_audio = track_kind == edit::TrackKind::Audio;
  context.assets_by_uri.emplace(uri, asset.id);
  context.project.assets.push_back(asset);
  return edit::Result<edit::Asset, std::string>::success(asset);
}

[[nodiscard]] edit::ClipKind clipKindFromMetadata(const QJsonObject& metadata) {
  const QJsonObject ve = metadata.value(QString::fromUtf8(kVideoEditorMetaKey)).toObject();
  const QString kind = ve.value(QStringLiteral("clip_kind")).toString();
  if (kind == QStringLiteral("Audio")) {
    return edit::ClipKind::Audio;
  }
  if (kind == QStringLiteral("Title")) {
    return edit::ClipKind::Title;
  }
  if (kind == QStringLiteral("NestedSequence")) {
    return edit::ClipKind::NestedSequence;
  }
  return edit::ClipKind::Video;
}

[[nodiscard]] edit::Result<std::vector<edit::Track>, std::string> importStackTracks(
    ImportContext& context, const QJsonObject& stack, const edit::Rate& frame_rate,
    const char* context_label);

[[nodiscard]] edit::Result<std::nullopt_t, std::string> importTrackChildren(
    ImportContext& context, const QJsonArray& children, const edit::Rate& frame_rate,
    edit::TrackKind track_kind, edit::Track& track, const char* context_label) {
  const std::uint32_t timescale = preferredTimescale(frame_rate, track_kind);
  edit::Time cursor{0, timescale};

  for (int index = 0; index < children.size(); ++index) {
    const QJsonObject child = children.at(index).toObject();
    const QString schema = child.value(QString::fromUtf8(kSchemaKey)).toString();
    const std::string child_context =
        std::string(context_label) + ".children[" + std::to_string(index) + "]";

    if (schema == QStringLiteral("Gap.1")) {
      const auto range = parseTimeRange(child.value(QStringLiteral("source_range")).toObject(),
                                        timescale, (child_context + ".source_range").c_str());
      if (!range) {
        return edit::Result<std::nullopt_t, std::string>::failure(range.error());
      }
      cursor = cursor + range.value().duration;
      continue;
    }

    if (isKnownEffectSchema(schema)) {
      appendSkipped(context.report, child_context + ": skipped unknown effect plugin " +
                                        schema.toStdString());
      continue;
    }

    if (schema == QStringLiteral("Stack.1")) {
      const auto nested_tracks = importStackTracks(context, child, frame_rate, child_context.c_str());
      if (!nested_tracks) {
        return edit::Result<std::nullopt_t, std::string>::failure(nested_tracks.error());
      }
      edit::Sequence nested;
      nested.id = edit::EntityId::generate();
      nested.name = child.value(QStringLiteral("name")).toString().toStdString();
      if (nested.name.empty()) {
        nested.name = "Nested Sequence";
      }
      nested.frame_rate = frame_rate;
      nested.tracks = std::move(nested_tracks.value());
      context.project.sequences.push_back(nested);

      const auto range = parseTimeRange(child.value(QStringLiteral("source_range")).toObject(),
                                        timescale, (child_context + ".source_range").c_str());
      if (!range) {
        return edit::Result<std::nullopt_t, std::string>::failure(range.error());
      }

      edit::Clip clip;
      clip.id = edit::EntityId::generate();
      clip.kind = edit::ClipKind::NestedSequence;
      clip.name = nested.name;
      clip.nested_sequence_id = nested.id;
      clip.timeline_range = edit::TimeRange(cursor, range.value().duration);
      clip.source_range = range.value();
      track.clips.push_back(std::move(clip));
      cursor = cursor + range.value().duration;
      continue;
    }

    if (schema != QStringLiteral("Clip.1")) {
      if (!schema.isEmpty() && !isKnownComposableSchema(schema)) {
        appendSkipped(context.report, child_context + ": skipped unknown composable " +
                                          schema.toStdString());
        continue;
      }
      return edit::Result<std::nullopt_t, std::string>::failure(child_context +
                                                                ": unsupported composable schema");
    }

    const QJsonObject media_reference = child.value(QStringLiteral("media_reference")).toObject();
    const QString media_schema = media_reference.value(QString::fromUtf8(kSchemaKey)).toString();
    if (media_schema == QStringLiteral("Stack.1")) {
      const auto nested_tracks =
          importStackTracks(context, media_reference, frame_rate, child_context.c_str());
      if (!nested_tracks) {
        return edit::Result<std::nullopt_t, std::string>::failure(nested_tracks.error());
      }
      edit::Sequence nested;
      nested.id = edit::EntityId::generate();
      nested.name = child.value(QStringLiteral("name")).toString().toStdString();
      nested.frame_rate = frame_rate;
      nested.tracks = std::move(nested_tracks.value());
      context.project.sequences.push_back(nested);

      const auto range = parseTimeRange(child.value(QStringLiteral("source_range")).toObject(),
                                        timescale, (child_context + ".source_range").c_str());
      if (!range) {
        return edit::Result<std::nullopt_t, std::string>::failure(range.error());
      }

      edit::Clip clip;
      clip.id = edit::EntityId::generate();
      clip.kind = edit::ClipKind::NestedSequence;
      clip.name = nested.name;
      clip.nested_sequence_id = nested.id;
      clip.timeline_range = edit::TimeRange(cursor, range.value().duration);
      clip.source_range = range.value();
      track.clips.push_back(std::move(clip));
      cursor = cursor + range.value().duration;
      continue;
    }

    if (media_schema != QStringLiteral("ExternalReference.1") || media_reference.isEmpty()) {
      return edit::Result<std::nullopt_t, std::string>::failure(
          child_context +
          ": Clip.1 requires ExternalReference.1 or nested Stack media identity");
    }

    const auto asset = assetFromReference(context, media_reference, track_kind, child_context.c_str());
    if (!asset) {
      return edit::Result<std::nullopt_t, std::string>::failure(asset.error());
    }

    const auto source_range = parseTimeRange(child.value(QStringLiteral("source_range")).toObject(),
                                             timescale, (child_context + ".source_range").c_str());
    if (!source_range) {
      return edit::Result<std::nullopt_t, std::string>::failure(source_range.error());
    }

    edit::Clip clip;
    clip.id = edit::EntityId::generate();
    clip.asset_id = asset.value().id;
    clip.kind = clipKindFromMetadata(child.value(QStringLiteral("metadata")).toObject());
    if (track_kind == edit::TrackKind::Audio) {
      clip.kind = edit::ClipKind::Audio;
    }
    clip.name = child.value(QStringLiteral("name")).toString().toStdString();
    clip.timeline_range = edit::TimeRange(cursor, source_range.value().duration);
    clip.source_range = source_range.value();

    const QJsonObject ve =
        child.value(QStringLiteral("metadata")).toObject().value(QString::fromUtf8(kVideoEditorMetaKey))
            .toObject();
    if (ve.contains(QStringLiteral("effects"))) {
      appendSkipped(context.report,
                    child_context + ": imported clip effects as unknown/disabled metadata");
    }

    track.clips.push_back(std::move(clip));
    cursor = cursor + source_range.value().duration;
  }

  return edit::Result<std::nullopt_t, std::string>::success(std::nullopt);
}

[[nodiscard]] edit::Result<std::vector<edit::Track>, std::string> importStackTracks(
    ImportContext& context, const QJsonObject& stack, const edit::Rate& frame_rate,
    const char* context_label) {
  const QJsonArray children = stack.value(QStringLiteral("children")).toArray();
  if (children.isEmpty()) {
    return edit::Result<std::vector<edit::Track>, std::string>::failure(
        std::string(context_label) + ": nested Stack has no tracks");
  }

  std::vector<edit::Track> tracks;
  for (int index = 0; index < children.size(); ++index) {
    const QJsonObject child = children.at(index).toObject();
    const QString schema = child.value(QString::fromUtf8(kSchemaKey)).toString();
    const std::string child_context =
        std::string(context_label) + ".children[" + std::to_string(index) + "]";
    if (schema != QStringLiteral("Track.1")) {
      appendSkipped(context.report, child_context + ": skipped non-track stack child " +
                                        schema.toStdString());
      continue;
    }

    edit::Track track;
    track.id = edit::EntityId::generate();
    track.name = child.value(QStringLiteral("name")).toString().toStdString();
    track.kind = otioKindToTrackKind(child.value(QStringLiteral("kind")).toString(), context.report);
    const auto imported = importTrackChildren(context, child.value(QStringLiteral("children")).toArray(),
                                              frame_rate, track.kind, track, child_context.c_str());
    if (!imported) {
      return edit::Result<std::vector<edit::Track>, std::string>::failure(imported.error());
    }
    tracks.push_back(std::move(track));
  }

  if (tracks.empty()) {
    return edit::Result<std::vector<edit::Track>, std::string>::failure(
        std::string(context_label) + ": nested Stack contained no importable tracks");
  }
  return edit::Result<std::vector<edit::Track>, std::string>::success(std::move(tracks));
}

[[nodiscard]] edit::Result<std::vector<edit::Marker>, std::string> importMarkers(
    const QJsonArray& markers, const edit::Rate& frame_rate, OtioReport* report) {
  const std::uint32_t timescale = preferredTimescale(frame_rate, edit::TrackKind::Video);
  std::vector<edit::Marker> imported;
  for (int index = 0; index < markers.size(); ++index) {
    const QJsonObject marker = markers.at(index).toObject();
    if (marker.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("Marker.1")) {
      appendSkipped(report, "markers[" + std::to_string(index) + "]: skipped non-Marker entry");
      continue;
    }
    const auto range = parseTimeRange(marker.value(QStringLiteral("marked_range")).toObject(),
                                      timescale,
                                      ("markers[" + std::to_string(index) + "].marked_range").c_str());
    if (!range) {
      return edit::Result<std::vector<edit::Marker>, std::string>::failure(range.error());
    }
    edit::Marker result;
    result.id = edit::EntityId::generate();
    result.label = marker.value(QStringLiteral("name")).toString().toStdString();
    result.range = range.value();
    const QJsonObject ve =
        marker.value(QStringLiteral("metadata")).toObject().value(QString::fromUtf8(kVideoEditorMetaKey))
            .toObject();
    if (ve.contains(QStringLiteral("red"))) {
      result.color.red = ve.value(QStringLiteral("red")).toDouble(result.color.red);
      result.color.green = ve.value(QStringLiteral("green")).toDouble(result.color.green);
      result.color.blue = ve.value(QStringLiteral("blue")).toDouble(result.color.blue);
      result.color.alpha = ve.value(QStringLiteral("alpha")).toDouble(result.color.alpha);
    }
    imported.push_back(std::move(result));
  }
  return edit::Result<std::vector<edit::Marker>, std::string>::success(std::move(imported));
}

}  // namespace

edit::Result<std::string, std::string> export_otio_json(const edit::Project& project,
                                                         const edit::EntityId sequence_id,
                                                         OtioReport* report) {
  const edit::Sequence* sequence = edit::findSequence(project, sequence_id);
  if (sequence == nullptr) {
    return edit::Result<std::string, std::string>::failure("sequence not found in project");
  }

  ExportContext context{.project = project, .report = report};

  QJsonObject timeline;
  timeline.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Timeline.1"));
  timeline.insert(QStringLiteral("name"), QString::fromStdString(sequence->name));

  QJsonObject ve_meta;
  ve_meta.insert(QStringLiteral("sequence_id"), QString::fromStdString(sequence->id.toString()));
  ve_meta.insert(QStringLiteral("width"), static_cast<int>(sequence->width));
  ve_meta.insert(QStringLiteral("height"), static_cast<int>(sequence->height));
  ve_meta.insert(QStringLiteral("frame_rate_numerator"),
                 static_cast<int>(sequence->frame_rate.numerator()));
  ve_meta.insert(QStringLiteral("frame_rate_denominator"),
                 static_cast<int>(sequence->frame_rate.denominator()));
  timeline.insert(QStringLiteral("metadata"),
                  QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), ve_meta}});

  QJsonObject stack;
  stack.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Stack.1"));
  QJsonArray tracks;
  for (const edit::Track& track : sequence->tracks) {
    QJsonObject track_object;
    track_object.insert(QString::fromUtf8(kSchemaKey), QStringLiteral("Track.1"));
    track_object.insert(QStringLiteral("name"), QString::fromStdString(track.name));
    track_object.insert(QStringLiteral("kind"), trackKindToOtio(track.kind));
    const QJsonObject track_meta = trackMetadata(track);
    if (!track_meta.isEmpty()) {
      track_object.insert(QStringLiteral("metadata"),
                          QJsonObject{{QString::fromUtf8(kVideoEditorMetaKey), track_meta}});
    }
    const auto children = exportTrackChildren(context, *sequence, track);
    if (!children) {
      return edit::Result<std::string, std::string>::failure(children.error());
    }
    track_object.insert(QStringLiteral("children"), children.value());
    tracks.append(track_object);
  }
  stack.insert(QStringLiteral("children"), tracks);
  timeline.insert(QStringLiteral("tracks"), stack);
  timeline.insert(QStringLiteral("markers"), exportMarkers(*sequence));

  const QJsonDocument document(timeline);
  const QByteArray encoded = document.toJson(QJsonDocument::Indented);
  return edit::Result<std::string, std::string>::success(
      std::string(encoded.constData(), static_cast<std::size_t>(encoded.size())));
}

edit::Result<edit::Project, std::string> import_otio_json(const std::string_view json,
                                                          OtioReport* report) {
  QJsonParseError parse_error{};
  const QJsonDocument document =
      QJsonDocument::fromJson(QByteArray(json.data(), static_cast<int>(json.size())), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return edit::Result<edit::Project, std::string>::failure(
        std::string("invalid JSON: ") + parse_error.errorString().toStdString());
  }

  const QJsonObject timeline = document.object();
  if (timeline.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("Timeline.1")) {
    return edit::Result<edit::Project, std::string>::failure("root object must be Timeline.1");
  }

  ImportContext context{
      .report = report,
      .assets_by_uri = {},
      .project = {},
  };
  context.project.id = edit::EntityId::generate();
  context.project.name = timeline.value(QStringLiteral("name")).toString().toStdString();
  if (context.project.name.empty()) {
    context.project.name = "Imported OTIO";
  }

  edit::Sequence sequence;
  sequence.id = edit::EntityId::generate();
  sequence.name = context.project.name;

  const QJsonObject ve_meta =
      timeline.value(QStringLiteral("metadata")).toObject().value(QString::fromUtf8(kVideoEditorMetaKey))
          .toObject();
  sequence.width = static_cast<std::uint32_t>(
      std::max(1, ve_meta.value(QStringLiteral("width")).toInt(static_cast<int>(sequence.width))));
  sequence.height = static_cast<std::uint32_t>(
      std::max(1, ve_meta.value(QStringLiteral("height")).toInt(static_cast<int>(sequence.height))));
  const int fps_num = ve_meta.value(QStringLiteral("frame_rate_numerator")).toInt(30);
  const int fps_den = std::max(1, ve_meta.value(QStringLiteral("frame_rate_denominator")).toInt(1));
  sequence.frame_rate = edit::Rate(static_cast<std::uint32_t>(fps_num),
                                   static_cast<std::uint32_t>(fps_den));

  const QJsonObject tracks_stack = timeline.value(QStringLiteral("tracks")).toObject();
  if (tracks_stack.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("Stack.1")) {
    return edit::Result<edit::Project, std::string>::failure("Timeline.tracks must be Stack.1");
  }

  const QJsonArray track_children = tracks_stack.value(QStringLiteral("children")).toArray();
  for (int index = 0; index < track_children.size(); ++index) {
    const QJsonObject track_object = track_children.at(index).toObject();
    const std::string track_context = "tracks.children[" + std::to_string(index) + "]";
    if (track_object.value(QString::fromUtf8(kSchemaKey)).toString() != QStringLiteral("Track.1")) {
      appendSkipped(report, track_context + ": skipped non-track entry");
      continue;
    }

    edit::Track track;
    track.id = edit::EntityId::generate();
    track.name = track_object.value(QStringLiteral("name")).toString().toStdString();
    track.kind =
        otioKindToTrackKind(track_object.value(QStringLiteral("kind")).toString(), report);
    const auto imported =
        importTrackChildren(context, track_object.value(QStringLiteral("children")).toArray(),
                            sequence.frame_rate, track.kind, track, track_context.c_str());
    if (!imported) {
      return edit::Result<edit::Project, std::string>::failure(imported.error());
    }
    sequence.tracks.push_back(std::move(track));
  }

  const auto markers = importMarkers(timeline.value(QStringLiteral("markers")).toArray(),
                                     sequence.frame_rate, report);
  if (!markers) {
    return edit::Result<edit::Project, std::string>::failure(markers.error());
  }
  sequence.markers = std::move(markers.value());

  context.project.sequences.push_back(std::move(sequence));
  return edit::Result<edit::Project, std::string>::success(std::move(context.project));
}

}  // namespace video_editor::interchange
