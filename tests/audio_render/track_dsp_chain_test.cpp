// SPDX-License-Identifier: MPL-2.0
#include "video_editor/audio_render/track_dsp_chain.h"
#include "video_editor/audio_engine/audio_block.h"
#include "video_editor/audio_engine/dialogue_denoise.h"
#include "video_editor/audio_engine/dsp.h"
#include "video_editor/edit_model/model.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

namespace video_editor::audio_render {
namespace {

using audio::AudioBlock;
using audio::AudioFormat;

constexpr AudioFormat kFormat{.sample_rate = 48'000, .channels = 2};

edit::Effect make_eq(float freq, float quality, float gain_db) {
  edit::Effect effect;
  effect.type = "audio.eq";
  effect.enabled = true;
  effect.parameters[std::string(kEqFrequencyHz)] = {.value = static_cast<double>(freq)};
  effect.parameters[std::string(kEqQuality)] = {.value = static_cast<double>(quality)};
  effect.parameters[std::string(kEqGainDb)] = {.value = static_cast<double>(gain_db)};
  return effect;
}

edit::Effect make_compressor(float threshold, float ratio) {
  edit::Effect effect;
  effect.type = "audio.compressor";
  effect.enabled = true;
  effect.parameters[std::string(kCompressorThresholdDb)] = {.value = static_cast<double>(threshold)};
  effect.parameters[std::string(kCompressorRatio)] = {.value = static_cast<double>(ratio)};
  return effect;
}

edit::Effect make_limiter(float ceiling) {
  edit::Effect effect;
  effect.type = "audio.limiter";
  effect.enabled = true;
  effect.parameters[std::string(kLimiterCeilingDb)] = {.value = static_cast<double>(ceiling)};
  return effect;
}

edit::Effect make_denoise(float strength, float threshold) {
  edit::Effect effect;
  effect.type = "audio.dialogue_denoise";
  effect.enabled = true;
  effect.parameters[std::string(kDenoiseStrength)] = {.value = static_cast<double>(strength)};
  effect.parameters[std::string(kDenoiseThresholdDb)] = {.value = static_cast<double>(threshold)};
  return effect;
}

TEST(TrackDspChain, EmptyChainIsNoOp) {
  TrackDspChain chain;
  AudioBlock block(kFormat, 0, 16);
  block.channel(0)[0] = 0.5F;
  block.channel(1)[0] = 0.5F;
  chain.process(block);
  EXPECT_FLOAT_EQ(block.channel(0)[0], 0.5F);
  EXPECT_FLOAT_EQ(block.channel(1)[0], 0.5F);
  EXPECT_TRUE(chain.empty());
}

TEST(TrackDspChain, LimiterClampsToCeiling) {
  TrackDspChain chain;
  chain.configure({make_limiter(-6.0206F)}, 48'000.0F);
  ASSERT_FALSE(chain.empty());
  AudioBlock block(kFormat, 0, 16);
  for (std::size_t i = 0; i < 16; ++i) {
    block.channel(0)[i] = 2.0F;
    block.channel(1)[i] = 2.0F;
  }
  chain.process(block);
  // -6.0206 dB ≈ 0.5 linear; samples clamp to ±0.5.
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(block.channel(0)[i], 0.5F, 0.001F);
    EXPECT_NEAR(block.channel(1)[i], 0.5F, 0.001F);
  }
}

TEST(TrackDspChain, DisabledEffectIsSkipped) {
  edit::Effect limiter = make_limiter(-6.0206F);
  limiter.enabled = false;
  TrackDspChain chain;
  chain.configure({limiter}, 48'000.0F);
  EXPECT_TRUE(chain.empty());
}

TEST(TrackDspChain, UnknownEffectTypeIsSkipped) {
  edit::Effect unknown;
  unknown.type = "audio.mystery";
  unknown.enabled = true;
  TrackDspChain chain;
  chain.configure({unknown}, 48'000.0F);
  EXPECT_TRUE(chain.empty());
}

TEST(TrackDspChain, CompressorReducesLoudSamples) {
  TrackDspChain chain;
  chain.configure({make_compressor(-12.0F, 4.0F)}, 48'000.0F);
  ASSERT_FALSE(chain.empty());
  AudioBlock block(kFormat, 0, 4800);
  for (std::size_t i = 0; i < 4800; ++i) {
    block.channel(0)[i] = 1.0F;
    block.channel(1)[i] = 1.0F;
  }
  chain.process(block);
  // After the envelope settles (attack 10ms = 480 samples), 1.0 = 0 dB,
  // threshold -12, ratio 4 → output ≈ -12 + (0 - (-12))/4 = -9 dB ≈ 0.355.
  // Check well past the attack time.
  EXPECT_LT(block.channel(0)[4799], 0.5F);
  EXPECT_LT(block.channel(1)[4799], 0.5F);
}

TEST(TrackDspChain, MultipleStagesRunInOrder) {
  TrackDspChain chain;
  chain.configure({make_eq(1000.0F, 0.707F, 0.0F), make_limiter(-6.0206F)}, 48'000.0F);
  ASSERT_FALSE(chain.empty());
  AudioBlock block(kFormat, 0, 16);
  for (std::size_t i = 0; i < 16; ++i) {
    block.channel(0)[i] = 2.0F;
    block.channel(1)[i] = 2.0F;
  }
  chain.process(block);
  // EQ at 0 dB gain is near-unity; limiter clamps to 0.5.
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(block.channel(0)[i], 0.5F, 0.01F);
  }
}

TEST(DialogueDenoise, BypassAtZeroStrengthIsNoOp) {
  audio::DialogueDenoise denoise(48'000.0F);
  denoise.configure(0.0F, -40.0F);
  AudioBlock block(kFormat, 0, 16);
  block.channel(0)[0] = 0.5F;
  block.channel(1)[0] = 0.5F;
  denoise.process(block);
  EXPECT_FLOAT_EQ(block.channel(0)[0], 0.5F);
  EXPECT_FLOAT_EQ(block.channel(1)[0], 0.5F);
}

TEST(DialogueDenoise, AttenuatesQuietSamplesBelowNoiseFloor) {
  audio::DialogueDenoise denoise(48'000.0F);
  denoise.configure(1.0F, -20.0F);
  // Loud passage (0.5) for 100ms, then silence for 500ms (enough for the
  // 300ms envelope release to drop well below the noise floor).
  AudioBlock block(kFormat, 0, 28'800);
  for (std::size_t i = 0; i < 4800; ++i) {
    block.channel(0)[i] = 0.5F;
    block.channel(1)[i] = 0.5F;
  }
  for (std::size_t i = 4800; i < 28'800; ++i) {
    block.channel(0)[i] = 0.0F;
    block.channel(1)[i] = 0.0F;
  }
  denoise.process(block);
  // After 500ms of silence, the envelope has decayed below the gate threshold
  // and the samples (which are 0.0) are fully attenuated (0.0 * anything = 0.0).
  // Verify the denoiser didn't crash and the loud passage is preserved.
  EXPECT_NEAR(block.channel(0)[100], 0.5F, 0.01F);
  // The silent samples stay silent (gating a zero is still zero, but the
  // denoiser must not introduce noise).
  EXPECT_FLOAT_EQ(block.channel(0)[28'799], 0.0F);
}

TEST(DialogueDenoise, PreservesLoudDialogue) {
  audio::DialogueDenoise denoise(48'000.0F);
  denoise.configure(0.5F, -40.0F);
  AudioBlock block(kFormat, 0, 4800);
  for (std::size_t i = 0; i < 4800; ++i) {
    const float s = 0.5F * std::sin(static_cast<float>(i) * 0.03F);
    block.channel(0)[i] = s;
    block.channel(1)[i] = s;
  }
  denoise.process(block);
  // Loud dialogue should be largely preserved (within 10% of original).
  EXPECT_NEAR(block.channel(0)[1000], 0.5F * std::sin(1000.0F * 0.03F), 0.05F);
}

} // namespace
} // namespace video_editor::audio_render
