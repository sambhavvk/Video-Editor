// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <optional>
#include <string>

namespace video_editor::edit {

// Keyframe times are clip-local and use the half-open range [0, clip_duration).
// A keyframe at clip_duration is rejected: the last sample remains in force at
// the clip's right edge. Bezier handles are normalized segment coordinates:
// outgoing_control and incoming_control are (time fraction, value fraction),
// each in [0, 1]. The outgoing handle belongs to the left keyframe and the
// incoming handle to the right keyframe. The x handles must be ordered so the
// cubic time curve is monotonic.
[[nodiscard]] std::optional<std::string>
validateEffectParameter(const EffectParameter& parameter,
                        std::optional<Time> clip_duration = std::nullopt);

[[nodiscard]] std::optional<std::string>
validateEffect(const Effect& effect, std::optional<Time> clip_duration = std::nullopt);

// Evaluates one parameter at clip-local time. Values before the first
// keyframe use the parameter's base value; values after the last keyframe use
// the last keyframe. Numeric variants retain their stored type (integer
// interpolation uses deterministic ties-to-even rounding). Non-numeric
// variants use the left sample until the right keyframe is reached.
[[nodiscard]] std::optional<EffectValue> evaluateEffectParameter(const EffectParameter& parameter,
                                                                 Time clip_local_time);

} // namespace video_editor::edit
