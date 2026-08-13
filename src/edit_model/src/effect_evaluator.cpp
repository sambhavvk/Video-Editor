// SPDX-License-Identifier: MPL-2.0
#include "video_editor/edit_model/effect_evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace video_editor::edit {
namespace {

[[nodiscard]] bool finite(const EffectValue& value) noexcept {
  return std::visit(
      [](const auto& item) noexcept {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, double>) {
          return std::isfinite(item);
        } else if constexpr (std::is_same_v<T, Vec2>) {
          return std::isfinite(item.x) && std::isfinite(item.y);
        } else if constexpr (std::is_same_v<T, ColorRgba>) {
          return std::isfinite(item.red) && std::isfinite(item.green) && std::isfinite(item.blue) &&
                 std::isfinite(item.alpha);
        } else {
          return true;
        }
      },
      value);
}

[[nodiscard]] bool finite(const Vec2 point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] double cubic(const double p0, const double p1, const double p2, const double p3,
                           const double t) noexcept {
  const double one_minus_t = 1.0 - t;
  return (one_minus_t * one_minus_t * one_minus_t * p0) +
         (3.0 * one_minus_t * one_minus_t * t * p1) + (3.0 * one_minus_t * t * t * p2) +
         (t * t * t * p3);
}

[[nodiscard]] double bezierProgress(const Vec2 outgoing, const Vec2 incoming,
                                    const double progress) noexcept {
  // The validation contract makes x(t) monotonic. Fixed-iteration bisection
  // avoids a platform-dependent root solver and gives deterministic output.
  double low = 0.0;
  double high = 1.0;
  for (int iteration = 0; iteration < 48; ++iteration) {
    const double middle = (low + high) * 0.5;
    if (cubic(0.0, outgoing.x, 1.0 + incoming.x, 1.0, middle) < progress) {
      low = middle;
    } else {
      high = middle;
    }
  }
  const double parameter = (low + high) * 0.5;
  // Handles are stored as offsets from their owning keyframe. The outgoing
  // handle is relative to the left sample and the incoming handle is relative
  // to the right sample. Convert the latter into segment-local coordinates.
  return std::clamp(cubic(0.0, outgoing.y, 1.0 + incoming.y, 1.0, parameter), 0.0, 1.0);
}

[[nodiscard]] std::int64_t roundTiesEven(const long double value) noexcept {
  if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  const long double floor_value = std::floor(value);
  const long double fraction = value - floor_value;
  if (fraction < 0.5L) {
    return static_cast<std::int64_t>(floor_value);
  }
  if (fraction > 0.5L) {
    return static_cast<std::int64_t>(floor_value + 1.0L);
  }
  const auto lower = static_cast<std::int64_t>(floor_value);
  return (lower % 2 == 0) ? lower : lower + 1;
}

[[nodiscard]] double timeRatio(const Time numerator, const Time denominator) noexcept {
  if (denominator.isZero()) {
    return 0.0;
  }
  const long double left =
      static_cast<long double>(numerator.value()) / static_cast<long double>(numerator.timescale());
  const long double right = static_cast<long double>(denominator.value()) /
                            static_cast<long double>(denominator.timescale());
  return right == 0.0L ? 0.0 : static_cast<double>(left / right);
}

[[nodiscard]] EffectValue interpolate(const EffectValue& left, const EffectValue& right,
                                      const double progress) {
  return std::visit(
      [progress](const auto& lhs, const auto& rhs) -> EffectValue {
        using Lhs = std::decay_t<decltype(lhs)>;
        using Rhs = std::decay_t<decltype(rhs)>;
        if constexpr (!std::is_same_v<Lhs, Rhs>) {
          return lhs;
        } else if constexpr (std::is_same_v<Lhs, std::int64_t>) {
          return roundTiesEven(static_cast<long double>(lhs) +
                               (static_cast<long double>(rhs) - static_cast<long double>(lhs)) *
                                   static_cast<long double>(progress));
        } else if constexpr (std::is_same_v<Lhs, double>) {
          return lhs + ((rhs - lhs) * progress);
        } else if constexpr (std::is_same_v<Lhs, Vec2>) {
          return Vec2{lhs.x + ((rhs.x - lhs.x) * progress), lhs.y + ((rhs.y - lhs.y) * progress)};
        } else if constexpr (std::is_same_v<Lhs, ColorRgba>) {
          return ColorRgba{lhs.red + ((rhs.red - lhs.red) * progress),
                           lhs.green + ((rhs.green - lhs.green) * progress),
                           lhs.blue + ((rhs.blue - lhs.blue) * progress),
                           lhs.alpha + ((rhs.alpha - lhs.alpha) * progress)};
        } else {
          return lhs;
        }
      },
      left, right);
}

} // namespace

