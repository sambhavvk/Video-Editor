// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/entity_id.h"
#include "video_editor/edit_model/model.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::edit {

// Structured, deterministic comparison of two versions of the same sequence.
//
// This powers a "what changed since the client-approved cut?" review view.
// The diff is a pure function of its inputs: entities are matched by their
// stable EntityId so that renames, moves, trims, and property edits are
// reported precisely rather than as unrelated add/remove pairs. Output order
// is deterministic (additions and modifications follow "after" order; removals
// follow "before" order) so a report can be rendered or asserted reliably.

enum class DiffChangeKind { Added, Removed, Modified };

// A single human-readable property change. Values are rendered with the
// model's own formatting helpers so callers do not need to know field types.
struct FieldChange final {
  std::string field;
  std::string before;
  std::string after;
  friend bool operator==(const FieldChange&, const FieldChange&) = default;
};

struct SequenceTrackChange final {
  DiffChangeKind kind{DiffChangeKind::Modified};
  EntityId track_id{};
  // Descriptive label: the track's name in "after" for additions and
  // modifications, or its name in "before" for removals.
  std::string label;
  // Position of the track within the sequence. For a pure reorder, the index
  // fields differ while the field list is empty.
  std::optional<std::size_t> before_index;
  std::optional<std::size_t> after_index;
  std::vector<FieldChange> fields;
  friend bool operator==(const SequenceTrackChange&, const SequenceTrackChange&) = default;
};

struct SequenceClipChange final {
  DiffChangeKind kind{DiffChangeKind::Modified};
  EntityId clip_id{};
  std::string label;
  // The owning track for the clip in each version. A clip that changes tracks
  // is reported as Modified with a "track" field change.
  std::optional<EntityId> before_track;
  std::optional<EntityId> after_track;
  std::vector<FieldChange> fields;
  friend bool operator==(const SequenceClipChange&, const SequenceClipChange&) = default;
};

// Markers, captions, and transitions are compared by id as whole values.
struct SequenceElementChange final {
  DiffChangeKind kind{DiffChangeKind::Modified};
  EntityId id{};
  std::string label;
  std::vector<FieldChange> fields;
  friend bool operator==(const SequenceElementChange&, const SequenceElementChange&) = default;
};

struct SequenceDiffSummary final {
  std::size_t tracks_added{0};
  std::size_t tracks_removed{0};
  std::size_t tracks_modified{0};
  std::size_t clips_added{0};
  std::size_t clips_removed{0};
  std::size_t clips_modified{0};
  std::size_t clips_moved{0};
  std::size_t markers_changed{0};
  std::size_t captions_changed{0};
  std::size_t transitions_changed{0};
  friend bool operator==(const SequenceDiffSummary&, const SequenceDiffSummary&) = default;
};

struct SequenceDiff final {
  // Sequence-level properties (name, frame rate, resolution, audio rate).
  std::vector<FieldChange> sequence_fields;
  std::vector<SequenceTrackChange> tracks;
  std::vector<SequenceClipChange> clips;
  std::vector<SequenceElementChange> markers;
  std::vector<SequenceElementChange> captions;
  std::vector<SequenceElementChange> transitions;
  SequenceDiffSummary summary{};

  // True when the two sequences are editorially identical (byte-for-byte equal
  // on every compared field). Entity id equality of the sequences themselves is
  // not required; comparing two named versions of one cut is the intended use.
  [[nodiscard]] bool identical() const noexcept;
};

[[nodiscard]] SequenceDiff diffSequences(const Sequence& before, const Sequence& after);

// A compact multi-line, human-readable summary suitable for a review panel or
// a changelog entry. Deterministic and stable across runs.
[[nodiscard]] std::string summarize(const SequenceDiff& diff);

} // namespace video_editor::edit
