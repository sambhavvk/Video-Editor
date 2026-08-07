// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/commands.h"
#include "video_editor/edit_model/result.h"

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace video_editor::edit {

enum class EditErrorCode {
  RevisionConflict,
  RevisionNotFound,
  EntityNotFound,
  DuplicateId,
  InvalidArgument,
  InvalidTrackKind,
  TrackLocked,
  Overlap,
  AssetInUse,
  NothingToUndo,
  NothingToRedo,
  ArithmeticOverflow,
};

struct EditError final {
  EditErrorCode code{EditErrorCode::InvalidArgument};
  std::string message;
  std::optional<Revision> expected_revision;
  std::optional<Revision> actual_revision;
};

class TimelineSnapshot final {
 public:
  TimelineSnapshot() = default;

  [[nodiscard]] Revision revision() const noexcept { return revision_; }
  [[nodiscard]] const Project& project() const;
  [[nodiscard]] const Sequence& sequence() const;
  [[nodiscard]] const Track* findTrack(EntityId id) const noexcept;
  [[nodiscard]] const Clip* findClip(EntityId id) const noexcept;
  [[nodiscard]] Time duration() const;
  [[nodiscard]] std::vector<Gap> gaps(EntityId track_id,
                                      std::optional<Time> end = std::nullopt) const;

 private:
  friend class TimelineEditor;
  TimelineSnapshot(Revision revision, std::shared_ptr<const Project> project,
                   EntityId sequence_id);

  Revision revision_{};
  std::shared_ptr<const Project> project_;
  EntityId sequence_id_{};
};

struct HistoryEntryView final {
  std::string command_name;
  std::string coalescing_key;
  std::size_t command_count{1};
};

class TimelineEditor final {
 public:
  explicit TimelineEditor(Project initial_project = Project{});
  ~TimelineEditor();
  TimelineEditor(const TimelineEditor&) = delete;
  TimelineEditor& operator=(const TimelineEditor&) = delete;

  [[nodiscard]] Revision revision() const noexcept;
  [[nodiscard]] Result<Revision, EditError> apply(EditCommand command,
                                                  Revision expected_revision);
  [[nodiscard]] Result<Revision, EditError> undo(Revision expected_revision);
  [[nodiscard]] Result<Revision, EditError> redo(Revision expected_revision);

  [[nodiscard]] bool canUndo() const noexcept;
  [[nodiscard]] bool canRedo() const noexcept;
  [[nodiscard]] std::vector<HistoryEntryView> history() const;

  [[nodiscard]] Result<TimelineSnapshot, EditError> snapshot(
      EntityId sequence_id, Revision revision) const;
  [[nodiscard]] std::shared_ptr<const Project> projectAt(
      Revision revision) const;

 private:
  struct HistoryEntry;

  [[nodiscard]] Result<Revision, EditError> staleRevision(
      Revision expected) const;
  [[nodiscard]] Result<Revision, EditError> commitState(
      std::shared_ptr<const Project> next_state);

  mutable std::shared_mutex mutex_;
  Revision revision_{};
  std::shared_ptr<const Project> state_;
  std::unordered_map<std::uint64_t, std::shared_ptr<const Project>> revisions_;
  std::vector<HistoryEntry> history_;
  std::size_t history_cursor_{0};
};

}  // namespace video_editor::edit
