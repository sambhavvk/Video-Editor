# F06 — Long-form film project validation

## Scope

Exercises F01–F05 on a representative 60–120 minute project: navigation, relink, reopen, audio stem export, and OTIO handoff between workstations.

## Recorded baseline (manual)

| Check | Expected | Status |
|-------|----------|--------|
| Project reopen after overnight save | Production metadata, subclips, channel maps intact | Pass (schema v7 round-trip) |
| Media bin scene/take search at 90+ min | Responsive filter (<200 ms perceived) | Pass on dev fixture |
| Relink moved originals | Fingerprint + production fields unchanged | Pass |
| Audio stem export with handles | WAV sample counts match declared range | Pass (unit path) |
| OTIO handoff on second machine | `timeline.otio` opens; report lists omissions | Pass (OpenTimelineIO subset) |

## Blocking defects

None recorded for beta-1.0-fix; long-form stress remains manual on user hardware.

## Follow-ups

- Automated 90-minute lavfi fixture generation (not yet in CI).
- GPU preview performance on 4K multicam selects sequences.
