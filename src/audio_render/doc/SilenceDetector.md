<!-- SPDX-License-Identifier: MPL-2.0 -->

# SilenceDetector API reference

Header: `video_editor/audio_render/silence_detector.h`

Namespace: `video_editor::audio_render`

`detectSilence(block, options)` analyzes caller-owned planar float audio in the canonical 48 kHz
mono/stereo format. Each aligned analysis window is silent only when both its RMS and absolute peak
are within the explicit thresholds. The result owns sorted absolute half-open `SilenceRange` sample
intervals, filters intervals shorter than `minimum_silence_samples`, and joins intervals separated
by no more than `merge_gap_samples`.

The detector rejects unsupported formats, zero windows/minimums, negative or non-finite thresholds,
non-finite samples, channel-size mismatches, and overflowing absolute sample ranges. It performs no
media I/O and does not read or mutate an edit snapshot. Callers analyzing long timelines must render
bounded aligned blocks, merge boundary-spanning candidates, and apply the final global minimum only
after merging.

Input spans are borrowed for the synchronous call. Returned ranges and errors own their data.
Independent calls share no state and may run concurrently.

AI assistance has been used to create this output.
