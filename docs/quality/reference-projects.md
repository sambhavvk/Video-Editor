<!-- SPDX-License-Identifier: MPL-2.0 -->

# B03: Reference editing scenarios

Recorded 12 September 2026. Another tester should be able to repeat four editing
workflows using eligible FOSS or generated stand-in media, then compare timing,
audio, relinking, and export behavior against the catalog expectations. This
document records media properties and local measurements from one generator run;
it does **not** claim 60–120 minute endurance, physical-device xrun/drift, or
other untested performance gates.

Related evidence:

- [B01: Baseline workflow verification](baseline-workflow-verification.md) —
  import, trim, save/reopen, captions, and export matrix for the current slice.
- [B02: FOSS component register](foss-component-register.md) — eligible libraries,
  codecs, and blocked items for substitution checks.

## Generate stand-in media

Catalog source of truth: `tests/fixtures/reference/scenarios.json`.

```sh
python3 tools/quality/generate_reference_fixture.py --validate
python3 tools/quality/generate_reference_fixture.py
```

Requirements:

- `ffmpeg` and `ffprobe` on `PATH` (or `FFMPEG` / `FFPROBE`).
- Optional `video_editor_agent_host` for the pre-built `synthetic-edit.veproj`
  (`--agent-host`, `VIDEO_EDITOR_AGENT_HOST`, or
  `build/dev/src/agent_host/video_editor_agent_host`). Use `--media-only` to skip
  the project step.

Output (gitignored under `tests/fixtures/reference/generated/`):

- `<scenario-id>/` — lavfi `testsrc` + sine media (`mpeg4`/`pcm_s16le` MKV or
  `pcm_s16le` WAV) with distinct sine frequencies per file.
- `synthetic-edit/captions.srt` — two 3 s caption segments.
- `synthetic-edit/synthetic-edit.veproj` — when the agent host is available.
  Re-running overwrites existing files under `generated/` (stderr warning).
  The generator deletes leftover `{uuid}.agent.working.sqlite` after save;
  an agent-host reopen may recreate it (gitignored — do not commit).
- `run-log.json` — file sizes and ffprobe summaries from the same run.

CMake: `generate-reference-fixture` custom target and
`quality.validate_reference_scenarios` ctest.

## Substitute your own eligible footage

You may replace generated stand-ins with your own media **if** you match the
catalog properties for resolution, frame rate, and audio rate/channels and
keep redistribution rights. Catalog `durationSeconds` is the generated
stand-in length (for example 12 s A-roll), not a 10–20 minute file. Longer
eligible footage is allowed for youtube/interview/film; record the measured
duration in the log instead of failing the run for length. Never commit
copyrighted files or generated binaries to Git. Store personal substitutes
**outside** the repository (re-running the generator overwrites `generated/`).

After substitution, re-run your edit scenario and record differences in the
measurement log below. Relink checks should still pass when paths change but
content properties stay the same.

## Hardware and version template

Fill this block on your machine before comparing results.

| Item | Template | Observed (12 Sep 2026, generator host) |
| --- | --- | --- |
| Host | Linux x86_64 | Linux x86_64, kernel 7.2.3-1-cachyos |
| Compiler | GCC (pinned in dev docs) | GCC 16.2.1 |
| Qt | 6.11.2 | 6.11.2 |
| FFmpeg | n9.0.1 LGPL build | n9.0.1 (`ffmpeg 2:9.0.1-4.1`) |
| Agent host | optional `video_editor_agent_host` | present at `build/dev/.../video_editor_agent_host` |
| Editor build | `cmake --preset dev` | not exercised interactively in this B03 pass |

## Scenario 1 — `synthetic-edit`

**Purpose:** Repeatable import, insert, save, reopen, caption, and reference-export checks.

**Media (catalog / generated):**

| File | Properties | Sine (Hz) | Size (bytes) | ffprobe (observed) |
| --- | --- | --- | --- | --- |
| `talking-head.mkv` | 1280×720, 30 fps, 6 s, stereo 48 kHz | 220 | 1,976,582 | video `mpeg4` 1280×720 @ 30/1; audio `pcm_s16le` 48 kHz stereo; duration 6.000 s |
| `captions.srt` | two segments 0–3 s and 3–6 s | — | 125 | text sidecar |

**Expected edits:**

1. Import `talking-head.mkv`
2. Ripple-insert the full asset at 00:00:00:00
3. Add two SRT captions covering 0–3 s and 3–6 s
4. Save `synthetic-edit.veproj`

**Expected delivery:** `master.ffv1` reference plus optional YouTube 1080p VP9/Opus WebM.

**Relink check:** Move `talking-head.mkv`, reopen, Relink to the new path; asset count stays 1 and clip count stays 2 (linked video+audio from the muxed MKV).

**Agent-host baseline (measurements, not performance):** `synthetic-edit.veproj` 16,384 bytes;
1 asset, 2 clips (linked A/V from the MKV), revision 2 after insert + save.
`open_project` + `read_session` still shows 1 asset and 2 clips; the in-memory
editor revision is 0 after hydrate from the checkpoint (that is not a failed save).

## Scenario 2 — `youtube-episode`

**Purpose:** Short generated stand-in (12 s A-roll / 6 s B-roll) for a 10–20 minute creator workflow: A-roll, B-roll, music, captions, 16:9 delivery. Do not treat the 12 s file as a failed 10–20 minute run.

**Media (catalog / generated):**

