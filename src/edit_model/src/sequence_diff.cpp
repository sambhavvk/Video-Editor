// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/sequence_diff.h"

#include <array>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace video_editor::edit {
namespace {

[[nodiscard]] std::string render(const bool value) {
  return value ? "true" : "false";
}

[[nodiscard]] std::string render(const double value) {
  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.6g", value);
  if (written <= 0) {
    return "0";
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

[[nodiscard]] std::string render(const std::uint32_t value) {
  return std::to_string(value);
}

[[nodiscard]] std::string render(const Time& time) {
  return time.toString();
}

[[nodiscard]] std::string render(const Rate& rate) {
  return std::to_string(rate.numerator()) + "/" + std::to_string(rate.denominator());
}

[[nodiscard]] std::string render(const TimeRange& range) {
  return "[" + range.start.toString() + " +" + range.duration.toString() + "]";
}

[[nodiscard]] std::string render(const std::optional<EntityId>& id) {
  return id.has_value() ? id->toString() : std::string{"none"};
}

[[nodiscard]] std::string render(const TrackKind kind) {
  switch (kind) {
    case TrackKind::Video:
      return "video";
    case TrackKind::Audio:
      return "audio";
    case TrackKind::Caption:
      return "caption";
  }
  return "unknown";
}

[[nodiscard]] std::string render(const ClipKind kind) {
  switch (kind) {
    case ClipKind::Video:
      return "video";
    case ClipKind::Audio:
      return "audio";
    case ClipKind::Title:
      return "title";
  }
  return "unknown";
}

[[nodiscard]] std::string render(const BlendMode mode) {
  switch (mode) {
    case BlendMode::Normal:
      return "normal";
    case BlendMode::Add:
      return "add";
    case BlendMode::Multiply:
      return "multiply";
    case BlendMode::Screen:
      return "screen";
    case BlendMode::Overlay:
      return "overlay";
  }
  return "unknown";
}

[[nodiscard]] std::string render(const TransitionKind kind) {
  switch (kind) {
    case TransitionKind::CrossDissolve:
      return "cross-dissolve";
    case TransitionKind::DipToBlack:
      return "dip-to-black";
  }
  return "unknown";
}

void addField(std::vector<FieldChange>& out, std::string field, std::string before,
              std::string after) {
  out.push_back(FieldChange{std::move(field), std::move(before), std::move(after)});
}

[[nodiscard]] std::string labelFor(const std::string& name, const EntityId& id) {
  if (!name.empty()) {
    return name;
  }
  return id.toString();
}

// Track-level fields excluding the clip list, which is diffed separately.
[[nodiscard]] std::vector<FieldChange> trackFieldChanges(const Track& before, const Track& after) {
  std::vector<FieldChange> fields;
  if (before.name != after.name) {
    addField(fields, "name", before.name, after.name);
  }
  if (before.kind != after.kind) {
    addField(fields, "kind", render(before.kind), render(after.kind));
  }
  if (before.locked != after.locked) {
    addField(fields, "locked", render(before.locked), render(after.locked));
  }
  if (before.muted != after.muted) {
    addField(fields, "muted", render(before.muted), render(after.muted));
  }
  if (before.solo != after.solo) {
    addField(fields, "solo", render(before.solo), render(after.solo));
  }
  if (before.visible != after.visible) {
    addField(fields, "visible", render(before.visible), render(after.visible));
  }
  if (before.targeted != after.targeted) {
    addField(fields, "targeted", render(before.targeted), render(after.targeted));
  }
  if (before.audio_gain_db != after.audio_gain_db) {
    addField(fields, "audio_gain_db", render(before.audio_gain_db), render(after.audio_gain_db));
  }
  if (before.audio_pan != after.audio_pan) {
    addField(fields, "audio_pan", render(before.audio_pan), render(after.audio_pan));
  }
  if (before.effects != after.effects) {
    addField(fields, "effects", render(static_cast<std::uint32_t>(before.effects.size())),
             render(static_cast<std::uint32_t>(after.effects.size())));
  }
  return fields;
}

[[nodiscard]] std::vector<FieldChange> clipFieldChanges(const Clip& before, const Clip& after) {
  std::vector<FieldChange> fields;
  if (before.name != after.name) {
    addField(fields, "name", before.name, after.name);
  }
  if (before.kind != after.kind) {
    addField(fields, "kind", render(before.kind), render(after.kind));
  }
  if (before.asset_id != after.asset_id) {
    addField(fields, "asset", before.asset_id.toString(), after.asset_id.toString());
  }
  if (before.timeline_range.start != after.timeline_range.start) {
    addField(fields, "timeline_start", render(before.timeline_range.start),
             render(after.timeline_range.start));
  }
  if (before.timeline_range.duration != after.timeline_range.duration) {
    addField(fields, "timeline_duration", render(before.timeline_range.duration),
             render(after.timeline_range.duration));
  }
  if (before.source_range.start != after.source_range.start) {
    addField(fields, "source_start", render(before.source_range.start),
             render(after.source_range.start));
  }
  if (before.source_range.duration != after.source_range.duration) {
    addField(fields, "source_duration", render(before.source_range.duration),
             render(after.source_range.duration));
  }
  if (!(before.playback_rate == after.playback_rate)) {
    addField(fields, "playback_rate", render(before.playback_rate), render(after.playback_rate));
  }
  if (before.reversed != after.reversed) {
    addField(fields, "reversed", render(before.reversed), render(after.reversed));
  }
  if (before.linked_group != after.linked_group) {
    addField(fields, "linked_group", render(before.linked_group), render(after.linked_group));
  }
  if (!(before.blend_mode == after.blend_mode)) {
    addField(fields, "blend_mode", render(before.blend_mode), render(after.blend_mode));
  }
  if (before.audio_gain_db != after.audio_gain_db) {
    addField(fields, "audio_gain_db", render(before.audio_gain_db), render(after.audio_gain_db));
  }
  if (before.audio_pan != after.audio_pan) {
    addField(fields, "audio_pan", render(before.audio_pan), render(after.audio_pan));
  }
  if (before.fade_in != after.fade_in) {
    addField(fields, "fade_in", render(before.fade_in), render(after.fade_in));
  }
  if (before.fade_out != after.fade_out) {
    addField(fields, "fade_out", render(before.fade_out), render(after.fade_out));
  }
  if (!(before.transform == after.transform)) {
    addField(fields, "transform", "changed", "changed");
  }
  if (before.effects != after.effects) {
    addField(fields, "effects", render(static_cast<std::uint32_t>(before.effects.size())),
             render(static_cast<std::uint32_t>(after.effects.size())));
  }
  if (before.title != after.title) {
    addField(fields, "title", before.title.has_value() ? "present" : "none",
             after.title.has_value() ? "present" : "none");
  }
  return fields;
}

struct ClipLocation final {
  EntityId track_id{};
  const Clip* clip{nullptr};
};

// Maps every clip id in a sequence to its owning track and clip value, in
// (track order, clip order). The returned vector preserves that traversal
// order for deterministic reporting.
void indexClips(const Sequence& sequence, std::unordered_map<EntityId, ClipLocation>& by_id,
                std::vector<EntityId>& order) {
  for (const auto& track : sequence.tracks) {
    for (const auto& clip : track.clips) {
      by_id.insert_or_assign(clip.id, ClipLocation{track.id, &clip});
      order.push_back(clip.id);
    }
  }
}

// Diffs a list of id-bearing elements (markers, captions, transitions) by id.
// field_fn computes per-field changes for a matched pair; label_fn renders a
// human-readable label for an element.
template <typename Element, typename FieldFn, typename LabelFn>
void diffElementsByValue(const std::vector<Element>& before, const std::vector<Element>& after,
                         const FieldFn& field_fn, std::vector<SequenceElementChange>& out,
                         std::size_t& changed_count, const LabelFn& label_fn) {
  std::unordered_map<EntityId, const Element*> before_by_id;
  std::unordered_map<EntityId, const Element*> after_by_id;
  before_by_id.reserve(before.size());
  after_by_id.reserve(after.size());
  for (const auto& element : before) {
    before_by_id.insert_or_assign(element.id, &element);
  }
  for (const auto& element : after) {
    after_by_id.insert_or_assign(element.id, &element);
  }

  for (const auto& element : before) {
    if (!after_by_id.contains(element.id)) {
      out.push_back(SequenceElementChange{DiffChangeKind::Removed, element.id,
                                          labelFor(label_fn(element), element.id), {}});
      ++changed_count;
    }
  }
  for (const auto& element : after) {
    const auto found = before_by_id.find(element.id);
    if (found == before_by_id.end()) {
      out.push_back(SequenceElementChange{DiffChangeKind::Added, element.id,
                                          labelFor(label_fn(element), element.id), {}});
      ++changed_count;
      continue;
    }
    auto fields = field_fn(*found->second, element);
    if (!fields.empty()) {
      out.push_back(SequenceElementChange{DiffChangeKind::Modified, element.id,
                                          labelFor(label_fn(element), element.id),
                                          std::move(fields)});
      ++changed_count;
    }
  }
}

[[nodiscard]] std::vector<FieldChange> markerFieldChanges(const Marker& before,
                                                          const Marker& after) {
  std::vector<FieldChange> fields;
  if (before.label != after.label) {
    addField(fields, "label", before.label, after.label);
  }
  if (!(before.range == after.range)) {
    addField(fields, "range", render(before.range), render(after.range));
  }
  if (!(before.color == after.color)) {
    addField(fields, "color", "changed", "changed");
  }
  return fields;
}

[[nodiscard]] std::vector<FieldChange> captionFieldChanges(const Caption& before,
                                                           const Caption& after) {
  std::vector<FieldChange> fields;
  if (before.text != after.text) {
    addField(fields, "text", before.text, after.text);
  }
  if (!(before.range == after.range)) {
    addField(fields, "range", render(before.range), render(after.range));
  }
  if (before.language != after.language) {
    addField(fields, "language", before.language, after.language);
  }
  if (!(before.style == after.style)) {
    addField(fields, "style", "changed", "changed");
  }
  if (before.words != after.words) {
    addField(fields, "words", render(static_cast<std::uint32_t>(before.words.size())),
             render(static_cast<std::uint32_t>(after.words.size())));
  }
  return fields;
}

[[nodiscard]] std::vector<FieldChange> transitionFieldChanges(const Transition& before,
                                                              const Transition& after) {
  std::vector<FieldChange> fields;
  if (!(before.kind == after.kind)) {
    addField(fields, "kind", render(before.kind), render(after.kind));
  }
  if (!(before.range == after.range)) {
    addField(fields, "range", render(before.range), render(after.range));
  }
  if (before.enabled != after.enabled) {
    addField(fields, "enabled", render(before.enabled), render(after.enabled));
  }
  if (before.outgoing_clip_id != after.outgoing_clip_id) {
    addField(fields, "outgoing_clip", before.outgoing_clip_id.toString(),
             after.outgoing_clip_id.toString());
  }
  if (before.incoming_clip_id != after.incoming_clip_id) {
    addField(fields, "incoming_clip", before.incoming_clip_id.toString(),
             after.incoming_clip_id.toString());
  }
  return fields;
}

void appendSequenceFieldChanges(const Sequence& before, const Sequence& after,
                                std::vector<FieldChange>& fields) {
  if (before.name != after.name) {
    addField(fields, "name", before.name, after.name);
  }
  if (!(before.frame_rate == after.frame_rate)) {
    addField(fields, "frame_rate", render(before.frame_rate), render(after.frame_rate));
  }
  if (before.width != after.width) {
    addField(fields, "width", render(before.width), render(after.width));
  }
  if (before.height != after.height) {
    addField(fields, "height", render(before.height), render(after.height));
  }
  if (before.audio_sample_rate != after.audio_sample_rate) {
    addField(fields, "audio_sample_rate", render(before.audio_sample_rate),
             render(after.audio_sample_rate));
  }
}

} // namespace

bool SequenceDiff::identical() const noexcept {
  return sequence_fields.empty() && tracks.empty() && clips.empty() && markers.empty() &&
         captions.empty() && transitions.empty();
}

SequenceDiff diffSequences(const Sequence& before, const Sequence& after) {
  SequenceDiff diff;

  appendSequenceFieldChanges(before, after, diff.sequence_fields);

  // Tracks: match by id, detect add/remove/reorder/field edits.
  std::unordered_map<EntityId, std::pair<std::size_t, const Track*>> before_tracks;
  std::unordered_map<EntityId, std::pair<std::size_t, const Track*>> after_tracks;
  before_tracks.reserve(before.tracks.size());
  after_tracks.reserve(after.tracks.size());
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    before_tracks.insert_or_assign(before.tracks[index].id,
                                   std::make_pair(index, &before.tracks[index]));
  }
  for (std::size_t index = 0; index < after.tracks.size(); ++index) {
    after_tracks.insert_or_assign(after.tracks[index].id,
                                  std::make_pair(index, &after.tracks[index]));
  }
  for (std::size_t index = 0; index < before.tracks.size(); ++index) {
    const auto& track = before.tracks[index];
    if (!after_tracks.contains(track.id)) {
      SequenceTrackChange change;
      change.kind = DiffChangeKind::Removed;
      change.track_id = track.id;
      change.label = labelFor(track.name, track.id);
      change.before_index = index;
      diff.tracks.push_back(std::move(change));
      ++diff.summary.tracks_removed;
    }
  }
  for (std::size_t index = 0; index < after.tracks.size(); ++index) {
    const auto& track = after.tracks[index];
    const auto found = before_tracks.find(track.id);
    if (found == before_tracks.end()) {
      SequenceTrackChange change;
      change.kind = DiffChangeKind::Added;
      change.track_id = track.id;
      change.label = labelFor(track.name, track.id);
      change.after_index = index;
      diff.tracks.push_back(std::move(change));
      ++diff.summary.tracks_added;
      continue;
    }
    const std::size_t before_index = found->second.first;
    auto fields = trackFieldChanges(*found->second.second, track);
    const bool reordered = before_index != index;
    if (!fields.empty() || reordered) {
      SequenceTrackChange change;
      change.kind = DiffChangeKind::Modified;
      change.track_id = track.id;
      change.label = labelFor(track.name, track.id);
      change.before_index = before_index;
      change.after_index = index;
      change.fields = std::move(fields);
      diff.tracks.push_back(std::move(change));
      ++diff.summary.tracks_modified;
    }
  }

  // Clips: match by id across the whole sequence so cross-track moves are
  // reported as edits rather than an add/remove pair.
  std::unordered_map<EntityId, ClipLocation> before_clips;
  std::unordered_map<EntityId, ClipLocation> after_clips;
  std::vector<EntityId> before_order;
  std::vector<EntityId> after_order;
  indexClips(before, before_clips, before_order);
  indexClips(after, after_clips, after_order);

  for (const auto& clip_id : before_order) {
    if (!after_clips.contains(clip_id)) {
      const auto& location = before_clips.at(clip_id);
      SequenceClipChange change;
      change.kind = DiffChangeKind::Removed;
      change.clip_id = clip_id;
      change.label = labelFor(location.clip->name, clip_id);
      change.before_track = location.track_id;
      diff.clips.push_back(std::move(change));
      ++diff.summary.clips_removed;
    }
  }
  for (const auto& clip_id : after_order) {
    const auto& after_location = after_clips.at(clip_id);
    const auto found = before_clips.find(clip_id);
    if (found == before_clips.end()) {
      SequenceClipChange change;
      change.kind = DiffChangeKind::Added;
      change.clip_id = clip_id;
      change.label = labelFor(after_location.clip->name, clip_id);
      change.after_track = after_location.track_id;
      diff.clips.push_back(std::move(change));
      ++diff.summary.clips_added;
      continue;
    }
    const auto& before_location = found->second;
    auto fields = clipFieldChanges(*before_location.clip, *after_location.clip);
    const bool moved = before_location.track_id != after_location.track_id;
    if (moved) {
      addField(fields, "track", before_location.track_id.toString(),
               after_location.track_id.toString());
    }
    if (!fields.empty()) {
      SequenceClipChange change;
      change.kind = DiffChangeKind::Modified;
      change.clip_id = clip_id;
      change.label = labelFor(after_location.clip->name, clip_id);
      change.before_track = before_location.track_id;
      change.after_track = after_location.track_id;
      change.fields = std::move(fields);
      diff.clips.push_back(std::move(change));
      ++diff.summary.clips_modified;
      if (moved) {
        ++diff.summary.clips_moved;
      }
    }
  }

  diffElementsByValue(before.markers, after.markers, markerFieldChanges, diff.markers,
                      diff.summary.markers_changed, [](const Marker& m) { return m.label; });
  diffElementsByValue(before.captions, after.captions, captionFieldChanges, diff.captions,
                      diff.summary.captions_changed, [](const Caption& c) { return c.text; });
  diffElementsByValue(before.transitions, after.transitions, transitionFieldChanges,
                      diff.transitions, diff.summary.transitions_changed,
                      [](const Transition&) { return std::string{}; });

  return diff;
}

std::string summarize(const SequenceDiff& diff) {
  if (diff.identical()) {
    return "No changes.\n";
  }
  std::string out;
  const auto line = [&out](const std::string& text) {
    out += text;
    out.push_back('\n');
  };
  const auto renderFields = [](const std::vector<FieldChange>& fields) {
    std::string text;
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (index != 0) {
        text += ", ";
      }
      text += fields[index].field + ": " + fields[index].before + " -> " + fields[index].after;
    }
    return text;
  };
  const auto kindLabel = [](const DiffChangeKind kind) -> std::string {
    switch (kind) {
      case DiffChangeKind::Added:
        return "added";
      case DiffChangeKind::Removed:
        return "removed";
      case DiffChangeKind::Modified:
        return "modified";
    }
    return "changed";
  };

  for (const auto& change : diff.sequence_fields) {
    line("sequence " + change.field + ": " + change.before + " -> " + change.after);
  }
  for (const auto& change : diff.tracks) {
    std::string text = "track " + kindLabel(change.kind) + " '" + change.label + "'";
    if (!change.fields.empty()) {
      text += " (" + renderFields(change.fields) + ")";
    } else if (change.kind == DiffChangeKind::Modified && change.before_index &&
               change.after_index) {
      text += " (reordered " + std::to_string(*change.before_index) + " -> " +
              std::to_string(*change.after_index) + ")";
    }
    line(text);
  }
  for (const auto& change : diff.clips) {
    std::string text = "clip " + kindLabel(change.kind) + " '" + change.label + "'";
    if (!change.fields.empty()) {
      text += " (" + renderFields(change.fields) + ")";
    }
    line(text);
  }
  for (const auto& change : diff.markers) {
    line("marker " + kindLabel(change.kind) + " '" + change.label + "'");
  }
  for (const auto& change : diff.captions) {
    line("caption " + kindLabel(change.kind) + " '" + change.label + "'");
  }
  for (const auto& change : diff.transitions) {
    line("transition " + kindLabel(change.kind));
  }
  return out;
}

} // namespace video_editor::edit
