<!-- SPDX-License-Identifier: MPL-2.0 -->

# User guide for the current desktop build

This guide describes behavior implemented today. The application is an engineering preview, not a
public beta. Save important work often and keep the original media files available.

## Important limitations

- The desktop first requests an engine GPU path for active video clips through libplacebo, applying
  crop, position, scale, a custom anchor or centered-pivot rotation, opacity, and Normal composition,
  then downloads the offscreen result for the viewer. Rotation around a moved pivot, effects, title
  clips, active transitions, and non-Normal blends use CPU for that frame without disabling future
  GPU attempts.
  Backend/device failures preserve the CPU frame and latch CPU preview for the session. This is not
  native swapchain presentation, zero-copy decode, or a full effects/color graph.
- Forward 1× transport uses optional miniaudio 48 kHz stereo playback when the default output opens.
  Its playhead uses a latency-compensated audio-master position, not the end of the submitted device
  buffer; the remaining latency is reported as uncertainty. Otherwise it clearly falls back to
  silent timer-driven video. Reverse and shuttle rates other than 1× are silent.
- Export includes deterministic 48 kHz stereo PCM audio from originals. It does **not** burn in or
  embed captions and is not a creator-delivery H.264/AAC preset.
- Only FFV1/Matroska and, when the encoder is available, ProRes 422 HQ/MOV masters are offered.
- The selected-clip Inspector can change the properties listed below. Effects-browser, keyframe,
  and source-monitor editing controls are not connected; mixer track faders and realtime meters
  are not active. The precision timeline tools are connected.
- Canonical title and transition state is saved and CPU-rendered, but the current desktop has no
  title or transition creation/editing surface. Existing schema-v2 state can be rendered and
  round-tripped; this guide does not imply an authoring workflow that is not present.
- Local transcription is not implemented; the button only explains the model requirement.
- Relinking, persistent cache management, thumbnails, and waveforms are not complete workflows.
- Project and internal schema compatibility are pre-beta and may change through migrations.

See the [full status matrix](beta-feature-status.md) before relying on a capability.

## Workspaces

The same project and timeline are shown through four layouts; switching workspaces does not create
a different editing mode.

- **Import** emphasizes the media bin and program viewer.
- **Edit** emphasizes the timeline, inspector, and effects browser.
- **Audio & Captions** shows the mixer and caption/transcript panel.
- **Deliver** shows the current reference-master export controls.

Dock panels can be rearranged with normal Qt docking behavior. The command palette searches the
registered application commands and their shortcuts.

## Create, open, and save a project

1. Choose **File > New Project**. A new project starts with two video and four audio tracks.
2. Choose **File > Save Project As** and select a `.veproj` path.
3. Continue with **Save Project**. A completed save creates a self-contained SQLite checkpoint;
   media and caches are not embedded.
4. Use **File > Open Project** to open a checkpoint. The application copies it to a local working
   database and edits that copy.

Every accepted edit is journaled to the local working database. If the application did not close
cleanly or has committed edits newer than its last checkpoint, the next startup scans the recovery
directory and can offer the most recent valid candidate. Choosing recovery opens the latest
committed snapshot and marks the project dirty; save it to a `.veproj` to retain it. See
[Project format and recovery](reference/project-format-and-recovery.md).

## Import and insert media

Import using **File > Import Media**, the media-bin Import button, a command-line media argument,
or by dropping files on the program viewer. Import runs asynchronously and probes each file with
FFmpeg. Failed items are reported without discarding successful imports.

The media bin shows name, duration, detected format, and status. Search filters by name or format.
Double-click an item to insert it at the playhead:

- Video goes to the first unlocked targeted video track.
- Audio goes to the first unlocked targeted audio track.
- An asset containing both creates linked video and audio clips as one atomic undoable revision.
- The insertion is rejected when it would overlap an existing clip; it does not silently replace
  existing work.
- The first inserted video clip derives sequence width, height, and nominal frame rate from the
  asset when those values are available. The sequence audio sample rate remains 48 kHz.

The application references the original path. Moving or deleting that file can make the asset
unavailable; the visible Relink command is not connected yet.

## Edit the timeline

Click a clip to replace the selection. **Ctrl+click** toggles one clip and **Shift+click** selects the
deterministic range from the selection anchor. Dragging any selected clip moves the selection as
one atomic edit; only the dragged active clip may change tracks, while the others retain their
tracks. A pointer gesture displays a transient preview and commits one revision when released.
Press **Escape** during a drag to cancel it. Dragging near a viewport edge auto-scrolls.

Snapping is resolved by the exact edit model, not a separate visual approximation. It considers the
playhead, marker boundaries, other clip edges, and the sequence frame grid with deterministic tie
priority. Moving clips and markers are excluded from their own candidates. Hold **Shift** during a
drag to disable snapping.

Choose a timeline tool from the toolbar/menu or its shortcut:

- **V — Select:** move clip bodies; drag an edge for a normal trim.
- **R — Ripple Trim:** drag an edge and shift following material by the exact duration change.
- **W — Overwrite Trim:** drag an edge without moving later positions; covered material is removed
  or edge-trimmed.
