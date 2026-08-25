<!-- SPDX-License-Identifier: MPL-2.0 -->

# User guide for the current desktop build

This guide describes behavior implemented today. The **first public beta is Linux exclusive**;
Windows is an engineering preview and is not a first-beta distribution target. Save important work
often and keep the original media files available.

## Important limitations

- The desktop first requests an engine GPU path for active video clips through libplacebo, applying
  crop, position, scale, a custom anchor or centered-pivot rotation, opacity, and Normal composition,
  then downloads the offscreen result for the viewer. Rotation around a moved pivot, effects, title
  clips, active transitions, and non-Normal blends use CPU for that frame without disabling future
  GPU attempts.
  Backend/device failures preserve the CPU frame and latch CPU preview for the session. This is not
  native swapchain presentation, zero-copy decode, or a full effects/color graph.
- Forward 1× transport uses optional miniaudio 48 kHz stereo playback when the selected output opens.
  Its playhead uses a latency-compensated audio-master position, not the end of the submitted device
  buffer; the remaining latency is reported as uncertainty. Otherwise it clearly falls back to
  silent timer-driven video. Reverse and shuttle rates other than 1× are silent. Device choice is
  persisted; one-second background polling pauses safely on loss and reopens a returned selected or
  system-default endpoint while playback is still intended.
- Creator presets export FOSS VP9/Opus WebM, not H.264/AAC. On Windows the exporter can select
  VP9 QSV and on Linux VP9 VAAPI after export-worker device validation. A hardware setup, upload,
  device, or encode failure restarts the complete atomic export with `libvpx-vp9`.
- Master peak/RMS and integrated `LUFS-I` are live. LUFS is calculated by libebur128 on a dedicated
  worker and explicitly shows analyzing/stale state. Normalization independently analyzes one
  immutable revision against an editable −24 through −9 LUFS target.
- The Inspector and Effects panel author the supported title, transition, speed, effect, and
  keyframe controls described below. Source-monitor insert/overwrite editing remains incomplete.
- Local transcription requires an explicitly downloaded, checksummed multilingual base model and a
  build with the pinned optional `whisper.cpp` backend. Builds without that backend keep manual
  captions available and report transcription as unavailable.
- Relinking, cache management, thumbnails, and waveforms are connected in the current desktop slice.
  Unplugged-media and disk-full fault matrices remain Linux-first release work. Signed Windows MSI
  and the Windows GPU matrix are deferred until after the Linux beta.
- Project and internal schema compatibility are pre-beta and may change through migrations.

See the [full status matrix](beta-feature-status.md) before relying on a capability.
Linux testers: [beta tester guide](../BETA-TEST-LINUX.md) covers the local session logger and how
to build from source.

## Workspaces

The same project and timeline are shown through four layouts; switching workspaces does not create
a different editing mode.

- **Import** emphasizes the media bin and program viewer.
- **Edit** emphasizes the timeline, inspector, and effects browser.
- **Audio & Captions** shows the mixer and caption/transcript panel.
- **Deliver** shows reference-master and FOSS creator-delivery controls.

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

The media bin shows a thumbnail, name, duration, detected format, and status (Original, Offline,
Changed, Proxy recommended, Creating proxy…, or Proxy ready). Search filters by name or format.
The Inspector Asset group edits a cached display title, tags, notes, and rating; an empty title
falls back to the file name. Double-click an item to insert it at the playhead:

- Video goes to the first unlocked targeted video track.
- Audio goes to the first unlocked targeted audio track.
- An asset containing both creates linked video and audio clips as one atomic undoable revision.
- The insertion is rejected when it would overlap an existing clip; it does not silently replace
  existing work.
- The first inserted video clip derives sequence width, height, and nominal frame rate from the
  asset when those values are available. The sequence audio sample rate remains 48 kHz.

The application references the original path. Missing files show **Offline** on the media bin and
timeline; a fingerprint mismatch shows **Changed**. Right-click **Relink media…** to pick a
replacement. Matching fingerprints relink immediately; changed content asks for confirmation. On
open, matching files with the same name are recovered from the project folder, the last successful
relink directory, or the original parent folder. Relinked URIs are saved with the project.

