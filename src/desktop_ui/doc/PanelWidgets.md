<!-- SPDX-License-Identifier: MPL-2.0 -->

# Authoring, mixer, and delivery panel widgets

Header: `video_editor/desktop_ui/panel_widgets.hpp`

Namespace: `video_editor::desktop_ui`

## MediaBinWidget

`MediaBinWidget` is a searchable table of `MediaItemView` rows: thumbnail, name, duration, format,
and status. A non-empty `metadataTitle` is the visible name; `filePath` remains the tooltip. Status
text is paired with color (`Offline`, `Changed`, proxy lifecycle, or `Original`). Relink is enabled
for offline or content-changed items. `mediaSelectionChanged` reports the current row identity.

## Inspector and effects

`InspectorWidget` presents clip transform/audio/title/speed state and typed effect parameters. The
controller supplies values with `setParameter` and a complete curve view with
`setEffectParameters`. Effect signals distinguish base-value edits, keyframe toggle/selection,
exact time/value, interpolation, deletion, and Bezier-control edits. All times are presentation
ticks and are converted to clip-local edit-model `Time` by the controller.

`setAssetMetadata` / `clearAssetMetadata` drive the Asset group (title, comma-separated tags, notes,
rating 0–5). An empty `assetId` hides the group. Field edits emit a complete `AssetMetadataView`.
`clearSelection()` also clears asset metadata.

`EffectsPanelWidget` owns a searchable list of `EffectView` presets and emits activation/add intent.
It does not instantiate a canonical effect itself.

## AudioMixerWidget

`setTracks` rebuilds owned strip views containing gain, pan, mute, solo, and current effect values.
Track signals report mixer and effect intent by presentation index and stable effect ID.
`setMasterMeter` updates master peak/RMS and truthfully labels authoritative integrated EBU-R128 as
live, analyzing, stale, or inactive. `setTrackMeters` maps post-DSP peak/RMS by stable track ID so
reorder/removal cannot attach a reading to the wrong strip; inactive and uncovered ranges are
disabled. `setMeterLevels` remains a compatibility entry point.

`setOutputDevices` replaces stable IDs/names, current selection, availability, and status. Selection
emits an ID, with the empty ID representing **System default**; actual device opening and recovery
remain application responsibilities. Normalization methods publish the editable −24 through −9
LUFS target plus busy, review, and failure state. Apply remains disabled until the controller
supplies a valid current-generation review.

## DeliverPanelWidget

`loadPlatformPresets` fills reference-master and capability-gated creator presets using lightweight
encoder-presence checks; actual driver/device validation is deferred to the export worker. Query methods
return the selected preset, destination, even output dimensions, rational frame rate, video/audio
bitrate, optional VP9 quality, hardware preference, and caption/sidecar modes. Browse and export are
separate signals; while running, the export action becomes cancellation.

The panel reports encoder availability but does not choose an FFmpeg encoder or perform I/O.

## CaptionsPanelWidget

`setCaptionRows` accepts owned `CaptionRowView` values with stable cue/word IDs, exact presentation
ticks, probability, provenance-facing suggestion state, and canonical style. Selecting a cue
rebuilds its word list; activating a word emits its stable ID and exact start tick for transcript
navigation. Legacy parallel timecode/text rows remain a compatibility entry point.

`setTranscriptionState`, `setModelDownloadState`, and `setTranscriptionOptions` expose explicit
model-missing/downloading/ready/running/cancelling/failure states without performing network or
worker work. Model download, one-job process lifecycle, and revision checks belong to the
application controller. `setReviewProposals` presents independently checkable items and emits apply,
discard, and selection intent; it never mutates the timeline itself.

Style controls emit a complete `CaptionStyleView` for the selected stable caption ID. Alignment,
vertical position, safe margin, font request, size, colors, emphasis, and outline are presentation
values; the controller validates and commits them through the canonical caption command.

## CacheBrowserDialog

`CacheBrowserDialog` is a modal inventory of rebuildable cache artifacts. `setInventory` replaces
used/budget bytes and the Name/Kind/Size/Last accessed table. Budget changes emit `budgetChanged`.
Remove selected, remove asset, evict unused, and clear-all (after confirmation) emit intent only;
the controller mutates `CacheStore`. Clearing never claims to delete projects or originals.

## Accessibility

Primary interactive controls set `accessibleName` (and often `accessibleDescription`) with stable
user-facing strings: media search is `Search media`, mixer faders are `Gain for <track>`, caption
add is `Add a caption at the playhead`, and deliver export is `Export video master`. Analyze/Apply
loudness buttons are `Analyze loudness` and `Apply loudness normalization`. Qt-internal clear and
spin arrows are unnamed by design.

## Ownership and thread safety

All widgets are QObject-parent-owned and GUI-thread only. View objects and strings are copied. They
emit user intent; `EditorController` owns asynchronous work and revision-checked mutation.

AI assistance has been used to create this output.