- **N — Roll:** drag the shared edge between adjacent clips while preserving the outer span.
- **Y — Slip:** drag a clip body to change its source window without changing timeline position.
- **U — Slide:** drag a middle clip body and trim its immediate neighbors.

Control requests ripple intent and Alt requests overwrite intent for compatible Select-tool
gestures. Invalid destinations, overlaps, missing source handles, non-adjacent precision edits, and
locked selected/linked participants reject the complete batch and restore the authoritative view.

Other editing operations:

- **Split Clip** splits every selected clip at the playhead and automatically includes linked A/V;
  all right-half IDs are explicit and the complete operation is atomic.
- **Delete** removes the selected clips and their linked A/V while leaving gaps.
- **Ripple Delete** removes them and closes each affected track independently.
- Undo and redo treat a multi-selection or linked operation as one step.
- **Alt+Left/Right** nudges the selection by one exact sequence frame; add **Shift** for ten frames
  and **Ctrl** for ripple intent. Relative offsets inside a multi-selection are preserved.

### Tracks, markers, and gaps

Track headers show lock, output visibility, and target state with text/icons in addition to color.
Click those controls to toggle them. Right-click a track header for Add Video/Audio/Caption Track,
Rename, Move Up/Down, Lock, Visible, Target, and Remove. **Insert** adds a video track,
**Shift+Insert** adds an audio track, **Ctrl+Insert** adds a caption track, and **Ctrl+Up/Down**
reorders the active track. With a track header active, **L**, **V**, and **T** toggle lock,
visibility, and targeting. Locked tracks reject structural edits. Hidden video tracks do not
contribute to preview or export. Targeted, unlocked compatible tracks receive media insertion.

Double-click the ruler or press **M** to add a point marker. Click/drag markers to select and move
them through canonical snapping; their context menu offers Rename and Remove.

Empty track regions are derived gaps rather than stored media. Click a visible gap and press
**Delete/Backspace**, or use **Close Gap** from its context menu, to shift later material left.
Closing a stale, partial, locked-track, or terminal gap is rejected safely.

## Adjust selected-clip properties

Selecting a video or title clip reveals revisioned Inspector controls for Position X/Y, uniform
Scale, Rotation, and Opacity. The expanded controls offer Normal, Add, Multiply, Screen, and Overlay
blend modes plus exact Scale X/Y, Anchor X/Y, and Crop Left/Top/Right/Bottom. These values are
undoable, persist in `.veproj`, and are rendered by the deterministic CPU preview/export path. Crop
values must leave a positive image area. The underlying model/renderer can represent signed scale
for flips, but the current Inspector exposes positive scale values only.

Selecting an audio clip reveals Gain (-96 to +24 dB), Pan, Fade In, and Fade Out. Those edits are
validated, undoable, persistent, and used by realtime forward playback and reference export.

The Audio Mixer's **M** and **S** buttons change track mute and solo state. These changes are
revisioned, persistent, undoable, and consumed by offline timeline audio render. Track faders are
intentionally disabled because track gain/pan is not implemented; select an audio clip and use its
Inspector Gain/Pan controls instead. The meter strips do not show live levels yet.

Adjacent Inspector changes to the same property and clip use one coalescing key so continuous
spin-box adjustment collapses in undo history. Keyframe diamonds and interpolation controls are not
wired, so the current properties are constant over the clip.

## Preview and transport

The program viewer requests frames asynchronously from the current immutable timeline snapshot.
New seeks use a newer request epoch so stale decode work cannot replace the latest viewer image.
The CPU path seeks from the preceding keyframe and decodes in presentation order.

The render engine requests only active video clips and composes supported crop, position, scale,
custom anchor or centered-pivot rotation, opacity, and Normal source-over on a compatible D3D11
(Windows) or Vulkan (Linux) libplacebo device. The desktop tries this before the deterministic CPU
renderer and downloads the offscreen GPU image for Qt. Rotation around a moved pivot, enabled
effects, title clips, active transitions, and non-Normal blend modes fall back to CPU for just that
frame; initialization, upload, composite, readback, or device failure preserves the CPU frame,
reports a local diagnostic, and
disables later GPU attempts until restart. The Program title and one transient message identify an
active backend. The path provides neither zero-copy decode, HDR tone mapping, native presentation,
nor a full GPU effects/color graph.

Current transport supports reverse, stop, forward, play/pause, single-frame movement, ruler seeking,
and J/K/L shuttle stepping. For forward 1× playback, a miniaudio-enabled build renders exact ranges
from an immutable timeline snapshot on a worker, prefills a bounded ring, and starts the default
output. The audio callback records the submitted position, while the playhead uses a conservative
latency-compensated position and exposes output latency as uncertainty; pause, resume, and seek
update that same clock. Qt actions enqueue versioned commands on a serialized background-control
thread, so media prefill does not block the UI. Video holds position while start/seek is pending and
follows the audio master only after successful completion. A completed edit or project replacement
enqueues stop for the old revision's playback.

