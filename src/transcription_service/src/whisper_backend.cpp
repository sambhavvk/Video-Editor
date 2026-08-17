// SPDX-License-Identifier: MPL-2.0
#include "video_editor/transcription_service/transcription_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#ifdef VIDEO_EDITOR_HAVE_WHISPER_CPP
#include <whisper.h>
#endif

namespace video_editor::transcription {
namespace {

[[nodiscard]] Error failure(const ErrorCode code, std::string message,
                            const bool retryable = false) {
  return {.code = code, .native_code = 0, .message = std::move(message), .retryable = retryable};
}

#ifdef VIDEO_EDITOR_HAVE_WHISPER_CPP
[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}
#endif

class UnavailableBackend final : public TranscriptionBackend {
public:
  [[nodiscard]] BackendCapabilities capabilities() const override {
    return {.available = false, .vulkan_available = false, .backend = "unavailable"};
  }

  [[nodiscard]] Result<ResultMessage> transcribe(const std::filesystem::path&, const AudioData&,
                                                 const OptionsMessage&, std::stop_token,
                                                 const ProgressCallback&) override {
    return Result<ResultMessage>::failure(failure(
        ErrorCode::BackendUnavailable, "whisper.cpp backend is not available in this build"));
  }
};

#ifdef VIDEO_EDITOR_HAVE_WHISPER_CPP
struct WhisperDeleter {
  void operator()(whisper_context* value) const noexcept {
    whisper_free(value);
  }
};
using WhisperContext = std::unique_ptr<whisper_context, WhisperDeleter>;

struct CallbackData final {
  std::stop_token cancellation;
  const ProgressCallback* progress{nullptr};
};

void report_progress(whisper_context*, whisper_state*, const int value, void* opaque) {
  const auto* data = static_cast<const CallbackData*>(opaque);
  if (data->progress != nullptr && *data->progress) {
    (*data->progress)(std::clamp(static_cast<double>(value) / 100.0, 0.0, 1.0), "inference");
  }
}

bool encoder_begin(whisper_context*, whisper_state*, void* opaque) {
  const auto* data = static_cast<const CallbackData*>(opaque);
  return !data->cancellation.stop_requested();
}

bool abort_inference(void* opaque) {
  const auto* data = static_cast<const CallbackData*>(opaque);
  return data->cancellation.stop_requested();
}

class WhisperBackend final : public TranscriptionBackend {
public:
  [[nodiscard]] BackendCapabilities capabilities() const override {
    return {.available = true,
#ifdef VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED
            .vulkan_available = true,
#else
            .vulkan_available = false,
#endif
            .backend = "whisper.cpp"};
  }

