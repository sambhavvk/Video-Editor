# X02 — Trim-loop playback (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X02). Discovery only; no implementation claimed.

## Options considered

| Option | Existing evidence | Why not first |
| --- | --- | --- |
| Trim-loop playback | Program In/Out marks exist (`user-guide.md`); no loop toggle wired to transport | **Selected** — smallest gap on a reference edit |
| Source patching / sync locks | Linked A/V move/trim implemented ([E03](e03-linked-av-sync.md)) | Large model + UI surface |
| Project-level track groups | [E04](e04-track-navigation.md) name-heuristic presets only | Needs schema + persistence |
| Overview / minimap strip | E04 residual: "No overview/minimap strip" | Layout + performance work |

## Chosen feature: trim-loop playback

Loop playback confined to the **program monitor In/Out range** (or full sequence when In/Out unset),
for reviewing a trimmed section without exporting.

### Reference edit (concrete behavior)

**Setup:** Sequence with one dialogue clip; set program **I** at 00:00:05:00 and **O** at
00:00:12:00 with the playhead at In.

| Step | Action | Expected result |
| --- | --- | --- |
| 1 | Press loop toggle (toolbar or **Ctrl+L** proposal) | Transport loops: play from In; on reaching Out, seek to In and continue |
| 2 | Scrub past Out while loop on | Playhead moves freely; on **play**, clamp start to max(playhead, In) and still loop at Out |
| 3 | Trim clip tail inward so Out is now inside the clip | Loop boundary follows **marks**, not clip end |
| 4 | Clear Out mark | Loop uses sequence end as Out |
| 5 | Toggle loop off mid-play | Play continues past former Out without snap-back |
| 6 | Ripple-trim while looping | Loop marks unchanged; preview obeys new timeline duration if Out was sequence end |

### Interaction with existing edits

- **Precision trims (E02):** Loop does not alter trim gestures; two-up compare unchanged. Loop pauses
  during active trim drag (same gate as background job admission during scrub).
- **Linked A/V:** Loop is sequence-time; no special casing.
- **Export range:** Deliver panel "use export range" remains independent; loop does not set export In/Out.
- **Multicam / nested:** Loop uses active sequence timeline time only.

### UI sketch notes

- Toggle adjacent to transport (icon: loop arrows); checked state persisted per sequence in session UI
  (QSettings `transport/loopEnabled` acceptable for v1; `.veproj` persistence deferred).
- Status bar shows `Loop: In–Out` when enabled and both marks set.
- In/Out line on program ruler highlights loop region (reuse marker overlay styling).

### Independently verifiable command/UI steps

1. Model: `TransportLoopMode` + `SetLoopEnabledCommand` (or controller flag if non-persistent v1).
2. Desktop: loop toggle, keyboard shortcut, playhead wrap at Out.
3. Test: `EditorControllerTest::loopsPlaybackWithinInOut`.
4. Test: `EditorWindowTest::loopTogglePersistsWhileMarksChange`.

## Non-goals (leave for later)

- Source-monitor loop, beat loop, or loop selection per clip.
- Track groups, overview strip, source patching, multicam angle loop.
- Bundling loop with slip/roll preview changes.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X02a | Transport loop policy + wrap on `advancePlayhead` |
| X02b | Desktop toggle, shortcut, ruler highlight |
| X02c | Tests + `user-guide.md` loop row |
