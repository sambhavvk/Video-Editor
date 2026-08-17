<!-- SPDX-License-Identifier: MPL-2.0 -->

# Representative media corpus

This directory is the checked-in index for the 200+ file codec and timeline
corpus. Large media should normally live in the controlled CI fixture store,
not in Git; a test runner materializes it at the paths recorded in
`manifest.json` and verifies every byte before using it.

The manifest starts in `scaffold` status with one small text fixture that tests
the verifier itself. `requiredCategories` records the minimum coverage expected
for beta, including VFR, B-frames, unusual starting PTS, rotation, interlacing,
8/10-bit color, HDR input, alpha, image sequences, corrupt inputs, unusual audio
layouts, SAR/field-order cases, and damaged timestamps. Coverage labels can be
combined on one asset, but the completed corpus must contain at least 200 unique
files and every required category.

Each asset entry must include:

- A stable ID and repository-relative `files/` path with no traversal.
- Exact byte length and lowercase SHA-256.
- One or more behavioral categories.
- Provenance and a redistribution license. Generated fixtures should also
  include a deterministic generation recipe.
- Optional media expectations that remain descriptive; decode golden data
  belongs beside the owning test rather than in this index.

The committed tree stays a scaffold (`manifest.json` plus
`files/manifest-smoke.txt`). The 200+ synthetic media files are generated
locally or in CI under `generated/` (gitignored) by deterministic ffmpeg
recipes. Those fixtures are MPL-2.0 generated media, not copyrighted footage.

Generate and verify:

```sh
python3 tools/quality/generate_corpus.py
python3 tools/quality/verify_corpus.py --generated
python3 tools/quality/verify_corpus.py --release
```

`generate_corpus.py` requires `ffmpeg` and `ffprobe` on `PATH` (or `FFMPEG` /
`FFPROBE`). It writes `tests/fixtures/corpus/generated/files/` plus a
`generated/manifest.json` that records exact byte lengths, SHA-256 digests, and
the generation recipe used for each asset. Encoders are not always bit-exact
across ffmpeg builds, so hashes are taken after generation in the same run.

The default verifier still checks only the committed scaffold:

```sh
python3 tools/quality/verify_corpus.py
```

`--generated` validates `tests/fixtures/corpus/generated/manifest.json`.
`--release` uses that generated manifest and requires `status: complete`, at
least 200 unique files, and every `requiredCategories` entry. If the generated
tree is missing, the command prints an instruction to run `generate_corpus.py`
instead of failing opaquely.

CMake exposes `verify-corpus` and `generate-corpus` custom targets, plus a cheap
`quality.verify_corpus_scaffold` ctest. Do not add hundreds of decode tests
against this corpus.

Do not add copyrighted sample footage without explicit redistribution rights.
Never silently regenerate an existing fixture: a byte change requires a new
checksum and review of all affected goldens.