std::optional<std::string> validateEffectParameter(const EffectParameter& parameter,
                                                   const std::optional<Time> clip_duration) {
  if (parameter.id.empty()) {
    return "effect parameter id cannot be empty";
  }
  if (!finite(parameter.value)) {
    return "effect parameter base value must be finite";
  }
  if (clip_duration && (clip_duration->isNegative() || clip_duration->isZero())) {
    return "effect parameter clip duration must be positive";
  }

  const Keyframe* previous = nullptr;
  for (const auto& keyframe : parameter.keyframes) {
    if (keyframe.time.isNegative()) {
      return "effect keyframe time cannot be negative";
    }
    if (clip_duration && keyframe.time >= *clip_duration) {
      return "effect keyframe time must be before the clip duration";
    }
    if (previous != nullptr && keyframe.time <= previous->time) {
      return "effect keyframe times must be strictly increasing and unique";
    }
    if (keyframe.value.index() != parameter.value.index()) {
      return "effect keyframe value type must match the parameter value type";
    }
    if (!finite(keyframe.value) || !finite(keyframe.incoming_control) ||
        !finite(keyframe.outgoing_control)) {
      return "effect keyframe values and controls must be finite";
    }
    const bool valid_incoming =
        keyframe.incoming_control.x >= -1.0 && keyframe.incoming_control.x <= 0.0 &&
        keyframe.incoming_control.y >= -1.0 && keyframe.incoming_control.y <= 1.0;
    const bool valid_outgoing =
        keyframe.outgoing_control.x >= 0.0 && keyframe.outgoing_control.x <= 1.0 &&
        keyframe.outgoing_control.y >= -1.0 && keyframe.outgoing_control.y <= 1.0;
    if (!valid_incoming || !valid_outgoing) {
      return "effect keyframe controls must be normalized offsets from their keyframe";
    }
    // A Bezier segment consumes the outgoing handle from the left sample and
    // the incoming handle from the right sample. Compare those two handles,
    // not the handles stored on one keyframe.
    if (previous != nullptr && previous->interpolation == KeyframeInterpolation::Bezier &&
        previous->outgoing_control.x > 1.0 + keyframe.incoming_control.x) {
      return "Bezier keyframe time controls must be ordered";
    }
    previous = &keyframe;
  }
  return std::nullopt;
}

std::optional<std::string> validateEffect(const Effect& effect,
                                          const std::optional<Time> clip_duration) {
  if (effect.type.empty() || effect.version == 0U) {
    return "effects require a type and non-zero version";
  }
  for (const auto& [map_id, parameter] : effect.parameters) {
    if (map_id != parameter.id) {
      return "effect parameter map key must match its parameter id";
    }
    if (const auto issue = validateEffectParameter(parameter, clip_duration)) {
      return issue;
    }
    if (!effect.known) {
      continue;
    }
    const auto validate_number = [&](const double minimum,
                                     const double maximum) -> std::optional<std::string> {
      const auto check = [&](const EffectValue& value) {
        const auto* number = std::get_if<double>(&value);
        return number != nullptr && std::isfinite(*number) && *number >= minimum &&
               *number <= maximum;
      };
      if (!check(parameter.value)) {
        return "known effect parameter value is outside its canonical range";
      }
      for (const auto& keyframe : parameter.keyframes) {
        if (!check(keyframe.value)) {
          return "known effect keyframe value is outside its canonical range";
        }
      }
      return std::nullopt;
    };

    std::optional<std::pair<double, double>> range;
    if (effect.type == "video.color") {
      if (parameter.id == "exposure") {
        range = std::pair{-32.0, 32.0};
      } else if (parameter.id == "contrast" || parameter.id == "saturation") {
        range = std::pair{0.0, 8.0};
      } else if (parameter.id == "temperature" || parameter.id == "tint") {
        range = std::pair{-1.0, 1.0};
      }
    } else if (effect.type == "video.crop") {
      range = std::pair{0.0, 1.0};
    } else if (effect.type == "video.gaussian_blur" && parameter.id == "radius") {
      range = std::pair{0.0, 64.0};
    } else if (effect.type == "audio.eq") {
      if (parameter.id == "frequency_hz")
        range = std::pair{20.0, 20'000.0};
      else if (parameter.id == "quality")
        range = std::pair{0.1, 20.0};
      else if (parameter.id == "gain_db")
        range = std::pair{-96.0, 24.0};
    } else if (effect.type == "audio.compressor") {
      if (parameter.id == "threshold_db")
        range = std::pair{-96.0, 0.0};
      else if (parameter.id == "ratio")
        range = std::pair{1.0, 20.0};
      else if (parameter.id == "attack_ms")
        range = std::pair{0.1, 200.0};
      else if (parameter.id == "release_ms")
        range = std::pair{1.0, 2'000.0};
      else if (parameter.id == "makeup_db")
        range = std::pair{-96.0, 24.0};
    } else if (effect.type == "audio.dialogue_denoise") {
      if (parameter.id == "strength")
        range = std::pair{0.0, 1.0};
      else if (parameter.id == "threshold_db")
        range = std::pair{-96.0, 0.0};
    } else if (effect.type == "audio.limiter" && parameter.id == "ceiling_db") {
      range = std::pair{-24.0, 0.0};
    }
    if (range) {
      if (const auto issue = validate_number(range->first, range->second)) {
        return issue;
      }
    }
  }
  return std::nullopt;
}

std::optional<EffectValue> evaluateEffectParameter(const EffectParameter& parameter,
                                                   const Time clip_local_time) {
  if (validateEffectParameter(parameter)) {
    return std::nullopt;
  }
  if (parameter.keyframes.empty()) {
    return parameter.value;
  }
  if (clip_local_time < parameter.keyframes.front().time) {
    return parameter.value;
  }
  const auto upper = std::upper_bound(
      parameter.keyframes.begin(), parameter.keyframes.end(), clip_local_time,
      [](const Time time, const Keyframe& keyframe) { return time < keyframe.time; });
  if (upper == parameter.keyframes.begin()) {
    return upper->value;
  }
  if (upper == parameter.keyframes.end()) {
    return parameter.keyframes.back().value;
  }

  const auto& right = *upper;
  const auto& left = *std::prev(upper);
  if (clip_local_time == right.time) {
    return right.value;
  }
  const double progress =
      std::clamp(timeRatio(clip_local_time - left.time, right.time - left.time), 0.0, 1.0);
  if (left.interpolation == KeyframeInterpolation::Hold) {
    return left.value;
  }
  const double adjusted =
      left.interpolation == KeyframeInterpolation::Bezier
          ? bezierProgress(left.outgoing_control, right.incoming_control, progress)
          : progress;
  return interpolate(left.value, right.value, adjusted);
}

} // namespace video_editor::edit
