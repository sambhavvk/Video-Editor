# X02 — Trim-loop playback (scoped)

Scoped on branch `beta-1.0-fix` (phase 10 X02). Discovery only; no new transport implementation
claimed. Core In/Out loop already ships; this chunk scopes remaining UX, tests, and docs.

## Options considered

| Option | Existing evidence | Why not first |
| --- | --- | --- |
| Trim-loop playback | Program In/Out marks exist; **loop wrap is already wired** | **Selected remainder** — smallest gap is discoverability, not the wrap policy |
| Source patching / sync locks | Linked A/V move/trim implemented ([E03](e03-linked-av-sync.md)) | Large model + UI surface |
| Project-level track groups | [E04](e04-track-navigation.md) name-heuristic presets only | Needs schema + persistence |
| Overview / minimap strip | E04 residual: "No overview/minimap strip" | Layout + performance work |

## Current product (do not rebuild)

`toggleLoopPlayback` is a registered command (`Ctrl+Shift+L`) connected to
`EditorController::toggleLoopPlayback`. While enabled, forward transport wraps from program Out
(or sequence end) back to program In (or 0). The user guide already says **I**/**O** mark program
In/Out "for loop and export range".

What is **not** done:

- No Timeline/transport menu row, toolbar toggle, or checkable action state (command palette and
  shortcut only; toast "Loop playback enabled/disabled").
- No QSettings / `.veproj` persistence; `loop_playback_` is a controller flag that resets on process
  restart.
- No status-bar `Loop: In–Out` string and no extra ruler chrome beyond existing program-mark drawing.
- No automated tests (`EditorControllerTest` / `EditorWindowTest` have no loop cases).
- Shortcut table in `user-guide.md` omits the command.
- **Ctrl+L is already Linked Selection** (`toggleLinkedSelection`). Do not reuse it for loop.
- Reverse / shuttle loop is not implemented (`loop_playback_` applies only when `playback_rate_ > 0`).
- Starting play while the playhead is past Out immediately wraps to In on the next tick; it does
  not keep playing from the scrubbed position until Out.

## Remaining feature: loop UX on the existing wrap

Keep the current In/Out wrap. Close the discoverability and test gap on one reference edit.

### Reference edit (concrete behavior)

**Setup:** Sequence with one dialogue clip; set program **I** at 00:00:05:00 and **O** at
00:00:12:00 with the playhead at In.

| Step | Action | Expected result |
| --- | --- | --- |
| 1 | Toggle loop (Timeline menu, transport, command palette, or **Ctrl+Shift+L**) | Transport loops: play from In; on reaching Out, seek to In and continue |
| 2 | Scrub past Out while loop on, then play | Keep today's wrap-to-In on the next tick (document it); do not invent a second start policy in v1 |
| 3 | Trim clip tail inward so Out is now inside the clip | Loop boundary follows **marks**, not clip end |
| 4 | Clear Out mark (`Alt+O`) | Loop uses sequence end as Out |
| 5 | Toggle loop off mid-play | Play continues past former Out without snap-back |
| 6 | Ripple-trim while looping | Loop marks unchanged; preview obeys new timeline duration if Out was sequence end |

### Interaction with existing edits

- **Precision trims (E02):** Loop does not alter trim gestures; two-up compare unchanged. Committing
  an edit already stops program transport (`apply` / `applyBatch`).
- **Linked A/V:** Loop is sequence-time; no special casing.
- **Export range:** Deliver panel "use export range" remains independent; loop does not set export In/Out.
- **Play Around (`Shift+K`):** Existing preroll/postroll stop remains a separate one-shot; loop must
  not fight `play_around_end_` (today play-around is checked first and stops).
- **Multicam / nested:** Loop uses active sequence timeline time only.

### UI sketch notes

- Checkable toggle on the Timeline menu (and optional transport toolbar), icon: loop arrows.
- Keep **Ctrl+Shift+L**; never **Ctrl+L**.
- Status bar shows `Loop: In–Out` when enabled and both marks set.
- In/Out line on program ruler already exists; optional stronger highlight of the loop region is
  polish, not a new mark model.
- Persistence: QSettings `transport/loopEnabled` is acceptable for v1; `.veproj` persistence deferred.

### Independently verifiable command/UI steps

1. Desktop: checkable Timeline/transport toggle, keep existing shortcut, optional status text.
2. Test: `EditorControllerTest::loopsPlaybackWithinInOut`.
3. Test: `EditorWindowTest::loopTogglePersistsWhileMarksChange` (session QSettings only).
4. `user-guide.md` shortcut row for Toggle Loop Playback.

## Non-goals (leave for later)

- Reimplementing In/Out wrap or a `SetLoopEnabledCommand` in the edit model.
- Stealing **Ctrl+L** from Linked Selection.
- Source-monitor loop, beat loop, loop selection per clip, or reverse-direction loop.
- Track groups, overview strip, source patching, multicam angle loop.
- Bundling loop with slip/roll preview changes.
- Changing Play Around into a looping region.

## Follow-up chunks

| ID | Work |
| --- | --- |
| X02a | Checkable Timeline/transport toggle + status text on existing `loop_playback_` |
| X02b | Optional ruler emphasis; QSettings `transport/loopEnabled` |
| X02c | Tests + `user-guide.md` loop shortcut row |
