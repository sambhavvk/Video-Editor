// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/track_dsp_chain.h"

#include "video_editor/audio_engine/dialogue_denoise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <variant>

namespace video_editor::audio_render {

namespace {

[[nodiscard]] double parameter_value(const edit::Effect& effect, std::string_view key,
                                     double fallback) {
  const auto it = effect.parameters.find(key);
  if (it == effect.parameters.end()) {
    return fallback;
  }
  const edit::EffectValue& value = it->second.value;
  if (std::holds_alternative<double>(value)) {
    return std::get<double>(value);
  }
  if (std::holds_alternative<std::int64_t>(value)) {
    return static_cast<double>(std::get<std::int64_t>(value));
  }
  return fallback;
}

[[nodiscard]] float param(const edit::Effect& effect, std::string_view key, float fallback) {
  return static_cast<float>(parameter_value(effect, key, static_cast<double>(fallback)));
}

class EqStage final {
public:
  EqStage(float sample_rate, const edit::Effect& effect)
      : biquad_(audio::Biquad::peaking(sample_rate, param(effect, kEqFrequencyHz, 1000.0F),
                                       std::max(param(effect, kEqQuality, 0.707F), 0.1F),
                                       param(effect, kEqGainDb, 0.0F))) {}
  void process(audio::AudioBlock& block) noexcept {
    biquad_.process(block);
  }

private:
  audio::Biquad biquad_;
};

class CompressorStage final {
public:
  CompressorStage(float sample_rate, const edit::Effect& effect) : compressor_(sample_rate) {
    audio::CompressorSettings settings;
    settings.threshold_db = param(effect, kCompressorThresholdDb, -18.0F);
    settings.ratio = std::max(param(effect, kCompressorRatio, 4.0F), 1.0F);
    settings.attack_ms = std::max(param(effect, kCompressorAttackMs, 10.0F), 0.1F);
    settings.release_ms = std::max(param(effect, kCompressorReleaseMs, 100.0F), 1.0F);
    settings.makeup_db = param(effect, kCompressorMakeupDb, 0.0F);
    compressor_.configure(settings);
  }
  void process(audio::AudioBlock& block) noexcept {
    compressor_.process(block);
  }

private:
  audio::Compressor compressor_;
};

class LimiterStage final {
public:
  explicit LimiterStage(const edit::Effect& effect) {
    const float ceiling = param(effect, kLimiterCeilingDb, -1.0F);
    limiter_.set_ceiling(ceiling);
  }
  void process(audio::AudioBlock& block) noexcept {
    limiter_.process(block);
  }

private:
  audio::LookaheadFreeLimiter limiter_;
};

class DenoiseStage final {
public:
  DenoiseStage(float sample_rate, const edit::Effect& effect) : denoise_(sample_rate) {
    const float strength = std::clamp(param(effect, kDenoiseStrength, 0.5F), 0.0F, 1.0F);
    const float threshold = param(effect, kDenoiseThresholdDb, -40.0F);
    denoise_.configure(strength, threshold);
  }
  void process(audio::AudioBlock& block) noexcept {
    denoise_.process(block);
  }

private:
  audio::DialogueDenoise denoise_;
};

class StageBase {
public:
  virtual ~StageBase() = default;
  virtual void process(audio::AudioBlock& block) noexcept = 0;
};

template <typename T> class StageWrapper final : public StageBase {
public:
  template <typename... Args>
  explicit StageWrapper(Args&&... args) : stage_(std::forward<Args>(args)...) {}
  void process(audio::AudioBlock& block) noexcept override {
    stage_.process(block);
  }

private:
  T stage_;
};

} // namespace

class TrackDspChain::Impl final {
public:
  std::vector<std::unique_ptr<StageBase>> stages;

  void configure(const std::vector<edit::Effect>& effects, float sample_rate) {
    stages.clear();
    // The persisted vector is presentation/order metadata. Audio processing
    // has one canonical order so equivalent projects cannot produce different
    // output merely because an inspector reordered the effect rows:
    // parametric EQ -> compressor -> dialogue denoise -> limiter. Multiple
    // stages of one type retain their persisted relative order.
    constexpr std::array<std::string_view, 4> canonical_types{
        "audio.eq", "audio.compressor", "audio.dialogue_denoise", "audio.limiter"};
    for (const auto type : canonical_types) {
      for (const edit::Effect& effect : effects) {
        if (!effect.enabled || effect.type != type) {
          continue;
        }
        if (type == "audio.eq") {
          stages.push_back(std::make_unique<StageWrapper<EqStage>>(sample_rate, effect));
        } else if (type == "audio.compressor") {
          stages.push_back(std::make_unique<StageWrapper<CompressorStage>>(sample_rate, effect));
        } else if (type == "audio.dialogue_denoise") {
          stages.push_back(std::make_unique<StageWrapper<DenoiseStage>>(sample_rate, effect));
        } else if (type == "audio.limiter") {
          stages.push_back(std::make_unique<StageWrapper<LimiterStage>>(effect));
        }
      }
    }
  }

  void process(audio::AudioBlock& block) noexcept {
    for (auto& stage : stages) {
      stage->process(block);
    }
  }
};

TrackDspChain::TrackDspChain() : impl_(std::make_unique<Impl>()) {}
TrackDspChain::~TrackDspChain() = default;
TrackDspChain::TrackDspChain(TrackDspChain&&) noexcept = default;
TrackDspChain& TrackDspChain::operator=(TrackDspChain&&) noexcept = default;

void TrackDspChain::configure(const std::vector<edit::Effect>& effects, float sample_rate) {
  impl_->configure(effects, sample_rate);
}

void TrackDspChain::process(audio::AudioBlock& block) noexcept {
  if (impl_ == nullptr || impl_->stages.empty()) {
    return;
  }
  impl_->process(block);
}

bool TrackDspChain::empty() const noexcept {
  return impl_ == nullptr || impl_->stages.empty();
}

} // namespace video_editor::audio_render