| File | Properties | Sine (Hz) | Size (bytes) | ffprobe (observed) |
| --- | --- | --- | --- | --- |
| `a-roll.mkv` | 1920×1080, 30 fps, 12 s, stereo 48 kHz | 330 | 5,175,144 | video `mpeg4` 1920×1080 @ 30/1; audio `pcm_s16le`; duration 12.000 s |
| `b-roll.mkv` | 1920×1080, 30 fps, 6 s, stereo 48 kHz | 440 | 2,586,587 | video `mpeg4` 1920×1080 @ 30/1; audio `pcm_s16le`; duration 6.000 s |
| `music.wav` | 12 s, stereo 48 kHz | 550 | 2,304,078 | audio `pcm_s16le`; duration 12.000 s |

**Expected edits:**

1. Mark source In/Out on A-roll (2 s–10 s) and ripple-insert
2. Overlay B-roll on a second video track for 3 s at 4 s
3. Place music under the sequence; lower clip gain so dialogue stays readable
4. Add captions for the A-roll sentences
5. Export YouTube 1080p VP9/Opus

**Expected delivery:** YouTube 1080p VP9/Opus WebM; sidecar WebVTT optional.

**Relink check:** Relink A-roll after moving the file; sequence duration and caption times must not change.

## Scenario 3 — `interview`

**Purpose:** Two picture angles plus one master audio recording before multicam exists.

**Media (catalog / generated):**

| File | Properties | Sine (Hz) | Size (bytes) | ffprobe (observed) |
| --- | --- | --- | --- | --- |
| `camera-a.mkv` | 1920×1080, 30 fps, 10 s, stereo 48 kHz | 660 | 4,303,153 | video `mpeg4` 1920×1080 @ 30/1; audio `pcm_s16le`; duration 10.000 s |
| `camera-b.mkv` | 1920×1080, 30 fps, 10 s, stereo 48 kHz | 770 | 4,303,153 | video `mpeg4` 1920×1080 @ 30/1; audio `pcm_s16le`; duration 10.000 s |
| `lav-master.wav` | 10 s, stereo 48 kHz | 880 | 1,920,078 | audio `pcm_s16le`; duration 10.000 s |

**Expected edits:**

1. Align both cameras and lav-master at 00:00:00:00 (manual; no multicam group yet)
2. Keep lav-master as the audible track; mute camera audio
3. Cut picture between camera A and B by trimming/splitting at 4 s and 7 s
4. Export podcast Opus-only plus a VP9 picture reference

**Expected delivery:** Podcast Opus WebM plus optional VP9 picture.

**Relink check:** Offline `camera-b`, relink; A/V offset of the remaining clips must stay zero.

## Scenario 4 — `short-film`

**Purpose:** 24 fps picture, production sound, titles, and a reference master.

**Media (catalog / generated):**

| File | Properties | Sine (Hz) | Size (bytes) | ffprobe (observed) |
| --- | --- | --- | --- | --- |
| `scene-wide.mkv` | 1920×1080, 24 fps, 8 s, stereo 48 kHz | 990 | 3,054,902 | video `mpeg4` 1920×1080 @ 24/1; audio `pcm_s16le`; duration 8.000 s |
| `production-sound.wav` | 8 s, stereo 48 kHz | 1046 | 1,536,078 | audio `pcm_s16le`; duration 8.000 s |

**Expected edits:**

1. Set sequence to 1920×1080 24 fps if the first clip does not
2. Insert picture and production sound, linked or stacked
3. Add a title at 0–3 s
4. Export FFV1/MKV reference master

**Expected delivery:** `master.ffv1` at 24 fps, Rec.709, originals-only.

**Relink check:** Copy the project folder to another path and reopen; media comes back Online or Relink reports the missing URI.

## Measurement log template

Copy this table per scenario run. Record **observations only** — do not infer
performance from a single short clip.

| Field | Your run |
| --- | --- |
| Date / commit | |
| Host / OS / kernel | |
| Compiler / Qt / FFmpeg | |
| Scenario id | |
| Media source | generated / substituted (describe license) |
| Clip count after edit | |
| Asset count | |
| Sequence duration (rational or timecode) | |
| Export preset | |
| Output file + size | |
| ffprobe summary (video/audio) | |
| Relink result | pass / fail + notes |
| Timing or A/V delta notes | |
| Unexpected behavior | |

Store completed logs outside Git or in a private notes file. The generator’s
`run-log.json` is a machine-readable baseline for media sizes and ffprobe only.

## Observed baseline on this machine (12 Sep 2026)

Generator command: `python3 tools/quality/generate_reference_fixture.py` after
`--validate`. Tools: FFmpeg n9.0.1, ffprobe n9.0.1. Host: Linux x86_64
CachyOS, GCC 16.2.1, Qt 6.11.2.

These numbers are **file-size and ffprobe measurements** from one local run.
They are not playback, export, or endurance benchmarks.

| Scenario | Generated files | Total media bytes | Project / sidecar |
| --- | --- | --- | --- |
| `synthetic-edit` | `talking-head.mkv`, `captions.srt` | 1,976,707 | `synthetic-edit.veproj` 16,384 B; 1 asset, 2 clips |
| `youtube-episode` | `a-roll.mkv`, `b-roll.mkv`, `music.wav` | 10,065,809 | (manual edit) |
| `interview` | `camera-a.mkv`, `camera-b.mkv`, `lav-master.wav` | 10,526,384 | (manual edit) |
| `short-film` | `scene-wide.mkv`, `production-sound.wav` | 4,590,980 | (manual edit) |

Full probe payloads: `tests/fixtures/reference/generated/run-log.json` (gitignored).

**Not measured in this pass:** interactive desktop timing, creator-delivery queue
encode, 60–120 minute sessions, physical audio xrun/drift, or GPU presentation.
