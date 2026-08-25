<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0007: Versioned proxy PTS map and original authority

- **Status:** accepted
- **Date:** 2026-08-08
- **Owners:** Core/Media and Quality/Platform

## Context

Variable-frame-rate media, B-frames, unusual timestamp origins, and encoder time-base choices make
nominal frame-rate mapping insufficient. A proxy must improve preview performance without becoming
editorial truth or changing which original frame is used for final export.

## Decision

- Every generated proxy is accompanied by a versioned little-endian `.vepts` sidecar. Version 1
  begins with `VEPTSMAP` and stores the proxy profile, source fingerprint, exact stream time bases,
  source origin PTS, and source/proxy PTS and duration for every video frame.
- Readers fail closed on unknown versions, invalid time bases, non-monotonic records, trailing data,
  or a mismatched source fingerprint. Forward compatibility requires an explicit reader change.
- The PTS map, not the proxy container's nominal rate, is the canonical mapping back to original
  presentation timestamps.
- Original media is always authoritative for export. The playback registry may select a complete,
  present proxy when a preview profile permits it and otherwise falls back to the original.
- The default profile is half-resolution ProRes Proxy/MOV with 48 kHz PCM audio. FFV1/Matroska is
  the predetermined patent-neutral fallback when profile resolution permits it and encoder
  availability requires it.
- Proxy and sidecar outputs are rebuildable cache artifacts outside `.veproj`. Generation and
  cancellation use temporary files; only a complete pair is committed.

## Consequences

- Proxy generation can preserve an auditable exact mapping for VFR and nonzero-origin sources.
- Project checkpoints remain small and portable, and deleting a proxy cannot delete an edit.
- Alternate media streams, subtitles, alpha, and interlaced field cadence are not represented by
  the current proxy profile and remain available only from originals.
- Playback still needs full `.vepts`-driven seek integration and corpus proof before VFR proxy
  correctness can be considered beta-complete.
+ Playback consumes registered `.vepts` maps for proxy seek and rebuilds decoded presentation from
  source PTS/duration. A missing, corrupt, or unregistered map fails closed to the original. VFR
  corpus proof remains a release gate before proxy correctness is considered beta-complete.

## Verification

Tests cover profile resolution/fallback, sidecar round-trip and validation, transcode timing,
cancellation, destination safety, and fail-closed worker preset/event behavior. The release gate adds
random proxy/original comparison over the completed VFR/B-frame/unusual-PTS corpus plus supervised
worker-death and cancellation tests.
