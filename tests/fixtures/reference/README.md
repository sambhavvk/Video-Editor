<!-- SPDX-License-Identifier: MPL-2.0 -->

# Reference editing scenarios

This directory holds the checked-in catalog for four representative editing
scenarios (`scenarios.json`). Synthetic stand-in media is generated locally or
in CI under `generated/` (gitignored) by deterministic ffmpeg lavfi/sine
recipes. Those fixtures are MPL-2.0 generated media, not copyrighted footage.

Generate and inspect:

```sh
python3 tools/quality/generate_reference_fixture.py --validate
python3 tools/quality/generate_reference_fixture.py
```

`generate_reference_fixture.py` requires `ffmpeg` and `ffprobe` on `PATH` (or
`FFMPEG` / `FFPROBE`). It writes `tests/fixtures/reference/generated/<scenario-id>/`
media, a `captions.srt` for `synthetic-edit`, optional
`synthetic-edit/synthetic-edit.veproj` when `video_editor_agent_host` is
available, and `generated/run-log.json` with file sizes and ffprobe properties
from the same run. Re-running overwrites existing files under `generated/`
(stderr warning). Muxed talking-head A/V is 1 asset / 2 linked clips. The
generator deletes leftover `{uuid}.agent.working.sqlite` after save; do not
commit that sidecar. Encoders are not always bit-exact across ffmpeg builds;
treat the run log as local measurements, not performance claims.

See [Reference projects](../../docs/quality/reference-projects.md) for tester
workflows, substitution rules, expected edits, and the measurement log template.

CMake exposes `generate-reference-fixture` and a cheap
`quality.validate_reference_scenarios` ctest. Do not commit generated binaries,
`.veproj` checkpoints, or `{uuid}.agent.working.sqlite` sidecars.

Do not add copyrighted sample footage without explicit redistribution rights.
