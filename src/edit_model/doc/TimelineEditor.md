<!-- SPDX-License-Identifier: MPL-2.0 -->

# TimelineEditor

Header: `video_editor/edit_model/timeline_editor.h`

Namespace: `video_editor::edit`

## Class overview

`TimelineEditor` is the dependency-free authority that validates edit commands, publishes immutable
project revisions, and owns undo/redo history. Use it whenever canonical project state must change.
It accepts one command or an atomic batch with the caller's expected revision.

## Related public types

| Type | Description |
| --- | --- |
| `EditErrorCode` | Stable failure category including conflicts, missing/duplicate entities, invalid arguments, locks, overlaps, history limits, and arithmetic overflow. |
| `EditError` | Error category/message plus optional expected and actual revisions. |
| `TimelineSnapshot` | Immutable revision/sequence view with project, track, clip, duration, and derived-gap queries. |
| `HistoryEntryView` | Read-only history label, coalescing key, and logical command count. |

## Construction and lifecycle

### `explicit TimelineEditor(Project initial_project = Project{})`

Validates and owns the initial project as revision zero. Invalid input throws `std::invalid_argument`.

### `~TimelineEditor()`

Releases retained revisions and history. Copy construction and copy assignment are deleted.

## Public methods

### `Revision revision() const noexcept`

Returns the current head revision under a shared lock.

### `Result<Revision, EditError> apply(EditCommand command, Revision expected_revision)`

Applies and validates one command against a candidate copy. Success publishes one revision; stale
or invalid input publishes nothing. Adjacent equal nonempty coalescing keys share one undo entry.

### `Result<Revision, EditError> applyBatch(std::vector<EditCommand> commands, Revision expected_revision, std::string batch_name, std::optional<std::string> coalescing_key = std::nullopt)`

Applies the ordered command list to one candidate and validates the complete result. Success
publishes one revision/history entry named by `batch_name`; any failed member, invalid final state,
empty list, invalid name, or stale revision leaves state/history unchanged.

### `Result<Revision, EditError> undo(Revision expected_revision)`

Publishes the previous history state as a new revision. One atomic batch is one undo step.

### `Result<Revision, EditError> redo(Revision expected_revision)`

Publishes the next retained history state as a new revision.

### `bool canUndo() const noexcept`

Reports whether an undo state exists.

### `bool canRedo() const noexcept`

Reports whether a redo state exists.

### `std::vector<HistoryEntryView> history() const`

Returns an owned read-only description of retained logical history entries.

### `Result<TimelineSnapshot, EditError> snapshot(EntityId sequence_id, Revision revision) const`

Returns an immutable view for a retained revision and sequence, or an error if either is absent.

### `std::shared_ptr<const Project> projectAt(Revision revision) const`

Returns shared ownership of a retained immutable project or null for an unknown revision.

## TimelineSnapshot methods

`project()` and `sequence()` return const references owned by the snapshot. `findTrack()` and
`findClip()` return borrowed pointers. `duration()` returns exact sequence duration. `gaps()`
derives half-open empty ranges for one track through an optional exact end; gaps are not entities.

## Ownership and thread safety

The editor owns immutable project states through `std::shared_ptr<const Project>`. Mutation,
history, undo, and redo take an exclusive lock. Revision and snapshot/history reads take a shared
lock. A snapshot or `projectAt` result extends its project lifetime independently of later edits.
No Qt thread affinity applies.

## Usage example

```cpp
edit::TimelineEditor editor{project};
auto result = editor.apply(
    edit::EditCommand{edit::SetTrackLockedCommand{sequence_id, track_id, true}, {}},
    editor.revision());
if (result) {
  auto snapshot = editor.snapshot(sequence_id, result.value());
}
```

AI assistance has been used to create this output.