**File > Manage Media Cache…** shows used space against a 10–200 GB budget (default 100 GB), lists
thumbnails, waveforms, metadata, proxies, and PTS maps, and can remove entries or clear the cache.
Clearing cache never deletes the `.veproj` or original media.

Audio clips draw a waveform on the timeline when the cache has generated one.

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
Click those controls to toggle them. Right-click a track header for Add Video/Audio Track, Rename,
Move Up/Down, Lock, Visible, Target, and Remove. **Insert** adds a video track and **Shift+Insert**
adds an audio track. **Ctrl+Up/Down**
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
values must leave a positive image area. Negative scale values flip the clip horizontally or
vertically; the renderer uses the magnitude for sizing.

Selecting an audio clip reveals Gain (-96 to +24 dB), Pan, Fade In, and Fade Out. Those edits are
validated, undoable, persistent, and used by realtime forward playback and reference export.

Use **Add title** to create a title at the playhead. A selected title exposes text, font family,
size, horizontal alignment, bold, and italic controls. A selected media clip exposes constant speed
from 0.01× through 100× and Reverse. On the timeline, transition regions expose duration handles;
their menu changes between Cross Dissolve and Dip to Black or removes the transition. These edits
are ordinary undoable schema-v2 state and use the CPU reference renderer when the GPU path reports
that frame unsupported.

Adjacent Inspector changes to the same property and clip use one coalescing key so continuous
spin-box adjustment collapses in undo history. The Effects panel adds supported color, crop, and
blur nodes. Expand **Effects & animation** to edit typed values, add/remove a keyframe at the
playhead, select a keyframe, enter exact clip-local time/value, choose Hold/Linear/Bezier, delete it,
or edit its curve. A curve drag commits once on release; Escape restores its pre-drag value. Moving
a clip preserves its animation because keyframe times are local to that clip.

## Mix and normalize audio

The Audio Mixer's track strips provide Gain (−96 to +24 dB), Pan, **M**ute, and **S**olo. Each audio
track can add parametric EQ, compressor, dialogue noise reduction, and limiter stages and edit their
current parameter values. The state is revisioned, persistent, undoable, and rendered in the fixed
order EQ → compressor → noise reduction → limiter for preview and export.

Each track strip shows live post-DSP peak/RMS for the block at the audio-master position—not a
future decode-ahead block. The master strip shows peak/RMS and authoritative integrated `LUFS-I`,
with analyzing or stale state when a valid result is unavailable. Choose a named output device or
**System default**. Background polling detects loss within one second, pauses audio safely, and
reopens a returned endpoint only while the previous playback intent is still active.

**Analyze loudness** renders the immutable current revision on a worker through libebur128. The
result previews the measured loudness and proposed gain for the selected −24 through −9 LUFS target.
**Apply** is enabled only while that revision is current and every contributing track can accept the
exact proposed gain without exceeding its canonical range. Changing the target invalidates an
in-flight or completed review. Applying publishes one atomic undo step.

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
from an immutable timeline snapshot on a worker, prefills a bounded ring, and starts the selected
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
does not make the present build a calibrated A/V-sync reference. Accelerated one-hour zero-xrun and
two-hour less-than-one-frame drift simulations pass, but the stricter 10 ms requirement,
physical-device latency calibration, and the supported hardware/OS matrix still gate beta.

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
- search caption or timed-word text and double-click a result to seek to its exact timeline time;
- edit alignment, vertical position, safe margin, text/background colors, emphasis, and outline;
- download the optional model, transcribe a selected media clip locally, cancel it, and review the
  proposed timed captions;
- review measured-silence cuts and conservative filler-word cuts before applying them;
- export the sequence captions as SRT or WebVTT.

Import validates timing, order, overlap, cue text, and UTF-8. All cues from one import form a single
undoable edit batch. Export writes atomically and rounds timing to the nearest millisecond because
SRT and WebVTT timestamps are millisecond-based. The Deliver panel can burn the canonical caption
style into video or create an SRT/WebVTT sidecar. The bitmap reference renderer is deterministic but
does not perform production font-family shaping, and embedded subtitle streams are not implemented.

