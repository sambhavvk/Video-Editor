<!-- SPDX-License-Identifier: MPL-2.0 -->

# ADR 0015: Clip-local effect curves and CPU reference effects

- **Status:** accepted
- **Date:** 2026-08-13
- **Owners:** Core/Media and Desktop/Product

## Context

Schema v2 already preserves typed, versioned clip effects and keyframes, but the desktop exposed no
complete parameter or curve workflow and the reference renderer skipped known effects. Allowing the
UI, persistence layer, and renderer to interpret keyframe time or interpolation independently would
make animations change when a clip moves or when preview and export use different paths.

## Decision

### Canonical curve contract

- Effect keyframe time is a clip-local rational offset in the half-open interval
  `[0, clip duration)`. Moving a clip therefore moves its animation. The controller adds or removes
  the clip start only at the playhead/presentation boundary.
- Parameter and keyframe values retain their declared `EffectValue` type. Keyframes are finite,
  type-compatible, uniquely identified, strictly ordered, and cannot share a time.
- The interpolation stored on the left keyframe controls the following segment. Hold keeps the left
  value; Linear interpolates directly. Bezier handles are finite normalized offsets from their
  owning keyframe: incoming X points left, outgoing X points right, and the resulting segment time
  curve must remain monotonic. This preserves the schema-v2 handle representation used by the
  desktop curve surface. Integer interpolation uses explicit ties-to-even rounding. Non-numeric
  values remain discrete.
- An invalid curve is rejected by the edit command before a revision is published. Project-codec
  validation remains a second defensive boundary, not the first place an editor-created curve can
  fail.

### Desktop authoring

- The Inspector shows essential effect values as exact numeric, Boolean, or text fields. Its
  expandable animation section provides parameter selection, keyframe list/navigation, exact time
  and value fields, Hold/Linear/Bezier selection, deletion, and an editable curve surface.
- A curve gesture is transient until release, then produces one revisioned
  `SetClipEffectParameterCommand`. Escape/cancellation leaves authoritative state unchanged. Screen
  readers receive named fields and the curve supports keyboard selection and adjustment.
- The Effects panel creates typed known-effect presets. Unknown future effects remain opaque and
  disabled while preserving their bytes and identifiers.

### Reference rendering and GPU fallback

- The dependency-free evaluator is shared by the CPU render graph. Known `video.color`,
  `video.crop`, and `video.gaussian_blur` parameters are evaluated at the active clip-local time.
  The CPU implementation establishes preview/export reference behavior.
- Expensive blur may be bypassed only when the preview profile explicitly permits it; full-quality
  export applies it. Unknown or disabled effects do not change the frame.
- An active effect remains a typed unsupported-timeline result for the current GPU compositor, so
  the desktop renders that frame through CPU without marking the GPU device failed.

## Consequences

Creators can author and persist parameter animation without a second beginner-only model. Moving a
clip preserves its animation geometry, save/reopen retains the curve, and CPU preview and export use
the same evaluation rules. Native GPU effect shaders and additional professional effects can be
added later against this reference contract.

## Required verification

- Hold, Linear, and Bezier boundaries; stable rational comparisons; integer ties-to-even; discrete
  values; malformed values, IDs, ordering, controls, and clip-duration bounds.
- Non-zero timeline-start controller tests proving local keyframe storage and absolute seeking.
- Inspector field, list, deletion, interpolation, curve mouse/keyboard, accessibility, and
  single-commit interaction tests.
- CPU golden frames before, between, and after keyframes; preview blur bypass; full-quality blur;
  unknown/disabled effect behavior; and typed GPU-to-CPU fallback.
