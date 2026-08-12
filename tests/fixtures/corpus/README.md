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

Run the integrity check with:

```sh
python3 tools/quality/verify_corpus.py
```

Use `--release` in a release gate. That mode requires `status: complete`, at
least 200 assets, all required categories, and no missing or mismatched files.
The default scaffold mode still validates every entry that is present and
prints the outstanding coverage.

Do not add copyrighted sample footage without explicit redistribution rights.
Never silently regenerate an existing fixture: a byte change requires a new
checksum and review of all affected goldens.