Model download is always explicit. The editor rejects a declared size mismatch and stops an
unknown-length stream before it can exceed the pin. Bytes are staged, checked against the pinned
byte length and upstream digest away from the UI thread, and atomically installed outside the
project. Cancellation also covers verification and removes partial staging data. Media, transcripts,
and project data are not uploaded. Transcription launches one worker process for the selected clip,
seeks and decodes only its conservatively bounded original-media source range to mono 16 kHz audio,
and maps returned
source-absolute word timing through the clip's exact trim, speed, and reverse mapping. Cancel
terminates that job process; a worker/backend failure leaves the project unchanged.

The review list is bound to the immutable project revision used for analysis. `Measured silence`
items come from bounded, incremental analysis of exact 48 kHz timeline audio, use a conservative
5 ms inset, and are selected by default. Conservative standalone
`um`, `uh`, and `erm` transcript fillers are labelled separately and start unselected. Toggle any
item, then apply or discard the set. Applying accepted captions and cuts creates one atomic revision
and one undo step; edits made after analysis make the review stale and require regeneration.

## Create an editing proxy

4K long-GOP video is marked **Proxy recommended** and queued automatically after import or reopen
when the original is online. Right-click any other item and choose **Create editing proxy**. The
same menu cancels an active generation by stopping the worker process. Jobs run one at a time in a
restartable worker host: a half-resolution ProRes
Proxy/MOV with 48 kHz PCM audio when available, or the predetermined FFV1/Matroska fallback.
Completion updates playback to permit the proxy; export still uses the original.

The generated proxy and `.vepts` timestamp map live in the media cache, not the project. Reopening
the project rediscovers a matching proxy by asset id and source fingerprint. Cancellation and
failures do not commit a partial destination. See [Media, proxies, and cache](reference/media-proxies-and-cache.md).

## Export a master or creator file

1. Add at least one clip to the sequence.
2. Open **Deliver**. Choose FFV1/Matroska or ProRes/MOV for a reference master, or a YouTube,
   vertical, or podcast preset for FOSS VP9/Opus WebM delivery.
3. Optionally expand the controls and select resolution, frame rate, video bitrate or VP9 quality,
   audio bitrate, and caption burn-in/sidecar behavior.
4. Choose **Browse…** to select a destination, then **Export master**. Use the export button again
   to request cancellation while the job is running.

The exporter binds to an immutable revision, renders the full sequence resolution with originals,
and writes a unique temporary sibling. Only a complete output is atomically committed. Failure or
cancellation removes the temporary file and leaves an existing destination unchanged.

Reference masters contain video plus deterministic signed 16-bit little-endian PCM at 48 kHz
stereo. Creator video presets contain VP9 plus optional `libopus`; podcast output contains one Opus
stream and no video. Software delivery uses `libvpx-vp9`. When **Use hardware VP9 when available**
is enabled and a hardware encoder is registered, the export worker tries `vp9_qsv` on Windows and
`vp9_vaapi` on Linux. It validates the actual device without blocking panel startup. Any hardware
setup, frame-upload, device, or encode failure discards
that temporary attempt, reports the restart, and encodes from the beginning with libvpx. Audio is
rendered from the same immutable revision in bounded
960-sample (20 ms) packets. The exact master
sample count is the ceiling of sequence duration multiplied by 48,000, and audio timestamps derive
from absolute sample positions. Original media—not proxy audio—is authoritative. The realtime
provider follows the same originals-only rule. Clip gain/pan,
fades, track gain/pan/mute/solo, the ordered track DSP chain, gaps, overlaps, playback rate, and
reverse mapping are applied. Creator scaling preserves aspect ratio and letterboxes unused space;
frame-rate conversion samples exact rational output times.

This path intentionally does not use H.264/AAC. Missing `libvpx-vp9`/`libopus` capabilities disable
incompatible presets with a local explanation; hardware VP9 is optional and never removes the
software fallback.

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