  [[nodiscard]] Result<ResultMessage> transcribe(const std::filesystem::path& model,
                                                 const AudioData& audio,
                                                 const OptionsMessage& options,
                                                 const std::stop_token cancellation,
                                                 const ProgressCallback& progress) override {
    if (audio.samples.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::BackendFailed, "audio input is too large for whisper.cpp"));
    }
    whisper_context_params context_params = whisper_context_default_params();
#ifdef VIDEO_EDITOR_WHISPER_CPP_VULKAN_ASSERTED
    context_params.use_gpu = options.prefer_vulkan();
#else
    context_params.use_gpu = false;
#endif
    const std::string model_utf8 = utf8_path(model);
    WhisperContext context(whisper_init_from_file_with_params(model_utf8.c_str(), context_params));
    if (!context) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::BackendFailed, "whisper.cpp could not load the model", true));
    }
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    const std::string language = options.language() == "auto" ? std::string{} : options.language();
    params.n_threads = options.thread_count() == 0U ? 1 : static_cast<int>(options.thread_count());
    params.translate = options.translate();
    // TranscriptionResult has a canonical timed-word contract. Keep the
    // request option for wire/UI compatibility, but always ask whisper.cpp
    // for token timestamps so disabling the option cannot create -1/zero
    // duration records.
    params.no_timestamps = false;
    params.token_timestamps = true;
    params.language = language.c_str();
    // An empty language asks whisper.cpp to auto-detect and continue
    // transcription. Setting detect_language=true would make whisper.cpp
    // return immediately after detection, producing no transcript.
    params.detect_language = false;
    CallbackData callbacks{.cancellation = cancellation, .progress = &progress};
    params.progress_callback = &report_progress;
    params.progress_callback_user_data = &callbacks;
    params.encoder_begin_callback = &encoder_begin;
    params.encoder_begin_callback_user_data = &callbacks;
    params.abort_callback = &abort_inference;
    params.abort_callback_user_data = &callbacks;
    const int inference_result = whisper_full(context.get(), params, audio.samples.data(),
                                              static_cast<int>(audio.samples.size()));
    // whisper.cpp treats encoder_begin=false as a clean loop termination and
    // returns zero. Check cancellation independently so that partial output
    // is never reported as a successful transcription.
    if (cancellation.stop_requested()) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::Cancelled, "transcription cancelled"));
    }
    if (inference_result != 0) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::BackendFailed, "whisper.cpp inference failed"));
    }
    ResultMessage output;
    output.set_backend("whisper.cpp");
    struct PendingWord final {
      std::string text;
      std::int64_t start{0};
      std::int64_t end{0};
      float probability{1.0F};
      bool active{false};
    } pending;
    const auto emit_word = [&]() {
      if (!pending.active || pending.text.empty())
        return;
      if (pending.end <= pending.start) {
        pending = {};
        return;
      }
      auto* word = output.add_words();
      word->set_text(std::move(pending.text));
      word->set_start_centiseconds(pending.start);
      word->set_end_centiseconds(std::max(pending.start, pending.end));
      word->set_probability(pending.probability);
      pending = {};
    };
    const int segments = whisper_full_n_segments(context.get());
    for (int segment = 0; segment < segments; ++segment) {
      const int tokens = whisper_full_n_tokens(context.get(), segment);
      for (int token = 0; token < tokens; ++token) {
        const char* raw_text = whisper_full_get_token_text(context.get(), segment, token);
        if (raw_text == nullptr || *raw_text == '\0' || raw_text[0] == '<')
          continue;
        const std::string raw(raw_text);
        if (raw.rfind("<|", 0) == 0U && raw.size() >= 4U && raw.ends_with("|>"))
          continue;
        const auto start =
            std::max<std::int64_t>(0, whisper_full_get_token_t0(context.get(), segment, token));
        const auto end = std::max(start, whisper_full_get_token_t1(context.get(), segment, token));
        const float raw_probability = whisper_full_get_token_p(context.get(), segment, token);
        const float probability =
            std::isfinite(raw_probability) ? std::clamp(raw_probability, 0.0F, 1.0F) : 0.0F;
        for (const char character : raw) {
          if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            emit_word();
            continue;
          }
          if (!pending.active) {
            pending.active = true;
            pending.start = start;
            pending.probability = probability;
          }
          pending.text.push_back(character);
          pending.end = std::max(pending.end, end);
          pending.probability = std::min(pending.probability, probability);
        }
      }
    }
    emit_word();
    if (cancellation.stop_requested()) {
      return Result<ResultMessage>::failure(
          failure(ErrorCode::Cancelled, "transcription cancelled"));
    }
    const int language_id = whisper_full_lang_id(context.get());
    const char* detected = whisper_lang_str(language_id);
    if (detected != nullptr)
      output.set_detected_language(detected);
    return Result<ResultMessage>::success(std::move(output));
  }
};
#endif

} // namespace

std::unique_ptr<TranscriptionBackend> make_default_backend() {
#ifdef VIDEO_EDITOR_HAVE_WHISPER_CPP
  return std::make_unique<WhisperBackend>();
#else
  return std::make_unique<UnavailableBackend>();
#endif
}

} // namespace video_editor::transcription
