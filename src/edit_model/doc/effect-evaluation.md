<!-- SPDX-License-Identifier: MPL-2.0 -->

# Effect evaluation API reference

Header: `video_editor/edit_model/effect_evaluator.h`

Namespace: `video_editor::edit`

## Contract

`validateEffectParameter` verifies a nonempty ID, finite typed values, strictly increasing unique
clip-local keyframe times, matching value types, finite normalized handle offsets, and optional
half-open clip-duration bounds. `validateEffect` additionally checks parameter-map identity and the
canonical ranges of known video and audio effect types.

`evaluateEffectParameter` returns the base value before the first keyframe, the last value after the
last keyframe, and otherwise applies interpolation stored on the left keyframe. Hold is discrete,
Linear interpolates numeric/vector/color values, and Bezier solves a monotonic time curve from the
left outgoing and right incoming handle offsets. Integer interpolation uses ties-to-even rounding;
non-numeric values remain discrete.

## Ownership and errors

Validation returns an owned explanatory string or no value. Evaluation returns no value for an
invalid parameter and otherwise returns an owned `EffectValue`. Functions retain no references,
perform no I/O, and are independent of Qt, FFmpeg, persistence, and rendering.

AI assistance has been used to create this output.