The viewer keeps one preview render in flight during transport. Timer ticks coalesce to the newest
playhead instead of repeatedly canceling the active decode, so slower CPU/GPU frames continue to be
presented while playback is running. A direct seek or edit still cancels obsolete work so the exact
requested frame wins.

If the adapter is not compiled, the default device cannot open/start, the renderer cannot prefill,
or the device later stops, the application reports the reason and continues with silent
timer-driven video. Reverse and non-1× shuttle are also silent. An underrun inserts silence while
the master counter continues and reports a local warning; this prevents a backwards clock jump but
does not make the present build a calibrated A/V-sync reference. Physical-device latency calibration,
one-hour xrun, and two-hour drift tests still gate beta.

If a development build has no sound at forward 1×, check its CMake configure output first. The
phrase `miniaudio adapter unavailable; manual callback fallback enabled` means that executable has
no physical audio backend and must be reconfigured with the pinned miniaudio 0.11.25 header. This is
a build capability, not an audio-mixer or operating-system volume setting.

## Create and exchange captions

Open **Audio & Captions** to:

- import UTF-8 SRT or WebVTT;
- add a two-second `New caption` cue at the playhead;
- edit caption text directly in the table;
- delete the selected caption;
- search caption text and double-click a result to seek to it;
- export the sequence captions as SRT or WebVTT.

Import validates timing, order, overlap, cue text, and UTF-8. All cues from one import form a single
undoable edit batch. Export writes atomically and rounds timing to the nearest millisecond because
SRT and WebVTT timestamps are millisecond-based. Caption styling, word timing, reflow UI,
transcription, and burn-in are not integrated.

## Create an editing proxy

For a media-bin item marked **Proxy recommended**, right-click and choose **Create editing proxy**.
The same menu cancels an active generation. The current in-process background job creates a
half-resolution ProRes Proxy/MOV with 48 kHz PCM audio when available, or uses the predetermined
FFV1/Matroska fallback when configured encoders require it. Completion updates playback to permit
the proxy; export still uses the original.

The generated proxy and `.vepts` timestamp map live in the application cache, not the project.
Cancellation and failures do not commit a partial destination. Proxy association is currently
session-local: reopening the project does not rediscover a previously generated proxy. There is no
cache browser, size budget, or LRU eviction yet. See [Media, proxies, and cache](reference/media-proxies-and-cache.md).

## Export a video master

1. Add at least one clip to the sequence.
2. Open **Deliver** and choose FFV1/Matroska or ProRes 422 HQ/MOV.
3. Choose **Export master** and a destination.
4. Use the same button to request cancellation while an export is running.

The exporter binds to an immutable revision, renders the full sequence resolution with originals,
and writes a unique temporary sibling. Only a complete output is atomically committed. Failure or
cancellation removes the temporary file and leaves an existing destination unchanged.

The file contains video plus deterministic signed 16-bit little-endian PCM at 48 kHz stereo. Audio
is rendered from the same immutable revision in bounded 960-sample (20 ms) packets. The exact master
sample count is the ceiling of sequence duration multiplied by 48,000, and audio timestamps derive
from absolute sample positions. Original media—not proxy audio—is authoritative. The realtime
provider follows the same originals-only rule. Clip gain/pan,
fades, track mute/solo, gaps, overlaps, playback rate, and reverse mapping are applied; track-effect
DSP, normalization, and limiting are not.

Export captions separately from the Captions panel. Do not treat the current PCM reference master
as a creator-ready H.264/AAC delivery file.

## Keyboard shortcuts

Qt maps standard shortcuts to the platform convention; the table uses the Windows/Linux spelling.

| Action | Shortcut |
| --- | --- |
| New / Open / Save / Save As | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` |
| Import media | `Ctrl+I` |
| Open Deliver workspace | `Ctrl+E` |
| Undo / Redo | `Ctrl+Z` / platform redo shortcut |
| Split selected clip | `Ctrl+B` |
| Delete / Ripple delete | `Delete` / `Shift+Delete` |
| Reverse / Stop / Forward | `J` / `K` / `L` |
| Play or pause | `Space` |
| Previous / next frame | `,` / `.` |
| Move playhead one frame | `Left` / `Right` |
| Move playhead ten frames | `Shift+Left` / `Shift+Right` |
| Sequence start / end | `Home` / `End` |
| Nudge active clip | `Alt+Left` / `Alt+Right` |
| Nudge active clip ten frames | `Alt+Shift+Left` / `Alt+Shift+Right` |
| Timeline zoom in / out / fit | standard zoom shortcuts / `Shift+Z` |
| Source-monitor placeholder | `Shift+2` |
| Precision-trim placeholder | `T` |
| Import / Edit / Audio & Captions / Deliver | `Ctrl+1` / `Ctrl+2` / `Ctrl+3` / `Ctrl+4` |
| Command palette | `Ctrl+Shift+P` |

Shortcuts are not remappable in the current build despite remapping being a beta requirement.
