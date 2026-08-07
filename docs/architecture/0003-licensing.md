# ADR 0003: MPL application with dynamically linked media dependencies

- **Status:** accepted
- **Date:** 2026-08-06

## Decision

License beta desktop source under MPL-2.0. Dynamically link only approved LGPL Qt, FFmpeg, and
libplacebo configurations in official builds. Maintain dependency manifests, notices, source offers,
and an SPDX software bill of materials.

GPL/nonfree FFmpeg options and external encoders are disabled in the default build. H.264 encoder
distribution and codec patents are a public-beta legal release gate, independent of source-code
license compatibility.

