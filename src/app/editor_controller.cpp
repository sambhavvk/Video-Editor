// SPDX-License-Identifier: MPL-2.0
#include "editor_controller.hpp"
#include "media_reconstruction.hpp"
#include "path_utils.hpp"
#include "session_event_log.hpp"
#include "worker_host_session.hpp"

#include "video_editor/audio_engine/async_realtime_playback.h"
#include "video_editor/audio_engine/miniaudio_output_device.h"
#include "video_editor/audio_render/loudness_normalize.h"
#include "video_editor/audio_render/original_audio_registry.h"
#include "video_editor/audio_render/timeline_audio_renderer.h"
#include "video_editor/caption_service/caption_service.h"
#include "video_editor/desktop_ui/cache_browser_dialog.hpp"
#include "video_editor/desktop_ui/editor_window.hpp"
#include "video_editor/desktop_ui/panel_widgets.hpp"
#include "video_editor/desktop_ui/program_viewer.hpp"
#include "video_editor/desktop_ui/timeline_widget.hpp"
#include "video_editor/export_service/export_service.h"
#include "video_editor/job_service/job_id.h"
#include "video_editor/job_service/protocol.h"
#include "video_editor/media_cache/cache_store.h"
#include "video_editor/media_cache/metadata_service.h"
#include "video_editor/media_cache/thumbnail_service.h"
#include "video_editor/media_cache/waveform_service.h"
#include "video_editor/playback/asset_registry.h"
#include "video_editor/playback/ffmpeg_frame_provider.h"
#include "video_editor/project_codec/project_codec.h"
#include "video_editor/project_store/project_store.hpp"
#include "video_editor/proxy_service/proxy_service.h"
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/frame.h"
#include "video_editor/render_engine/gpu_backend.h"
#include "video_editor/render_engine/gpu_timeline_renderer.h"
#include "video_editor/transcription_service/transcription_service.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace video_editor::app {

AudioDevicePollDecision
evaluateAudioDevicePoll(const std::span<const audio::AudioDeviceInfo> previous,
                        const std::span<const audio::AudioDeviceInfo> current,
                        const std::string_view selected_id) noexcept {
  const auto contains = [](const std::span<const audio::AudioDeviceInfo> devices,
                           const std::string_view id) {
    return std::any_of(devices.begin(), devices.end(),
                       [id](const auto& device) { return device.connected && device.id == id; });
  };
  const auto has_default = [](const std::span<const audio::AudioDeviceInfo> devices) {
    return std::any_of(devices.begin(), devices.end(),
                       [](const auto& device) { return device.connected && device.is_default; });
  };
  if (!selected_id.empty()) {
    return {.selected_missing = !contains(current, selected_id),
            .default_missing = false,
            .selected_recovered =
                contains(current, selected_id) && !contains(previous, selected_id),
            .default_recovered = false};
  }
  return {.selected_missing = false,
          .default_missing = !current.empty() && !has_default(current),
          .selected_recovered = false,
          .default_recovered = has_default(current) && !has_default(previous)};
}

AudioMixerOutputStatus audioMixerOutputStatus(const bool backend_available,
                                              const bool selected_lost) noexcept {
  if (!backend_available) {
    return AudioMixerOutputStatus::BackendMissing;
  }
  if (selected_lost) {
    return AudioMixerOutputStatus::SelectedUnavailable;
  }
  return AudioMixerOutputStatus::Ready;
}

std::vector<edit::CaptionWord> mapTranscriptionWordsToTimeline(
    const std::span<const edit::CaptionWord> source_words, const edit::TimeRange source_range,
    const edit::TimeRange timeline_range, const edit::Rate playback_rate, const bool reversed) {
  std::vector<edit::CaptionWord> mapped;
  if (source_range.empty() || timeline_range.empty() || playback_rate.numerator() == 0U ||
      playback_rate.denominator() == 0U) {
    return mapped;
  }

  const auto map_source_time = [&](const edit::Time source_time) {
    const edit::Time source_offset =
        reversed ? source_range.end() - source_time : source_time - source_range.start;
    const edit::Time timeline_offset =
        source_offset.scaled(playback_rate.denominator(), playback_rate.numerator(),
                             edit::RoundingMode::NearestTiesEven);
    const edit::Time timeline_time = timeline_range.start + timeline_offset;
    return std::clamp(timeline_time, timeline_range.start, timeline_range.end());
  };

  mapped.reserve(source_words.size());
  for (const auto& source_word : source_words) {
    const edit::Time start =
        source_word.range.start < source_range.start ? source_range.start : source_word.range.start;
    const edit::Time end =
        source_word.range.end() > source_range.end() ? source_range.end() : source_word.range.end();
    if (end <= start) {
      continue;
    }
    const edit::Time mapped_start = map_source_time(start);
    const edit::Time mapped_end = map_source_time(end);
    const edit::Time timeline_start = std::min(mapped_start, mapped_end);
    const edit::Time timeline_end = std::max(mapped_start, mapped_end);
    if (timeline_end <= timeline_start) {
      continue;
    }
    auto word = source_word;
    word.range = edit::TimeRange(timeline_start, timeline_end - timeline_start);
    mapped.push_back(std::move(word));
  }
  std::stable_sort(mapped.begin(), mapped.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.range.start != rhs.range.start) {
      return lhs.range.start < rhs.range.start;
    }
    return lhs.range.end() < rhs.range.end();
  });
  return mapped;
}

bool modelDownloadSizeAllowed(const std::uintmax_t bytes_received,
                              const std::int64_t content_length,
                              const std::uintmax_t expected_bytes) noexcept {
  if (bytes_received > expected_bytes)
    return false;
  return content_length < 0 || static_cast<std::uintmax_t>(content_length) == expected_bytes;
}

namespace {

constexpr qint64 kUiTimescale = 48'000;
constexpr qint64 kModelNetworkReadBufferBytes = 1'048'576;

[[nodiscard]] qint64 modelReplyContentLength(const QNetworkReply& reply) {
  bool valid = false;
  const qint64 value = reply.header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
  return valid && value >= 0 ? value : -1;
}

using desktop_ui::MediaItemView;
using desktop_ui::TimelineClipView;
using desktop_ui::TimelineTrackView;

constexpr qint64 kDefaultMediaCacheBudgetBytes = 100LL * 1024 * 1024 * 1024;
constexpr int kCacheJobThumbnail = 0;
constexpr int kCacheJobWaveform = 1;

[[nodiscard]] assets::AssetRecord* findImported(std::vector<assets::AssetRecord>& records,
                                                const std::string& id) {
  const auto found =
      std::find_if(records.begin(), records.end(),
                   [&id](const assets::AssetRecord& record) { return record.id == id; });
  return found == records.end() ? nullptr : &*found;
}

[[nodiscard]] bool recordHasVideo(const assets::AssetRecord& record) {
  if (record.descriptor.best_video_stream >= 0) {
    return true;
  }
  return std::any_of(record.descriptor.streams.begin(), record.descriptor.streams.end(),
                     [](const media::StreamDescriptor& stream) { return stream.video.has_value(); });
}

[[nodiscard]] bool recordHasAudio(const assets::AssetRecord& record) {
  if (record.descriptor.best_audio_stream >= 0) {
    return true;
  }
  return std::any_of(record.descriptor.streams.begin(), record.descriptor.streams.end(),
                     [](const media::StreamDescriptor& stream) { return stream.audio.has_value(); });
}

[[nodiscard]] bool storeErrorLooksFull(const std::string& message) {
  return message.find("budget") != std::string::npos || message.find("Full") != std::string::npos ||
         message.find("exceeds") != std::string::npos;
}

[[nodiscard]] QImage imageFromJpeg(const std::vector<std::byte>& jpeg) {
  QImage image;
  if (jpeg.empty()) {
    return image;
  }
  image.loadFromData(QByteArray(reinterpret_cast<const char*>(jpeg.data()),
                                static_cast<int>(jpeg.size())),
                     "JPEG");
  return image;
}

[[nodiscard]] QVector<desktop_ui::WaveformBucketView>
waveformBucketsForUi(const media_cache::Waveform& waveform) {
  if (waveform.levels.empty()) {
    return {};
  }
  const media_cache::WaveformLevel* chosen = &waveform.levels.front();
  std::int64_t best_diff = std::numeric_limits<std::int64_t>::max();
  for (const auto& level : waveform.levels) {
    const auto diff = level.bucket_count > 200 ? level.bucket_count - 200 : 200 - level.bucket_count;
    if (diff < best_diff) {
      best_diff = diff;
      chosen = &level;
    }
  }
  QVector<desktop_ui::WaveformBucketView> buckets;
  buckets.reserve(static_cast<qsizetype>(chosen->buckets.size()));
  for (const auto& bucket : chosen->buckets) {
    buckets.push_back({.minimum = bucket.minimum, .maximum = bucket.maximum});
  }
  return buckets;
}

[[nodiscard]] QString cacheKindText(const media_cache::CacheKind kind) {
  switch (kind) {
  case media_cache::CacheKind::Thumbnail:
    return QStringLiteral("Thumbnail");
  case media_cache::CacheKind::Waveform:
    return QStringLiteral("Waveform");
  case media_cache::CacheKind::Metadata:
    return QStringLiteral("Metadata");
  case media_cache::CacheKind::Proxy:
    return QStringLiteral("Proxy");
  case media_cache::CacheKind::ProxyPtsMap:
    return QStringLiteral("PTS map");
  }
  return QStringLiteral("Thumbnail");
}

[[nodiscard]] std::optional<media_cache::CacheKind> cacheKindFromText(const QString& text) {
  if (text == QLatin1String("Thumbnail")) {
    return media_cache::CacheKind::Thumbnail;
  }
  if (text == QLatin1String("Waveform")) {
    return media_cache::CacheKind::Waveform;
  }
  if (text == QLatin1String("Metadata")) {
    return media_cache::CacheKind::Metadata;
  }
  if (text == QLatin1String("Proxy")) {
    return media_cache::CacheKind::Proxy;
  }
  if (text == QLatin1String("PTS map")) {
    return media_cache::CacheKind::ProxyPtsMap;
  }
  return std::nullopt;
}

[[nodiscard]] proxy::ProxyProfile proxyProfileForHash(const assets::ProxyProfile& profile,
                                                      const bool ffv1) {
  return {.video_codec = ffv1 || profile.codec == assets::ProxyCodec::Ffv1
                             ? proxy::VideoCodec::Ffv1
                             : proxy::VideoCodec::ProResProxy,
          .scale_numerator = 1,
          .scale_denominator = 2,
          .maximum_width = profile.maximum_width,
          .maximum_height = profile.maximum_height,
          .include_pcm_audio = profile.include_pcm_audio,
          .allow_ffv1_fallback = true};
}

void appendUniqueSearchDirectory(std::vector<std::filesystem::path>& directories,
                                 std::unordered_set<std::string>& seen,
                                 const std::filesystem::path& directory) {
  if (directory.empty()) {
    return;
  }
  const std::string key = utf8_from_path(directory);
  if (seen.insert(key).second) {
    directories.push_back(directory);
  }
}

QString durationText(const edit::Time duration) {
  const double seconds =
      static_cast<double>(duration.value()) / static_cast<double>(duration.timescale());
  const qint64 total_seconds = std::max<qint64>(0, static_cast<qint64>(std::llround(seconds)));
  const qint64 hours = total_seconds / 3600;
  const qint64 minutes = (total_seconds % 3600) / 60;
  const qint64 remaining = total_seconds % 60;
  return QStringLiteral("%1:%2:%3")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(remaining, 2, 10, QLatin1Char('0'));
}

QString timecodeText(const qint64 position, const edit::Rate& rate) {
  const std::int64_t frame_number = rate.framesAt(
      edit::Time(std::max<qint64>(position, 0), static_cast<std::uint32_t>(kUiTimescale)),
      edit::RoundingMode::Floor);
  const std::int64_t nominal_fps = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(std::llround(static_cast<double>(rate.numerator()) /
                                                static_cast<double>(rate.denominator()))));
  const std::int64_t frames = frame_number % nominal_fps;
  const std::int64_t total_seconds = frame_number / nominal_fps;
  const std::int64_t seconds = total_seconds % 60;
  const std::int64_t minutes = (total_seconds / 60) % 60;
  const std::int64_t hours = total_seconds / 3'600;
  return QStringLiteral("%1:%2:%3:%4")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(frames, 2, 10, QLatin1Char('0'));
}

qint64 toUiTime(const edit::Time time) {
  return static_cast<qint64>(
      time.rescaledTo(static_cast<std::uint32_t>(kUiTimescale), edit::RoundingMode::NearestTiesEven)
          .value());
}

QString gapKey(const edit::EntityId& trackId, const edit::TimeRange& range) {
  return QStringLiteral("%1:%2/%3:%4/%5")
      .arg(QString::fromStdString(trackId.toString()))
      .arg(range.start.value())
      .arg(range.start.timescale())
      .arg(range.duration.value())
      .arg(range.duration.timescale());
}

QColor colorForTrack(const edit::TrackKind kind, const std::size_t index) {
  if (kind == edit::TrackKind::Audio) {
    return QColor::fromHsv(static_cast<int>((120U + (index * 23U)) % 360U), 110, 170);
  }
  if (kind == edit::TrackKind::Caption) {
    return QColor(166, 116, 190);
  }
  return QColor::fromHsv(static_cast<int>((205U + (index * 19U)) % 360U), 135, 185);
}

desktop_ui::TrackKind uiTrackKind(const edit::TrackKind kind) {
  switch (kind) {
  case edit::TrackKind::Audio:
    return desktop_ui::TrackKind::Audio;
  case edit::TrackKind::Caption:
    return desktop_ui::TrackKind::Caption;
  case edit::TrackKind::Video:
  default:
    return desktop_ui::TrackKind::Video;
  }
}

std::optional<edit::EntityId> parseId(const QString& text) {
  return edit::EntityId::parse(text.toStdString());
}

QString gpuBackendName(const render::GpuBackendKind backend) {
  switch (backend) {
  case render::GpuBackendKind::D3D11:
    return QStringLiteral("D3D11");
  case render::GpuBackendKind::Vulkan:
    return QStringLiteral("Vulkan");
  case render::GpuBackendKind::Auto:
  default:
    return QStringLiteral("GPU");
  }
}

bool isKnownPlatformPreset(const int value) noexcept {
  return value >= static_cast<int>(export_service::PlatformPreset::ReferenceFfv1) &&
         value <= static_cast<int>(export_service::PlatformPreset::PodcastAudioOnly);
}

QVariant effectValueForUi(const edit::EffectValue& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    return QVariant::fromValue<qlonglong>(*integer);
  }
  if (const auto* number = std::get_if<double>(&value)) {
    return *number;
  }
  if (const auto* boolean = std::get_if<bool>(&value)) {
    return *boolean;
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    return QString::fromStdString(*text);
  }
  if (const auto* time = std::get_if<edit::Time>(&value)) {
    return QString::fromStdString(time->toString());
  }
  if (const auto* vector = std::get_if<edit::Vec2>(&value)) {
    return QStringLiteral("%1, %2").arg(vector->x).arg(vector->y);
  }
  const auto& color = std::get<edit::ColorRgba>(value);
  return QStringLiteral("%1, %2, %3, %4")
      .arg(color.red)
      .arg(color.green)
      .arg(color.blue)
      .arg(color.alpha);
}

edit::EffectValue effectValueFromUi(const edit::EffectValue& original, const QVariant& value) {
  if (std::holds_alternative<std::int64_t>(original)) {
    return static_cast<std::int64_t>(value.toLongLong());
  }
  if (std::holds_alternative<double>(original)) {
    return value.toDouble();
  }
  if (std::holds_alternative<bool>(original)) {
    return value.toBool();
  }
  if (std::holds_alternative<std::string>(original)) {
    return value.toString().toStdString();
  }
  return original;
}

edit::EffectValue effectValueFromDouble(const edit::EffectValue& original, const double value) {
  if (std::holds_alternative<std::int64_t>(original)) {
    return static_cast<std::int64_t>(std::llround(value));
  }
  if (std::holds_alternative<double>(original)) {
    return value;
  }
  return original;
}

QString effectDisplayName(const std::string& type) {
  const QString id = QString::fromStdString(type);
  if (id == QStringLiteral("video.color")) {
    return QObject::tr("Color Adjustments");
  }
  if (id == QStringLiteral("video.crop")) {
    return QObject::tr("Crop");
  }
  if (id == QStringLiteral("video.gaussian_blur")) {
    return QObject::tr("Gaussian Blur");
  }
  if (id == QStringLiteral("audio.eq")) {
    return QObject::tr("Parametric Equalizer");
  }
  if (id == QStringLiteral("audio.compressor")) {
    return QObject::tr("Compressor");
  }
  if (id == QStringLiteral("audio.dialogue_denoise")) {
    return QObject::tr("Dialogue Noise Reduction");
  }
  if (id == QStringLiteral("audio.limiter")) {
    return QObject::tr("Limiter");
  }
  return id;
}

edit::Effect effectPreset(const QString& effectId) {
  edit::Effect effect;
  effect.type = effectId.toStdString();
  const auto add = [&effect](const char* id, edit::EffectValue value) {
    effect.parameters.emplace(
        id, edit::EffectParameter{.id = id, .value = std::move(value), .keyframes = {}});
  };
  if (effectId == QStringLiteral("video.color")) {
    add("exposure", 0.0);
    add("contrast", 1.0);
    add("saturation", 1.0);
    add("temperature", 0.0);
    add("tint", 0.0);
  } else if (effectId == QStringLiteral("video.crop")) {
    add("left", 0.0);
    add("top", 0.0);
    add("right", 0.0);
    add("bottom", 0.0);
  } else if (effectId == QStringLiteral("video.gaussian_blur")) {
    add("radius", 0.0);
  } else if (effectId == QStringLiteral("audio.eq")) {
    add("frequency", 1000.0);
    add("gain", 0.0);
    add("q", 1.0);
  } else if (effectId == QStringLiteral("audio.compressor")) {
    add("threshold", -18.0);
    add("ratio", 4.0);
    add("attack", 10.0);
    add("release", 100.0);
  } else if (effectId == QStringLiteral("audio.dialogue_denoise")) {
    add("strength", 0.5);
  } else {
    add("amount", 0.0);
  }
  return effect;
}

std::vector<std::byte> binaryPayload(const store::JournalEntry& entry) {
  if (const auto* bytes = std::get_if<store::BinaryPayload>(&entry.payload)) {
    return *bytes;
  }
  const auto& text = std::get<std::string>(entry.payload);
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](const char value) { return static_cast<std::byte>(value); });
  return bytes;
}

std::optional<std::uint32_t> projectSnapshotSchema(const store::JournalEntry& entry) {
  if (entry.command_type == "project.snapshot.v1") {
    return 1U;
  }
  if (entry.command_type == "project.snapshot.v2") {
    return 2U;
  }
  if (entry.command_type == "project.snapshot.v3") {
    return 3U;
  }
  return std::nullopt;
}

const store::JournalEntry* latestProjectSnapshot(const std::vector<store::JournalEntry>& entries) {
  const store::JournalEntry* result = nullptr;
  for (const auto& entry : entries) {
    const auto schema = projectSnapshotSchema(entry);
    if (!schema.has_value()) {
      if (entry.command_type.starts_with("project.snapshot.v")) {
        throw std::runtime_error("Project journal contains an unsupported snapshot version");
      }
      continue;
    }
    if (entry.payload_schema_version != *schema) {
      throw std::runtime_error("Project snapshot journal type does not match its payload schema");
    }
    result = &entry;
  }
  return result;
}

QString captionDiagnostics(const std::vector<caption_service::Diagnostic>& diagnostics) {
  QStringList lines;
  for (const auto& diagnostic : diagnostics) {
    lines.push_back(diagnostic.line == 0 ? QString::fromStdString(diagnostic.message)
                                         : QObject::tr("Line %1: %2")
                                               .arg(static_cast<qulonglong>(diagnostic.line))
                                               .arg(QString::fromStdString(diagnostic.message)));
  }
  return lines.join(QLatin1Char('\n'));
}

class EpochSyncFrameProvider final : public render::FrameProvider {
public:
  explicit EpochSyncFrameProvider(std::shared_ptr<playback::FfmpegFrameProvider> provider)
      : provider_(std::move(provider)) {}

  render::RenderResult<std::shared_ptr<const render::CpuFrame>>
  request(const render::AssetFrameRequest& request) override {
    provider_->begin_epoch(request.request_epoch);
    return provider_->request(request);
  }

private:
  std::shared_ptr<playback::FfmpegFrameProvider> provider_;
};

class TimelinePlaybackAudioProvider final : public audio::PlaybackAudioProvider {
public:
  TimelinePlaybackAudioProvider(std::shared_ptr<audio_render::TimelineAudioRenderer> renderer,
                                edit::TimelineSnapshot snapshot, const std::int64_t endSample)
      : renderer_(std::move(renderer)), snapshot_(std::move(snapshot)), end_sample_(endSample) {}

  audio::PlaybackRenderResult render(const audio::PlaybackRenderRequest& request) override {
    if (request.cancellation.stop_requested()) {
      return audio::PlaybackRenderResult::cancelled("audio pre-render request was cancelled");
    }
    if (request.start_sample >= end_sample_) {
      return audio::PlaybackRenderResult::end_of_stream();
    }
    auto rendered = renderer_->render(snapshot_, {.start_sample = request.start_sample,
                                                  .sample_count = request.sample_count,
                                                  .cancellation = request.cancellation});
    if (!rendered) {
      const auto& error = rendered.error();
      if (error.code == audio_render::AudioRenderErrorCode::Cancelled) {
        return audio::PlaybackRenderResult::cancelled(error.message);
      }
      return audio::PlaybackRenderResult::failure(error.message);
    }
    audio::AudioBlock block = std::move(rendered).value();
    if (block.start_sample() != request.start_sample ||
        block.frame_count() != request.sample_count ||
        block.format().sample_rate != audio::kPlaybackAudioFormat.sample_rate ||
        block.format().channels != audio::kPlaybackAudioFormat.channels) {
      return audio::PlaybackRenderResult::failure(
          "timeline audio renderer returned a block outside the requested 48 kHz stereo range");
    }
    return audio::PlaybackRenderResult::ready(std::move(block));
  }

private:
  std::shared_ptr<audio_render::TimelineAudioRenderer> renderer_;
  edit::TimelineSnapshot snapshot_;
  std::int64_t end_sample_{0};
};

} // namespace

EditorController::EditorController(desktop_ui::EditorWindow& window, QObject* parent)
    : QObject(parent), window_(window),
      playback_registry_(std::make_shared<playback::AssetRegistry>()),
      audio_registry_(std::make_shared<audio_render::OriginalAudioRegistry>()),
      frame_provider_(std::make_shared<playback::FfmpegFrameProvider>(playback_registry_)),
      renderer_(std::make_shared<render::CpuRenderer>(frame_provider_)) {
  try {
    media_cache_ = std::make_unique<media_cache::CacheStore>(mediaCacheDirectory());
    QSettings cache_settings;
    const qint64 budget =
        cache_settings
            .value(QStringLiteral("mediaCache/budgetBytes"), kDefaultMediaCacheBudgetBytes)
            .toLongLong();
    media_cache_->set_budget_bytes(
        static_cast<std::uint64_t>(std::max<qint64>(0, budget)));
    (void)media_cache_->evict_to_budget();
  } catch (const std::exception& exception) {
    media_cache_.reset();
    window_.showTransientMessage(
        tr("Media cache could not be opened: %1").arg(QString::fromUtf8(exception.what())));
  }
  transcription_network_ = new QNetworkAccessManager(this);
  if (auto gpu = render::GpuRenderer::create(); gpu != nullptr) {
    const render::GpuCapabilities capabilities = gpu->capabilities();
    if (capabilities.available() && capabilities.offscreen_rendering) {
      gpu_renderer_ = std::shared_ptr<render::GpuRenderer>(std::move(gpu));
      gpu_timeline_renderer_ =
          std::make_shared<render::GpuTimelineRenderer>(frame_provider_, gpu_renderer_);
      window_.programViewer()->setTitle(
          tr("Program · %1 GPU ready").arg(gpuBackendName(capabilities.backend)));
    } else {
      gpu_fallback_latched_ = true;
      window_.programViewer()->setTitle(tr("Program · CPU"));
    }
  } else {
    gpu_fallback_latched_ = true;
    window_.programViewer()->setTitle(tr("Program · CPU"));
  }
  window_.installEventFilter(this);
  connect(&window_, &desktop_ui::EditorWindow::newProjectRequested, this,
          &EditorController::newProject);
  connect(&window_, &desktop_ui::EditorWindow::openProjectRequested, this,
          &EditorController::openProject);
  connect(&window_, &desktop_ui::EditorWindow::saveProjectRequested, this,
          &EditorController::saveProject);
  connect(&window_, &desktop_ui::EditorWindow::saveProjectAsRequested, this,
          &EditorController::saveProjectAs);
  connect(&window_, &desktop_ui::EditorWindow::importMediaRequested, this,
          &EditorController::chooseMedia);
  connect(&window_, &desktop_ui::EditorWindow::mediaActivated, this,
          &EditorController::insertAsset);
  connect(&window_, &desktop_ui::EditorWindow::splitClipRequested, this,
          &EditorController::splitSelectedClip);
  connect(&window_, &desktop_ui::EditorWindow::deleteSelectionRequested, this,
          &EditorController::deleteSelectedClip);
  connect(&window_, &desktop_ui::EditorWindow::undoRequested, this, &EditorController::undo);
  connect(&window_, &desktop_ui::EditorWindow::redoRequested, this, &EditorController::redo);
  connect(&window_, &desktop_ui::EditorWindow::seekRequested, this, &EditorController::seek);
  connect(&window_, &desktop_ui::EditorWindow::playbackRateRequested, this,
          &EditorController::setPlaybackRate);
  connect(window_.programViewer(), &desktop_ui::ProgramViewer::filesDropped, this,
          &EditorController::importPaths);
  connect(window_.programViewer(), &desktop_ui::ProgramViewer::togglePlaybackRequested, this,
          [this] { setPlaybackRate(playback_rate_ == 0.0 ? 1.0 : 0.0); });
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::importCaptionsRequested, this,
          &EditorController::chooseCaptionFile);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::exportCaptionsRequested, this,
          &EditorController::chooseCaptionExport);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::captionActivated, this,
          &EditorController::seekCaption);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::wordActivated, this,
          [this](const QString&, const qint64 start) { seek(start); });
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::addCaptionRequested, this,
          &EditorController::addCaptionAtPlayhead);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::removeCaptionRequested, this,
          &EditorController::removeCaption);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::captionTextEdited, this,
          &EditorController::updateCaptionText);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::findInTranscriptRequested,
          this, &EditorController::searchTranscript);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::transcribeRequested, this,
          [this] {
            window_.captionsPanel()->setTranscriptionState(
                desktop_ui::TranscriptionState::ModelMissing,
                tr("Download the optional checksummed Whisper model before transcribing."));
          });
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::downloadModelRequested, this,
          &EditorController::downloadTranscriptionModel);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::transcribeWithOptionsRequested,
          this, &EditorController::startTranscription);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::cancelTranscriptionRequested,
          this, &EditorController::cancelTranscription);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::applyReviewRequested, this,
          &EditorController::applyCaptionReview);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::discardReviewRequested, this,
          &EditorController::discardCaptionReview);
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::reviewProposalToggled, this,
          [this](const QString& id, const bool selected) {
            for (auto& proposal : caption_proposals_) {
              if (proposal.id == id)
                proposal.selected = selected;
            }
          });
  connect(window_.captionsPanel(), &desktop_ui::CaptionsPanelWidget::captionStyleEdited, this,
          &EditorController::captionStyleEdited);
  connect(&window_, &desktop_ui::EditorWindow::exportRequested, this,
          &EditorController::chooseVideoExport);
  connect(window_.deliverPanel(), &desktop_ui::DeliverPanelWidget::destinationBrowseRequested, this,
          [this] {
            bool numeric = false;
            const int value = window_.deliverPanel()->selectedPresetId().toInt(&numeric);
            const auto platform = numeric && isKnownPlatformPreset(value)
                                      ? static_cast<export_service::PlatformPreset>(value)
                                      : export_service::PlatformPreset::ReferenceFfv1;
            const auto video = export_service::reference_video_preset_for(platform).value_or(
                export_service::VideoPreset::Ffv1Matroska);
            const QString extension = video == export_service::VideoPreset::Vp9OpusWebm
                                          ? QStringLiteral("webm")
                                          : (video == export_service::VideoPreset::ProRes422HqMov
                                                 ? QStringLiteral("mov")
                                                 : QStringLiteral("mkv"));
            const QString destination =
                QFileDialog::getSaveFileName(&window_, tr("Choose export destination"),
                                             QStringLiteral("export.%1").arg(extension),
                                             tr("Export files (*.%1)").arg(extension));
            if (!destination.isEmpty()) {
              window_.deliverPanel()->setDestinationPath(destination);
            }
          });
  connect(&window_, &desktop_ui::EditorWindow::parameterEdited, this,
          &EditorController::updateSelectedClipProperty);
  connect(&window_, &desktop_ui::EditorWindow::keyframeToggleRequested, this,
          &EditorController::toggleSelectedClipKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::effectAddRequested, this,
          &EditorController::addEffect);
  connect(&window_, &desktop_ui::EditorWindow::effectParameterEdited, this,
          &EditorController::updateSelectedEffectParameter);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeToggleRequested, this,
          &EditorController::toggleSelectedEffectKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeSelected, this,
          &EditorController::selectEffectKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeValueEdited, this,
          &EditorController::updateSelectedEffectKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeInterpolationEdited, this,
          &EditorController::updateSelectedEffectInterpolation);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeRemoved, this,
          &EditorController::removeSelectedEffectKeyframe);
  connect(&window_, &desktop_ui::EditorWindow::effectKeyframeControlPointsEdited, this,
          &EditorController::updateSelectedEffectControlPoints);
  connect(&window_, &desktop_ui::EditorWindow::addTitleRequested, this,
          &EditorController::addTitleClip);
  connect(&window_, &desktop_ui::EditorWindow::transitionActivated, this,
          [this](const QString& transitionId) { setTransitionSelection(transitionId); });
  connect(&window_, &desktop_ui::EditorWindow::transitionDurationEdited, this,
          &EditorController::updateTransitionDuration);
  connect(&window_, &desktop_ui::EditorWindow::transitionRemoved, this,
          &EditorController::removeTransition);
  connect(&window_, &desktop_ui::EditorWindow::transitionPresetChanged, this,
          &EditorController::changeTransitionPreset);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::muteToggled, this,
          &EditorController::setAudioTrackMuted);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::soloToggled, this,
          &EditorController::setAudioTrackSolo);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::gainEdited, this,
          &EditorController::setAudioTrackGain);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::panEdited, this,
          &EditorController::setAudioTrackPan);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::trackEffectAddRequested, this,
          &EditorController::addAudioTrackEffect);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::trackEffectRemoveRequested, this,
          &EditorController::removeAudioTrackEffect);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::trackEffectParameterEdited, this,
          &EditorController::updateAudioTrackEffectParameter);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::normalizationAnalyzeRequested, this,
          &EditorController::analyzeLoudnessNormalization);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::normalizationApplyRequested, this,
          &EditorController::applyLoudnessNormalization);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::outputDeviceSelected, this,
          &EditorController::selectAudioOutputDevice);
  connect(window_.audioMixer(), &desktop_ui::AudioMixerWidget::normalizationTargetChanged, this,
          &EditorController::setNormalizationTarget);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipActivated, this,
          [this](const QString& clipId) { setClipSelection({clipId}, clipId); });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipSelectionChanged, this,
          &EditorController::setClipSelection);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::clipBatchEditCommitted, this,
          [this](const QStringList& clipIds, const int destinationTrackIndex,
                 const qint64 startDelta, const qint64 durationDelta,
                 const desktop_ui::TimelineWidget::EditMode mode,
                 const desktop_ui::TimelineWidget::EditIntent intent,
                 const desktop_ui::TimelineSnapResult& snap) {
            Q_UNUSED(snap)
            commitTimelineBatchEdit(clipIds, destinationTrackIndex, startDelta, durationDelta,
                                    static_cast<int>(mode), static_cast<int>(intent));
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::frameNudgeRequested, this,
          [this](const QStringList& clipIds, const int frameCount,
                 const desktop_ui::TimelineWidget::EditIntent intent) {
            nudgeTimelineSelection(clipIds, frameCount, static_cast<int>(intent));
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerSelectionChanged, this,
          &EditorController::selectMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerAddRequested, this,
          &EditorController::addMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerMoveCommitted, this,
          [this](const QString& markerId, const qint64 start,
                 const desktop_ui::TimelineSnapResult& snap) {
            moveMarker(markerId, snap.snapped() ? snap.time : start);
          });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerRenameRequested, this,
          &EditorController::renameMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::markerRemoveRequested, this,
          &EditorController::removeMarker);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::gapSelectionChanged, this,
          &EditorController::selectGap);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::closeGapRequested, this,
          &EditorController::closeGap);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackAddRequested, this,
          [this](const desktop_ui::TrackKind kind) { addTrack(static_cast<int>(kind)); });
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackRenameRequested, this,
          &EditorController::renameTrack);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackReorderRequested, this,
          &EditorController::reorderTrack);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackLockToggled, this,
          &EditorController::setTrackLocked);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackVisibilityToggled, this,
          &EditorController::setTrackVisible);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackTargetToggled, this,
          &EditorController::setTrackTargeted);
  connect(window_.timeline(), &desktop_ui::TimelineWidget::trackRemoveRequested, this,
          &EditorController::removeTrack);
  connect(window_.mediaBin(), &desktop_ui::MediaBinWidget::proxyRequested, this,
          &EditorController::generateProxy);
  connect(window_.mediaBin(), &desktop_ui::MediaBinWidget::relinkRequested, this,
          &EditorController::relinkMedia);
  connect(&window_, &desktop_ui::EditorWindow::mediaSelectionChanged, this,
          &EditorController::selectMedia);
  connect(&window_, &desktop_ui::EditorWindow::assetMetadataEdited, this,
          &EditorController::saveAssetMetadata);
  connect(&window_, &desktop_ui::EditorWindow::manageMediaCacheRequested, this,
          &EditorController::showMediaCacheBrowser);
  if (auto* browser = window_.cacheBrowser(); browser != nullptr) {
    connect(browser, &desktop_ui::CacheBrowserDialog::budgetChanged, this,
            &EditorController::handleCacheBudgetChanged);
    connect(browser, &desktop_ui::CacheBrowserDialog::removeEntryRequested, this,
            &EditorController::removeCacheEntry);
    connect(browser, &desktop_ui::CacheBrowserDialog::removeAssetRequested, this,
            &EditorController::removeCacheAsset);
    connect(browser, &desktop_ui::CacheBrowserDialog::evictToBudgetRequested, this,
            &EditorController::evictCacheToBudget);
    connect(browser, &desktop_ui::CacheBrowserDialog::clearAllRequested, this,
            &EditorController::clearMediaCache);
  }

  playback_timer_.setTimerType(Qt::PreciseTimer);
  playback_timer_.setInterval(16);
  connect(&playback_timer_, &QTimer::timeout, this, &EditorController::advancePlayback);
  connect(&normalization_watcher_, &QFutureWatcher<NormalizationReview>::finished, this, [this] {
    if (!normalization_completion_gate_.complete(active_normalization_generation_)) {
      normalization_review_.valid = false;
      window_.audioMixer()->setNormalizationBusy(false);
      window_.audioMixer()->setNormalizationStatus(tr("Target changed; analyze again."));
      return;
    }
    normalization_review_ = normalization_watcher_.result();
    window_.audioMixer()->setNormalizationBusy(false);
    if (!normalization_review_.valid) {
      window_.audioMixer()->setNormalizationStatus(
          tr("Normalization failed: %1").arg(normalization_review_.error));
    } else {
      window_.audioMixer()->setNormalizationReview(normalization_review_.measured_lufs,
                                                   normalization_review_.gain_db,
                                                   normalization_review_.target_lufs);
    }
  });
  connect(&caption_analysis_watcher_, &QFutureWatcher<CaptionAnalysisOutcome>::finished, this,
          &EditorController::captionAnalysisFinished);
  connect(&model_verification_watcher_, &QFutureWatcher<ModelVerificationOutcome>::finished, this,
          &EditorController::modelVerificationFinished);
  connect(
      &audio_devices_watcher_, &QFutureWatcher<std::vector<audio::AudioDeviceInfo>>::finished, this,
      [this] {
        const auto devices = audio_devices_watcher_.result();
        const auto transition = evaluateAudioDevicePoll(known_audio_devices_, devices,
                                                        selected_audio_device_id_.toStdString());
        const bool selectedLost = transition.selected_missing;
        const bool defaultLost = transition.default_missing;
        if ((selectedLost || defaultLost) &&
            (audio_master_active_ || (audio_playback_ != nullptr && playback_rate_ != 0.0))) {
          stopAudioPlayback();
          audio_master_active_ = false;
          window_.audioMixer()->setMasterMeter(0.0F, 0.0F, 0.0, false);
          window_.showTransientMessage(
              selectedLost
                  ? tr("Selected audio device disconnected; playback paused until it returns.")
                  : tr("Default audio device disconnected; playback paused until a default "
                       "returns."),
              8'000);
        }
        known_audio_devices_ = devices;
        if ((transition.selected_recovered || transition.default_recovered) &&
            playback_rate_ > 0.0) {
          audio_recovery_pending_ = true;
        }
        if (audio_recovery_pending_ && playback_rate_ > 0.0 && !selectedLost && !defaultLost) {
          if (audio_playback_ != nullptr) {
            const auto diagnostics = audio_playback_->diagnostics();
            if (diagnostics.requested_state == audio::PlaybackState::Stopped &&
                diagnostics.playback.state == audio::PlaybackState::Stopped) {
              audio_playback_.reset();
            }
          }
          if (audio_playback_ == nullptr && startAudioMasterPlayback()) {
            audio_recovery_pending_ = false;
            playback_timer_.start();
            window_.showTransientMessage(tr("Audio device recovered; realtime playback resumed."));
          }
        }
        QStringList ids;
        QStringList names;
        for (const auto& device : devices) {
          if (device.connected) {
            ids.push_back(QString::fromStdString(device.id));
            names.push_back(QString::fromStdString(device.name));
          }
        }
        const bool backend = audio::MiniaudioOutputDevice::available();
        const auto presentation = audioMixerOutputStatus(backend, selectedLost);
        const bool available = presentation != AudioMixerOutputStatus::BackendMissing;
        QString status;
        switch (presentation) {
        case AudioMixerOutputStatus::BackendMissing:
          status = tr("Realtime audio backend was not built into this executable");
          break;
        case AudioMixerOutputStatus::SelectedUnavailable:
          status = tr("Selected device unavailable");
          break;
        case AudioMixerOutputStatus::Ready:
          status = tr("Ready");
          break;
        }
        window_.audioMixer()->setOutputDevices(ids, names, selected_audio_device_id_, available,
                                               status);
      });
  QSettings audioSettings;
  selected_audio_device_id_ =
      audioSettings.value(QStringLiteral("audio/outputDeviceId")).toString();
  normalization_target_lufs_ = std::clamp(
      audioSettings.value(QStringLiteral("audio/normalizationTargetLufs"), -14.0).toDouble(), -24.0,
      -9.0);
  window_.audioMixer()->setNormalizationTargetLufs(normalization_target_lufs_);
  audio_device_poll_timer_.setInterval(1'000);
  connect(&audio_device_poll_timer_, &QTimer::timeout, this,
          &EditorController::refreshAudioDevices);
  audio_device_poll_timer_.start();
  refreshAudioDevices();
  refreshTranscriptionState();
  newProject();
}

EditorController::~EditorController() {
  stopAudioPlayback();
  if (model_download_reply_ != nullptr) {
    model_download_reply_->abort();
  }
  if (model_verification_watcher_.isRunning())
    model_verification_stop_source_.request_stop();
  if (model_verification_watcher_.isRunning())
    model_verification_watcher_.waitForFinished();
  if (transcription_session_ != nullptr) {
    transcription_session_->cancel();
    transcription_session_->waitUntilFinished();
  }
  caption_analysis_stop_source_.request_stop();
  if (caption_analysis_future_.isRunning())
    caption_analysis_future_.waitForFinished();
  normalization_stop_source_.request_stop();
  if (normalization_future_.isRunning())
    normalization_future_.waitForFinished();
  if (model_download_file_ != nullptr) {
    model_download_file_->close();
    delete model_download_file_;
    model_download_file_ = nullptr;
  }
  if (!model_download_staging_.isEmpty()) {
    QDir(model_download_staging_).removeRecursively();
    model_download_staging_.clear();
  }
  if (export_session_ != nullptr) {
    export_cancel_requested_ = true;
    export_session_->cancel();
    export_session_->waitUntilFinished();
  }
  clearExportCheckpoint();
  waitForInFlightCacheJob(true);
  for (auto& [asset_id, job] : proxy_jobs_) {
    Q_UNUSED(asset_id)
    job.cancel_requested = true;
    if (job.session != nullptr) {
      job.session->cancel();
      job.session->waitUntilFinished();
    }
  }
  playback_timer_.stop();
  if (store_) {
    try {
      store_->mark_clean_close(store_->metadata().head_revision);
    } catch (...) {
      // Recovery state remains intentionally unclean when final metadata cannot be written.
    }
  }
}

std::int64_t EditorController::audioMasterSampleCounter() const noexcept {
  return audio_playback_ != nullptr ? audio_playback_->sample_counter() : playhead_;
}

std::uint64_t EditorController::audioXrunCount() const {
  return audio_playback_ != nullptr ? audio_playback_->diagnostics().playback.xrun_count : 0;
}

bool EditorController::audioControlPending() const {
  return audio_playback_ != nullptr &&
         audio_playback_->diagnostics().latest_status == audio::PlaybackCommandStatus::Pending;
}

edit::Project EditorController::makeDefaultProject() {
  edit::Project project;
  project.name = "Untitled Project";
  edit::Sequence sequence;
  sequence.name = "Timeline 1";
  sequence.frame_rate = edit::Rate(30'000, 1'001);
  sequence.width = 1'920;
  sequence.height = 1'080;
  sequence.audio_sample_rate = 48'000;
  for (int index = 1; index <= 2; ++index) {
    edit::Track track;
    track.kind = edit::TrackKind::Video;
    track.name = "V" + std::to_string(index);
    sequence.tracks.push_back(std::move(track));
  }
  for (int index = 1; index <= 4; ++index) {
    edit::Track track;
    track.kind = edit::TrackKind::Audio;
    track.name = "A" + std::to_string(index);
    sequence.tracks.push_back(std::move(track));
  }
  project.sequences.push_back(std::move(sequence));
  return project;
}

std::filesystem::path EditorController::recoveryDirectory() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath() + QStringLiteral("/VideoEditor");
  }
  const QString recovery = QDir(root).filePath(QStringLiteral("recovery"));
  QDir().mkpath(recovery);
  return pathFromQString(recovery);
}

std::filesystem::path EditorController::proxyCacheDirectory() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath() + QStringLiteral("/VideoEditor-cache");
  }
  const QString proxies = QDir(root).filePath(QStringLiteral("proxies"));
  QDir().mkpath(proxies);
  return pathFromQString(proxies);
}

std::filesystem::path EditorController::mediaCacheDirectory() {
  QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (root.isEmpty()) {
    root = QDir::tempPath() + QStringLiteral("/VideoEditor-cache");
  }
  const QString cache = QDir(root).filePath(QStringLiteral("media_cache"));
  QDir().mkpath(cache);
  return pathFromQString(cache);
}

std::filesystem::path EditorController::newWorkingPath(const edit::EntityId& projectId) const {
  return recoveryDirectory() / (projectId.toString() + ".working.sqlite");
}

bool EditorController::eventFilter(QObject* watched, QEvent* event) {
  if (watched == &window_ && event->type() == QEvent::Close && !closing_after_confirmation_) {
    if (!confirmDiscardChanges()) {
      static_cast<QCloseEvent*>(event)->ignore();
      return true;
    }
    closing_after_confirmation_ = true;
  }
  return QObject::eventFilter(watched, event);
}

bool EditorController::confirmDiscardChanges() {
  if (!dirty_) {
    return true;
  }
  const auto answer = QMessageBox::warning(
      &window_, tr("Unsaved changes"), tr("Save the current project before continuing?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (answer == QMessageBox::Cancel) {
    return false;
  }
  if (answer == QMessageBox::Save) {
    saveProject();
    return !dirty_;
  }
  return true;
}

void EditorController::newProject() {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("Cancel the current export before creating a new project"));
    return;
  }
  if (editor_ && !confirmDiscardChanges()) {
    return;
  }
  try {
    edit::Project project = makeDefaultProject();
    const auto working = newWorkingPath(project.id);
    std::error_code ignored;
    std::filesystem::remove(working, ignored);
    auto wal = working;
    wal += "-wal";
    auto shm = working;
    shm += "-shm";
    std::filesystem::remove(wal, ignored);
    std::filesystem::remove(shm, ignored);
    auto project_store = std::make_unique<store::ProjectStore>(
        working, store::OpenOptions{.project_uuid = project.id.toString()});
    installProject(std::move(project), working, std::move(project_store), std::nullopt);
    persistSnapshot("project.created");
    store_->mark_saved(store_->metadata().head_revision);
    setDirty(false);
    window_.showTransientMessage(tr("New project ready"));
  } catch (const std::exception& exception) {
    showError(tr("Could not create project"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::openProject() {
  const QString path = QFileDialog::getOpenFileName(&window_, tr("Open project"), {},
                                                    tr("Video Editor projects (*.veproj)"));
  if (!path.isEmpty()) {
    (void)openProjectFile(pathFromQString(path));
  }
}

bool EditorController::openProjectFile(const std::filesystem::path& checkpoint) {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("Cancel the current export before opening another project"));
    return false;
  }
  return confirmDiscardChanges() && loadCheckpoint(checkpoint);
}

bool EditorController::offerRecoveryOnStartup() {
  try {
    const store::RecoveryCatalog catalog = store::scan_recovery_directory(recoveryDirectory());
    const auto candidate = std::find_if(
        catalog.candidates.begin(), catalog.candidates.end(), [this](const auto& item) {
          return item.valid_project_database && item.recovery_recommended &&
                 item.working_database != working_path_;
        });
    if (candidate == catalog.candidates.end()) {
      return false;
    }

    const QString last_edit = QLocale().toString(
        QDateTime::fromMSecsSinceEpoch(candidate->heartbeat_utc_ms, QTimeZone::UTC).toLocalTime(),
        QLocale::ShortFormat);
    const QString details =
        candidate->head_revision != candidate->saved_revision
            ? tr("It contains committed edits newer than the last saved checkpoint.")
            : tr("It was not closed cleanly, so its last committed state is available.");
    const auto answer = QMessageBox::question(
        &window_, tr("Recover your last project?"),
        tr("Video Editor found a recoverable project from %1.\n\n%2\n\nOpen it now?")
            .arg(last_edit, details),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    return answer == QMessageBox::Yes && loadWorkingRecovery(candidate->working_database);
  } catch (const std::exception& exception) {
    window_.showTransientMessage(
        tr("Recovery scan could not be completed: %1").arg(QString::fromUtf8(exception.what())));
    return false;
  }
}

bool EditorController::loadWorkingRecovery(const std::filesystem::path& workingDatabase) {
  try {
    auto recovered_store = std::make_unique<store::ProjectStore>(
        workingDatabase, store::OpenOptions{.create_if_missing = false,
                                            .run_integrity_check = true,
                                            .busy_timeout_ms = 5'000,
                                            .project_uuid = std::nullopt});
    const auto commands = recovered_store->read_commands();
    const store::JournalEntry* snapshot_entry = latestProjectSnapshot(commands);
    if (snapshot_entry == nullptr) {
      throw std::runtime_error("Recovery database contains no readable project snapshot");
    }
    const auto bytes = binaryPayload(*snapshot_entry);
    auto decoded = project_codec::deserialize_project(std::span<const std::byte>(bytes));
    if (!decoded) {
      throw std::runtime_error(decoded.error().message);
    }
    if (decoded.value().id.toString() != recovered_store->metadata().project_uuid) {
      throw std::runtime_error("Recovery project identity does not match its database");
    }
    installProject(std::move(decoded).value(), workingDatabase, std::move(recovered_store),
                   std::nullopt);
    setDirty(true);
    window_.showTransientMessage(tr("Recovered the latest committed edit; save it to keep it"),
                                 8'000);
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not recover project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

bool EditorController::loadCheckpoint(const std::filesystem::path& checkpoint) {
  try {
    auto working_name = checkpoint.stem();
    working_name += "-" + edit::EntityId::generate().toString() + ".working.sqlite";
    const std::filesystem::path working = recoveryDirectory() / working_name;
    std::filesystem::copy_file(checkpoint, working,
                               std::filesystem::copy_options::overwrite_existing);
    auto opened_store = std::make_unique<store::ProjectStore>(working);
    const auto commands = opened_store->read_commands();
    const store::JournalEntry* snapshot_entry = latestProjectSnapshot(commands);
    if (snapshot_entry == nullptr) {
      throw std::runtime_error("Project contains no readable model snapshot");
    }
    const auto bytes = binaryPayload(*snapshot_entry);
    auto decoded = project_codec::deserialize_project(std::span<const std::byte>(bytes));
    if (!decoded) {
      throw std::runtime_error(decoded.error().message);
    }
    installProject(std::move(decoded).value(), working, std::move(opened_store), checkpoint);
    setDirty(media_paths_updated_on_install_);
    window_.showTransientMessage(tr("Project opened"));
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not open project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

void EditorController::installProject(edit::Project project, std::filesystem::path workingPath,
                                      std::unique_ptr<store::ProjectStore> projectStore,
                                      std::optional<std::filesystem::path> checkpoint) {
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  if (store_) {
    try {
      store_->mark_clean_close(store_->metadata().head_revision);
    } catch (...) {
      // Preserve the previous recovery database as unclean if finalization fails.
    }
  }
  editor_ = std::make_unique<edit::TimelineEditor>(std::move(project));
  store_ = std::move(projectStore);
  working_path_ = std::move(workingPath);
  checkpoint_path_ = std::move(checkpoint);
  visible_caption_indices_.clear();
  caption_search_.clear();
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_marker_id_.reset();
  selected_gap_key_.clear();
  timeline_time_scale_ = static_cast<std::uint32_t>(kUiTimescale);
  playhead_ = 0;
  media_paths_updated_on_install_ = reconstructMediaState();
  refreshViews();
}

void EditorController::saveProject() {
  if (!checkpoint_path_.has_value()) {
    saveProjectAs();
    return;
  }
  (void)saveTo(*checkpoint_path_);
}

void EditorController::saveProjectAs() {
  QString suggested;
  if (editor_) {
    suggested = QString::fromStdString(editor_->projectAt(editor_->revision())->name) +
                QStringLiteral(".veproj");
  }
  const QString path = QFileDialog::getSaveFileName(&window_, tr("Save project"), suggested,
                                                    tr("Video Editor projects (*.veproj)"));
  if (!path.isEmpty()) {
    (void)saveTo(pathFromQString(path));
  }
}

bool EditorController::saveTo(const std::filesystem::path& destination) {
  try {
    if (destination.extension() != ".veproj") {
      auto corrected = destination;
      corrected += ".veproj";
      checkpoint_path_ = corrected;
    } else {
      checkpoint_path_ = destination;
    }
    const auto revision = store_->metadata().head_revision;
    store_->checkpoint_to(*checkpoint_path_, revision);
    setDirty(false);
    window_.showTransientMessage(tr("Project saved"));
    return true;
  } catch (const std::exception& exception) {
    showError(tr("Could not save project"), QString::fromUtf8(exception.what()));
    return false;
  }
}

bool EditorController::saveProjectFile(const std::filesystem::path& destination) {
  return saveTo(destination);
}

void EditorController::chooseMedia() {
  const QStringList paths =
      QFileDialog::getOpenFileNames(&window_, tr("Import media"), {},
                                    tr("Media files (*.mp4 *.mov *.mkv *.webm *.avi *.mxf *.wav "
                                       "*.flac *.mp3 *.ogg);;All files (*)"));
  importPaths(paths);
}

void EditorController::chooseCaptionFile() {
  const QString path = QFileDialog::getOpenFileName(
      &window_, tr("Import captions"), {},
      tr("Caption files (*.srt *.vtt);;SubRip captions (*.srt);;WebVTT captions (*.vtt)"));
  if (!path.isEmpty()) {
    (void)importCaptionFile(pathFromQString(path));
  }
}

void EditorController::chooseCaptionExport() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || sequence->captions.empty()) {
    window_.showTransientMessage(tr("There are no sequence captions to export"));
    return;
  }
  const QString path =
      QFileDialog::getSaveFileName(&window_, tr("Export captions"), QStringLiteral("captions.srt"),
                                   tr("SubRip captions (*.srt);;WebVTT captions (*.vtt)"));
  if (!path.isEmpty()) {
    (void)exportCaptionFile(pathFromQString(path));
  }
}

bool EditorController::importCaptionFile(const std::filesystem::path& source) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    showError(tr("Could not import captions"), tr("The project has no active sequence."));
    return false;
  }
  QFile file(qStringFromPath(source));
  if (!file.open(QIODevice::ReadOnly)) {
    showError(tr("Could not import captions"), file.errorString());
    return false;
  }
  const QByteArray contents = file.readAll();
  const QString suffix = qStringFromPath(source.extension()).toLower();
  const auto format = suffix == QStringLiteral(".vtt") ? caption_service::SubtitleFormat::WebVtt
                                                       : caption_service::SubtitleFormat::Srt;
  auto parsed = caption_service::parse(
      std::string_view(contents.constData(), static_cast<std::size_t>(contents.size())), format);
  if (!parsed) {
    showError(tr("Could not import captions"), captionDiagnostics(parsed.error()));
    return false;
  }

  const auto captions = caption_service::toEditCaptions(parsed.value());
  if (captions.empty()) {
    showError(tr("Could not import captions"), tr("The caption file contains no cues."));
    return false;
  }
  const std::string gesture = "import-captions:" + edit::EntityId::generate().toString();
  std::vector<edit::EditCommand> commands;
  commands.reserve(captions.size());
  for (const edit::Caption& caption : captions) {
    commands.push_back(
        {.operation = edit::AddCaptionCommand{.sequence_id = sequence->id, .caption = caption},
         .coalescing_key = gesture});
  }
  if (!applyBatch(std::move(commands), tr("Could not import captions"))) {
    return false;
  }
  window_.showTransientMessage(tr("Imported %1 caption cue(s)").arg(captions.size()));
  return true;
}

bool EditorController::exportCaptionFile(const std::filesystem::path& destination) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || sequence->captions.empty()) {
    showError(tr("Could not export captions"), tr("The active sequence has no captions."));
    return false;
  }
  const QString suffix = qStringFromPath(destination.extension()).toLower();
  const auto format = suffix == QStringLiteral(".vtt") ? caption_service::SubtitleFormat::WebVtt
                                                       : caption_service::SubtitleFormat::Srt;
  const auto document = caption_service::fromEditCaptions(sequence->captions, format);
  const caption_service::SerializeOptions options{
      .timestamp_policy = caption_service::TimestampPolicy::NearestMillisecond,
      .emit_utf8_bom = false,
      .validation = {}};
  auto serialized = caption_service::serialize(document, format, options);
  if (!serialized) {
    showError(tr("Could not export captions"), captionDiagnostics(serialized.error()));
    return false;
  }

  QSaveFile output(qStringFromPath(destination));
  if (!output.open(QIODevice::WriteOnly)) {
    showError(tr("Could not export captions"), output.errorString());
    return false;
  }
  const std::string& text = serialized.value();
  if (output.write(text.data(), static_cast<qint64>(text.size())) !=
          static_cast<qint64>(text.size()) ||
      !output.commit()) {
    showError(tr("Could not export captions"), output.errorString());
    return false;
  }
  window_.showTransientMessage(tr("Captions exported"));
  return true;
}

void EditorController::chooseVideoExport(const QString& presetId) {
  if (export_in_flight_) {
    export_cancel_requested_ = true;
    if (export_session_ != nullptr) {
      export_session_->cancel();
    }
    window_.showTransientMessage(tr("Cancelling export…"));
    return;
  }
  bool numeric_preset = false;
  const auto parsed_preset = presetId.toInt(&numeric_preset);
  const bool legacy_prores = presetId == QStringLiteral("master.prores");
  const auto platform = numeric_preset && isKnownPlatformPreset(parsed_preset)
                            ? static_cast<export_service::PlatformPreset>(parsed_preset)
                            : (legacy_prores ? export_service::PlatformPreset::ReferenceProRes
                                             : export_service::PlatformPreset::ReferenceFfv1);
  const auto preset = export_service::reference_video_preset_for(platform).value_or(
      export_service::VideoPreset::Ffv1Matroska);
  const bool webm = preset == export_service::VideoPreset::Vp9OpusWebm;
  const QString extension =
      webm ? QStringLiteral("webm")
           : (preset == export_service::VideoPreset::Ffv1Matroska ? QStringLiteral("mkv")
                                                                  : QStringLiteral("mov"));
  const QString filter =
      webm ? tr("WebM video (*.webm)")
           : (preset == export_service::VideoPreset::Ffv1Matroska ? tr("Matroska video (*.mkv)")
                                                                  : tr("QuickTime movie (*.mov)"));
  const QString default_name = platform == export_service::PlatformPreset::PodcastAudioOnly
                                   ? QStringLiteral("podcast.webm")
                                   : QStringLiteral("export.%1").arg(extension);
  QString destination = window_.deliverPanel()->destinationPath();
  if (destination.isEmpty() ||
      !destination.endsWith(QStringLiteral(".%1").arg(extension), Qt::CaseInsensitive)) {
    destination =
        QFileDialog::getSaveFileName(&window_, tr("Export creator delivery"), default_name, filter);
  }
  if (!destination.isEmpty()) {
    window_.deliverPanel()->setDestinationPath(destination);
    (void)startVideoExport(pathFromQString(destination), presetId, true);
  }
}

bool EditorController::startVideoExport(const std::filesystem::path& destination,
                                        const QString& presetId, const bool overwriteExisting) {
  if (export_in_flight_) {
    window_.showTransientMessage(tr("An export is already running"));
    return false;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || edit::sequenceDuration(*sequence).isZero()) {
    showError(tr("Could not export"), tr("Add at least one clip to the timeline first."));
    return false;
  }

  bool numeric_preset = false;
  const auto parsed_preset = presetId.toInt(&numeric_preset);
  const bool legacy_prores = presetId == QStringLiteral("master.prores");
  if (numeric_preset && !isKnownPlatformPreset(parsed_preset)) {
    showError(tr("Could not export"), tr("The selected export preset is not recognized."));
    return false;
  }
  const auto platform = numeric_preset && isKnownPlatformPreset(parsed_preset)
                            ? static_cast<export_service::PlatformPreset>(parsed_preset)
                            : (legacy_prores ? export_service::PlatformPreset::ReferenceProRes
                                             : export_service::PlatformPreset::ReferenceFfv1);
  const auto preset = export_service::reference_video_preset_for(platform).value_or(
      export_service::VideoPreset::Ffv1Matroska);
  const export_service::PresetInfo preset_details = export_service::preset_info(preset);
  const bool audio_only = platform == export_service::PlatformPreset::PodcastAudioOnly;
  if (!audio_only && !preset_details.available) {
    showError(tr("Encoder unavailable"),
              tr("The selected %1 encoder is not available in this build.")
                  .arg(QString::fromStdString(preset_details.display_name)));
    return false;
  }

  const auto project = editor_->projectAt(editor_->revision());
  if (!project) {
    showError(tr("Could not export"), tr("The current project revision is unavailable."));
    return false;
  }
  project_codec::ProjectBytes checkpoint_bytes;
  try {
    checkpoint_bytes = project_codec::serialize_project(*project);
  } catch (const std::exception& exception) {
    showError(tr("Could not export"), QString::fromStdString(exception.what()));
    return false;
  }

  const auto checkpoint_path =
      pathFromQString(QDir::temp().filePath(QStringLiteral("video-editor-export-%1.veproj")
                                                .arg(QString::fromStdString(jobs::make_job_id()))));
  {
    QFile checkpoint_file(qStringFromPath(checkpoint_path));
    if (!checkpoint_file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        checkpoint_file.write(reinterpret_cast<const char*>(checkpoint_bytes.data()),
                              static_cast<qint64>(checkpoint_bytes.size())) !=
            static_cast<qint64>(checkpoint_bytes.size())) {
      showError(tr("Could not export"), tr("The export checkpoint could not be written."));
      return false;
    }
  }

  const auto panel = window_.deliverPanel();
  jobs::v1::ExportOptions options;
  options.set_schema_version(1);
  options.set_platform_preset(static_cast<std::int32_t>(platform));
  options.set_caption_mode(panel->captionModeKey().toStdString());
  options.set_sidecar_format(panel->sidecarFormatKey().toStdString());
  options.set_override_width(static_cast<std::uint32_t>(std::max(panel->overrideWidth(), 0)));
  options.set_override_height(static_cast<std::uint32_t>(std::max(panel->overrideHeight(), 0)));
  options.set_override_frame_rate_num(panel->overrideFrameRateNum());
  options.set_override_frame_rate_den(panel->overrideFrameRateDen());
  options.set_override_audio_bitrate(panel->overrideAudioBitrate());
  options.set_override_video_bitrate(panel->overrideVideoBitrate());
  if (const auto video_quality = panel->overrideVideoQuality(); video_quality.has_value()) {
    options.set_video_quality(*video_quality);
  }
  options.set_prefer_hardware(panel->preferHardwareEncoder());
  options.set_include_audio(true);
  options.set_overwrite_existing(overwriteExisting);
  options.set_sequence_id(sequence->id.toString());

  jobs::v1::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(jobs::v1::JOB_KIND_EXPORT);
  spec.set_project_revision(editor_->revision().value);
  spec.set_project_checkpoint(utf8StringFromPath(checkpoint_path));
  spec.set_output_uri(utf8StringFromPath(destination));
  spec.set_preset_id("video-editor.export.creator.v1");
  if (!options.SerializeToString(spec.mutable_options())) {
    std::error_code ignored;
    std::filesystem::remove(checkpoint_path, ignored);
    showError(tr("Could not export"), tr("The export options could not be encoded."));
    return false;
  }

  export_checkpoint_path_ = checkpoint_path;
  export_destination_ = destination;
  export_cancel_requested_ = false;
  export_in_flight_ = true;
  window_.deliverPanel()->setExportRunning(true, 0);
  window_.showTransientMessage(
      tr("Exporting full-quality video and 48 kHz audio from original media…"), 0);

  auto* session = new WorkerHostSession(this);
  export_session_ = session;
  session->setEventHandler([this](const jobs::v1::WorkerEvent& envelope) {
    const auto& job = envelope.event();
    if (job.state() == jobs::v1::JOB_STATE_ACCEPTED ||
        job.state() == jobs::v1::JOB_STATE_RUNNING) {
      if (job.state() == jobs::v1::JOB_STATE_RUNNING) {
        const int percent =
            std::clamp(static_cast<int>(std::lround(job.progress() * 100.0)), 0, 100);
        window_.deliverPanel()->setExportRunning(true, percent);
        const auto fallback = job.metadata().find("restarted_after_hardware_fallback");
        if (job.phase() == "hardware-fallback" ||
            (fallback != job.metadata().end() && fallback->second == "true")) {
          window_.showTransientMessage(tr("Hardware VP9 failed; restarting export in software…"),
                                       0);
        }
      }
      return;
    }
    if (job.state() == jobs::v1::JOB_STATE_SUCCEEDED) {
      const auto& metadata = job.metadata();
      const auto parse_u64 = [&metadata](const char* key) -> std::uint64_t {
        const auto found = metadata.find(key);
        if (found == metadata.end()) {
          return 0;
        }
        try {
          return static_cast<std::uint64_t>(std::stoull(found->second));
        } catch (...) {
          return 0;
        }
      };
      finishVideoExport(VideoExportOutcome{.succeeded = true,
                                           .cancelled = false,
                                           .frame_count = parse_u64("frame_count"),
                                           .audio_sample_count = parse_u64("audio_sample_count"),
                                           .error = {}});
      return;
    }
    if (job.state() == jobs::v1::JOB_STATE_CANCELLED || export_cancel_requested_) {
      finishVideoExport(VideoExportOutcome{.succeeded = false,
                                           .cancelled = true,
                                           .frame_count = 0,
                                           .audio_sample_count = 0,
                                           .error = {}});
      return;
    }
    finishVideoExport(VideoExportOutcome{
        .succeeded = false,
        .cancelled = false,
        .frame_count = 0,
        .audio_sample_count = 0,
        .error = job.has_error() ? QString::fromStdString(job.error().user_message())
                                 : tr("Export failed.")});
  });
  session->setStartFailedHandler([this](const QString& message) {
    finishVideoExport(VideoExportOutcome{.succeeded = false,
                                         .cancelled = false,
                                         .frame_count = 0,
                                         .audio_sample_count = 0,
                                         .error = message});
  });
  session->setFinishedHandler([this](const bool abnormal, int, QProcess::ExitStatus) {
    if (!export_in_flight_) {
      return;
    }
    finishVideoExport(VideoExportOutcome{
        .succeeded = false,
        .cancelled = export_cancel_requested_,
        .frame_count = 0,
        .audio_sample_count = 0,
        .error = abnormal ? tr("The export worker stopped unexpectedly.")
                          : tr("Export ended without a complete result.")});
  });
  WorkerHostSession::LaunchOptions launch;
  launch.application_directory = QCoreApplication::applicationDirPath();
  launch.configured_path = qEnvironmentVariable("VIDEO_EDITOR_WORKER_HOST");
  if (!session->start(spec, launch)) {
    export_session_ = nullptr;
    session->deleteLater();
    export_in_flight_ = false;
    window_.deliverPanel()->setExportRunning(false, 0);
    clearExportCheckpoint();
    showError(tr("Could not export"), tr("The export worker request could not be started."));
    return false;
  }
  return true;
}

void EditorController::importPaths(const QStringList& paths) {
  if (paths.isEmpty()) {
    return;
  }
  SessionEventLog::instance().log_backend(
      "importPaths", "count=" + std::to_string(static_cast<unsigned>(paths.size())));
  window_.showTransientMessage(tr("Importing %1 item(s)…").arg(paths.size()), 0);
  auto* watcher = new QFutureWatcher<std::vector<ImportOutcome>>(this);
  connect(watcher, &QFutureWatcher<std::vector<ImportOutcome>>::finished, this, [this, watcher] {
    const auto outcomes = watcher->result();
    watcher->deleteLater();
    int imported = 0;
    QStringList errors;
    for (const auto& outcome : outcomes) {
      if (outcome.asset.has_value()) {
        addImportedAsset(*outcome.asset);
        ++imported;
      } else {
        errors.push_back(QStringLiteral("%1: %2").arg(qStringFromPath(outcome.path.filename()),
                                                      QString::fromStdString(outcome.error)));
      }
    }
    refreshViews();
    window_.showTransientMessage(tr("Imported %1 item(s)").arg(imported));
    if (!errors.isEmpty()) {
      showError(tr("Some media could not be imported"), errors.join(QLatin1Char('\n')));
    }
  });

  std::vector<std::filesystem::path> media_paths;
  media_paths.reserve(static_cast<std::size_t>(paths.size()));
  for (const QString& path : paths) {
    media_paths.emplace_back(pathFromQString(path));
  }
  watcher->setFuture(QtConcurrent::run([media_paths = std::move(media_paths)] {
    assets::AssetService service;
    std::vector<ImportOutcome> outcomes;
    outcomes.reserve(media_paths.size());
    for (const auto& path : media_paths) {
      auto imported = service.import(path);
      if (imported) {
        outcomes.push_back({.path = path, .asset = std::move(imported).value(), .error = {}});
      } else {
        outcomes.push_back(
            {.path = path, .asset = std::nullopt, .error = imported.error().message});
      }
    }
    return outcomes;
  }));
}

void EditorController::addImportedAsset(assets::AssetRecord asset) {
  edit::Asset model_asset;
  const auto parsed_id = edit::EntityId::parse(asset.id);
  model_asset.id = parsed_id.value_or(edit::EntityId::generate());
  asset.id = model_asset.id.toString();
  model_asset.name = utf8StringFromPath(asset.uri.filename());
  model_asset.source_uri = utf8StringFromPath(asset.uri);
  model_asset.fingerprint = asset.fingerprint.quick_sha256;
  if (asset.descriptor.duration_microseconds.has_value() &&
      *asset.descriptor.duration_microseconds > 0) {
    model_asset.duration = edit::Time(*asset.descriptor.duration_microseconds, 1'000'000);
  } else {
    model_asset.duration = edit::Time(5, 1);
  }
  model_asset.metadata["container"] = asset.descriptor.format_name;
  for (const auto& stream : asset.descriptor.streams) {
    if (stream.video.has_value() && !model_asset.has_video) {
      model_asset.has_video = true;
      model_asset.width = static_cast<std::uint32_t>(std::max(stream.video->width, 0));
      model_asset.height = static_cast<std::uint32_t>(std::max(stream.video->height, 0));
      model_asset.metadata["video_codec"] = stream.codec_name;
      if (stream.video->average_frame_rate.numerator > 0 &&
          stream.video->average_frame_rate.denominator > 0) {
        model_asset.nominal_frame_rate =
            edit::Rate(static_cast<std::uint32_t>(stream.video->average_frame_rate.numerator),
                       static_cast<std::uint32_t>(stream.video->average_frame_rate.denominator));
      }
    }
    if (stream.audio.has_value() && !model_asset.has_audio) {
      model_asset.has_audio = true;
      model_asset.audio_sample_rate = static_cast<std::uint32_t>(stream.audio->sample_rate);
      model_asset.audio_channels = static_cast<std::uint32_t>(stream.audio->channels);
      model_asset.metadata["audio_codec"] = stream.codec_name;
    }
  }
  if (apply(edit::EditCommand{.operation = edit::AddAssetCommand{.asset = model_asset},
                              .coalescing_key = {}},
            tr("Could not add imported media"))) {
    playback::AssetPlaybackSources sources{
        .original = {.path = asset.uri, .video_stream_index = -1}, .proxy = std::nullopt};
    if (asset.proxy.has_value() && asset.proxy->complete) {
      sources.proxy =
          playback::AssetStreamLocation{.path = asset.proxy->proxy_uri, .video_stream_index = -1};
    }
    if (playback_registry_->register_asset(model_asset.id, std::move(sources))) {
      registered_playback_assets_.push_back(model_asset.id);
    }
    if (model_asset.has_audio) {
      if (audio_registry_->register_original(
              model_asset.id,
              audio_render::OriginalAudioMedia{.path = asset.uri, .audio_stream_index = -1})) {
        registered_audio_assets_.push_back(model_asset.id);
      }
    }
    imported_assets_.push_back(std::move(asset));
    enqueueMediaCacheJobs(imported_assets_.back());
    scheduleRecommendedProxies();
  }
}

void EditorController::generateProxy(const QString& assetId) {
  const std::string key = assetId.toStdString();
  if (const auto running = proxy_jobs_.find(key); running != proxy_jobs_.end()) {
    running->second.cancel_requested = true;
    if (running->second.session != nullptr) {
      running->second.session->cancel();
    }
    window_.showTransientMessage(tr("Cancelling proxy generation…"));
    return;
  }

  auto record =
      std::find_if(imported_assets_.begin(), imported_assets_.end(),
                   [&key](const assets::AssetRecord& candidate) { return candidate.id == key; });
  if (record == imported_assets_.end()) {
    window_.showTransientMessage(tr("Reimport this media before creating its proxy"));
    return;
  }
  if (record->proxy.has_value() && record->proxy->complete) {
    window_.showTransientMessage(tr("This media already has an editing proxy"));
    return;
  }

  const assets::ProxyProfile asset_profile = assets::AssetService::default_proxy_profile(*record);
  proxy::ProxyProfile profile{.video_codec = asset_profile.codec == assets::ProxyCodec::Ffv1
                                                 ? proxy::VideoCodec::Ffv1
                                                 : proxy::VideoCodec::ProResProxy,
                              .scale_numerator = 1,
                              .scale_denominator = 2,
                              .maximum_width = asset_profile.maximum_width,
                              .maximum_height = asset_profile.maximum_height,
                              .include_pcm_audio = asset_profile.include_pcm_audio,
                              .allow_ffv1_fallback = true};
  const auto resolved = proxy::resolve_profile(profile, proxy::encoder_availability());
  if (!resolved) {
    showError(tr("Could not create proxy"), QString::fromStdString(resolved.error().message));
    return;
  }

  const QString extension = resolved.value().container == proxy::Container::QuickTime
                                ? QStringLiteral(".mov")
                                : QStringLiteral(".mkv");
  const std::filesystem::path destination =
      proxyCacheDirectory() / pathFromQString(assetId + QStringLiteral(".proxy") + extension);
  const std::filesystem::path source = record->uri;
  const bool ffv1_preset = resolved.value().video_codec == proxy::VideoCodec::Ffv1;
  jobs::v1::JobSpec spec;
  spec.set_job_id(jobs::make_job_id());
  spec.set_kind(jobs::v1::JOB_KIND_PROXY);
  spec.add_input_uris(utf8StringFromPath(source));
  spec.set_output_uri(utf8StringFromPath(destination));
  spec.set_preset_id(ffv1_preset ? "video-editor.proxy.ffv1-half.v1"
                                 : "video-editor.proxy.prores-half.v1");

  auto* session = new WorkerHostSession(this);
  proxy_jobs_.emplace(key, ProxyJob{.session = session, .destination = destination});
  refreshMediaView();
  window_.showTransientMessage(tr("Creating a half-resolution editing proxy…"), 0);

  session->setEventHandler([this, key, destination](const jobs::v1::WorkerEvent& envelope) {
    const auto& job = envelope.event();
    if (job.state() == jobs::v1::JOB_STATE_ACCEPTED ||
        job.state() == jobs::v1::JOB_STATE_RUNNING) {
      return;
    }
    const bool cancelled =
        job.state() == jobs::v1::JOB_STATE_CANCELLED ||
        (proxy_jobs_.contains(key) && proxy_jobs_.at(key).cancel_requested);
    if (job.state() == jobs::v1::JOB_STATE_SUCCEEDED) {
      const auto& metadata = job.metadata();
      const auto codec = metadata.find("video_codec");
      finishProxyJob(key, ProxyOutcome{.asset_id = key,
                                       .destination = destination,
                                       .succeeded = true,
                                       .cancelled = false,
                                       .ffv1 = codec != metadata.end() && codec->second == "ffv1",
                                       .error = {}});
      return;
    }
    finishProxyJob(key, ProxyOutcome{
                            .asset_id = key,
                            .destination = destination,
                            .succeeded = false,
                            .cancelled = cancelled,
                            .ffv1 = false,
                            .error = job.has_error() ? QString::fromStdString(job.error().user_message())
                                                     : tr("Proxy generation failed.")});
  });
  session->setStartFailedHandler([this, key, destination](const QString& message) {
    finishProxyJob(key, ProxyOutcome{.asset_id = key,
                                     .destination = destination,
                                     .succeeded = false,
                                     .cancelled = false,
                                     .ffv1 = false,
                                     .error = message});
  });
  session->setFinishedHandler([this, key, destination](const bool abnormal, int, QProcess::ExitStatus) {
    if (!proxy_jobs_.contains(key)) {
      return;
    }
    const bool cancelled = proxy_jobs_.at(key).cancel_requested;
    finishProxyJob(key, ProxyOutcome{
                            .asset_id = key,
                            .destination = destination,
                            .succeeded = false,
                            .cancelled = cancelled,
                            .ffv1 = false,
                            .error = cancelled ? QString{}
                                               : (abnormal ? tr("The proxy worker stopped unexpectedly.")
                                                           : tr("Proxy generation ended without a complete result."))});
  });
  WorkerHostSession::LaunchOptions launch;
  launch.application_directory = QCoreApplication::applicationDirPath();
  launch.configured_path = qEnvironmentVariable("VIDEO_EDITOR_WORKER_HOST");
  if (!session->start(spec, launch)) {
    proxy_jobs_.erase(key);
    session->deleteLater();
    showError(tr("Could not create proxy"), tr("The proxy worker request could not be started."));
    refreshMediaView();
    pumpProxyQueue();
  }
}

void EditorController::finishProxyJob(const std::string& asset_id, const ProxyOutcome& outcome) {
  const auto running = proxy_jobs_.find(asset_id);
  if (running == proxy_jobs_.end()) {
    return;
  }
  if (running->second.session != nullptr) {
    running->second.session->deleteLater();
  }
  proxy_jobs_.erase(running);

  auto imported = std::find_if(imported_assets_.begin(), imported_assets_.end(),
                               [&outcome](const assets::AssetRecord& candidate) {
                                 return candidate.id == outcome.asset_id;
                               });
  if (outcome.succeeded && imported != imported_assets_.end()) {
    assets::ProxyProfile manifest_profile = assets::AssetService::default_proxy_profile(*imported);
    manifest_profile.codec =
        outcome.ffv1 ? assets::ProxyCodec::Ffv1 : assets::ProxyCodec::ProResProxy;
    imported->proxy = assets::ProxyManifest{.proxy_uri = outcome.destination,
                                            .profile = manifest_profile,
                                            .source_fingerprint = imported->fingerprint,
                                            .engine_version = "proxy-service-v1",
                                            .complete = true};
    std::filesystem::path playback_proxy = outcome.destination;
    if (media_cache_ != nullptr) {
      if (cache_job_future_.isRunning()) {
        cache_job_future_.waitForFinished();
      }
      const auto hash_profile = proxyProfileForHash(manifest_profile, outcome.ffv1);
      const media_cache::CacheKey proxy_key{.asset_id = outcome.asset_id,
                                            .kind = media_cache::CacheKind::Proxy,
                                            .parameter_hash = proxy::proxy_parameter_hash(hash_profile)};
      const auto stored = media_cache_->put_file(proxy_key, outcome.destination);
      if (stored) {
        if (auto path = media_cache_->path_for(proxy_key)) {
          imported->proxy->proxy_uri = path.value();
          playback_proxy = path.value();
        }
      } else if (stored.error().code == media_cache::CacheErrorCode::Full) {
        cache_disk_full_ = true;
        showError(tr("Media cache is full"), QString::fromStdString(stored.error().message));
      }
      const auto pts_source = proxy::default_pts_map_path(outcome.destination);
      std::error_code exists_error;
      if (std::filesystem::is_regular_file(pts_source, exists_error) && !exists_error) {
        const media_cache::CacheKey pts_key{
            .asset_id = outcome.asset_id,
            .kind = media_cache::CacheKind::ProxyPtsMap,
            .parameter_hash = proxy::proxy_parameter_hash(hash_profile)};
        (void)media_cache_->put_file(pts_key, pts_source);
      }
    }
    const auto model_id = edit::EntityId::parse(outcome.asset_id);
    if (model_id.has_value()) {
      playback::AssetPlaybackSources sources{
          .original = {.path = imported->uri, .video_stream_index = -1},
          .proxy = playback::AssetStreamLocation{.path = playback_proxy, .video_stream_index = -1}};
      (void)playback_registry_->register_asset(*model_id, std::move(sources));
      frame_provider_->invalidate(*model_id);
    }
    window_.showTransientMessage(outcome.ffv1 ? tr("Proxy ready (FFV1 compatibility profile)")
                                              : tr("Proxy ready"));
  } else if (outcome.cancelled) {
    window_.showTransientMessage(tr("Proxy generation cancelled; the cache was left intact"));
  } else if (!outcome.error.isEmpty()) {
    showError(tr("Proxy generation failed"), outcome.error);
  }
  refreshViews();
  pumpProxyQueue();
}

void EditorController::finishVideoExport(const VideoExportOutcome& outcome) {
  if (!export_in_flight_) {
    return;
  }
  export_in_flight_ = false;
  if (export_session_ != nullptr) {
    export_session_->deleteLater();
    export_session_ = nullptr;
  }
  const QString output_display = qStringFromPath(export_destination_);
  clearExportCheckpoint();
  window_.deliverPanel()->setExportRunning(false, 0);
  if (outcome.succeeded) {
    const QString message = tr("Export complete · %1 video frames · %2 audio samples")
                                .arg(outcome.frame_count)
                                .arg(outcome.audio_sample_count);
    window_.showTransientMessage(message);
    emit videoExportFinished(true, output_display, message);
  } else if (outcome.cancelled) {
    const QString message = tr("Export cancelled; the destination was left unchanged");
    window_.showTransientMessage(message);
    emit videoExportFinished(false, output_display, message);
  } else {
    showError(tr("Export failed"), outcome.error);
    emit videoExportFinished(false, output_display, outcome.error);
  }
}

void EditorController::clearExportCheckpoint() {
  if (export_checkpoint_path_.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::remove(export_checkpoint_path_, ignored);
  export_checkpoint_path_.clear();
}

void EditorController::insertAsset(const QString& assetId) {
  const edit::Asset* asset = assetByTextId(assetId);
  const edit::Sequence* sequence = currentSequence();
  if (asset == nullptr || sequence == nullptr) {
    return;
  }
  const edit::Asset asset_copy = *asset;
  const edit::EntityId sequence_id = sequence->id;
  std::optional<edit::EntityId> video_track_id;
  std::optional<edit::EntityId> audio_track_id;
  for (const edit::Track& track : sequence->tracks) {
    if (!track.locked && track.targeted && track.kind == edit::TrackKind::Video &&
        !video_track_id.has_value()) {
      video_track_id = track.id;
    }
    if (!track.locked && track.targeted && track.kind == edit::TrackKind::Audio &&
        !audio_track_id.has_value()) {
      audio_track_id = track.id;
    }
  }
  const edit::Time start = playheadTime();
  const edit::Time duration = asset_copy.duration.isZero() ? edit::Time(5, 1) : asset_copy.duration;
  const edit::EntityId linked = edit::EntityId::generate();
  std::optional<edit::EntityId> first_inserted;
  std::vector<edit::EditCommand> commands;

  const bool sequence_has_no_clips =
      std::all_of(sequence->tracks.begin(), sequence->tracks.end(),
                  [](const edit::Track& track) { return track.clips.empty(); });
  if (sequence_has_no_clips && asset_copy.has_video && asset_copy.width > 0 &&
      asset_copy.height > 0) {
    commands.push_back(
        {.operation =
             edit::SetSequenceFormatCommand{
                 .sequence_id = sequence_id,
                 .frame_rate = asset_copy.nominal_frame_rate.value_or(sequence->frame_rate),
                 .width = asset_copy.width,
                 .height = asset_copy.height},
         .coalescing_key = {}});
  }

  auto prepare_insert = [&](const std::optional<edit::EntityId> track_id,
                            const edit::ClipKind clip_kind) {
    if (!track_id.has_value()) {
      return false;
    }
    edit::Clip clip;
    clip.asset_id = asset_copy.id;
    clip.kind = clip_kind;
    clip.name = asset_copy.name;
    clip.timeline_range = {start, duration};
    clip.source_range = {edit::Time{}, duration};
    if (asset_copy.has_video && asset_copy.has_audio) {
      clip.linked_group = linked;
    }
    if (!first_inserted.has_value()) {
      first_inserted = clip.id;
    }
    commands.push_back(
        {.operation = edit::InsertClipCommand{.sequence_id = sequence_id,
                                              .track_id = *track_id,
                                              .clip = std::move(clip),
                                              .mode = edit::InsertMode::RejectOverlap},
         .coalescing_key = {}});
    return true;
  };

  if (asset_copy.has_video && !prepare_insert(video_track_id, edit::ClipKind::Video)) {
    window_.showTransientMessage(tr("No unlocked targeted video track is available"));
    return;
  }
  if (asset_copy.has_audio && !prepare_insert(audio_track_id, edit::ClipKind::Audio)) {
    window_.showTransientMessage(tr("No unlocked targeted audio track is available"));
    return;
  }
  if (applyBatch(std::move(commands), tr("Could not insert media at the playhead"))) {
    selected_clip_ids_.clear();
    if (first_inserted.has_value()) {
      selected_clip_ids_.insert(*first_inserted);
    }
    active_clip_id_ = first_inserted;
    selected_marker_id_.reset();
    selected_gap_key_.clear();
    refreshViews();
  }
}

void EditorController::splitSelectedClip() {
  const edit::Sequence* sequence = currentSequence();
  const auto selected = selectedClipIds();
  if (sequence == nullptr || selected.empty()) {
    window_.showTransientMessage(tr("Select a clip before splitting"));
    return;
  }
  std::unordered_set<edit::EntityId> consumed;
  std::vector<edit::EditCommand> commands;
  for (const auto& selected_id : selected) {
    if (consumed.contains(selected_id)) {
      continue;
    }
    const auto participants = expandLinkedSelection(*sequence, {selected_id});
    consumed.insert(participants.begin(), participants.end());
    edit::SplitClipCommand split{.sequence_id = sequence->id,
                                 .clip_id = selected_id,
                                 .split_time = playheadTime(),
                                 .right_clip_id = edit::EntityId::generate(),
                                 .include_linked = participants.size() > 1,
                                 .linked_right_clip_ids = {}};
    for (const auto& participant : participants) {
      if (participant != selected_id) {
        split.linked_right_clip_ids.push_back(
            {.clip_id = participant, .right_clip_id = edit::EntityId::generate()});
      }
    }
    commands.push_back({.operation = std::move(split), .coalescing_key = {}});
  }
  (void)applyBatch(std::move(commands),
                   tr("Could not split the selected clips and their linked media"));
}

void EditorController::deleteSelectedClip(const bool ripple) {
  const edit::Sequence* sequence = currentSequence();
  const auto selected = selectedClipIds();
  if (sequence == nullptr || selected.empty()) {
    return;
  }
  std::vector<edit::EditCommand> commands;
  std::unordered_set<edit::EntityId> consumed;
  for (const auto& clipId : selected) {
    if (consumed.contains(clipId)) {
      continue;
    }
    const auto participants = expandLinkedSelection(*sequence, {clipId});
    consumed.insert(participants.begin(), participants.end());
    commands.push_back(
        {.operation = edit::RemoveClipCommand{.sequence_id = sequence->id,
                                              .clip_id = clipId,
                                              .ripple = ripple,
                                              .include_linked = participants.size() > 1},
         .coalescing_key = {}});
  }
  if (applyBatch(std::move(commands), ripple ? tr("Could not ripple-delete the selection")
                                             : tr("Could not delete the selection"))) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
  }
}

void EditorController::commitTimelineEdit(const QString& clipId, const int destinationTrackIndex,
                                          const qint64 startDelta, const qint64 durationDelta,
                                          const int editMode, const int editIntent) {
  commitTimelineBatchEdit({clipId}, destinationTrackIndex, startDelta, durationDelta, editMode,
                          editIntent);
}

void EditorController::commitTimelineBatchEdit(const QStringList& clipIds,
                                               const int destinationTrackIndex,
                                               const qint64 startDelta, const qint64 durationDelta,
                                               const int editMode, const int editIntent) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || destinationTrackIndex < 0 ||
      destinationTrackIndex >= static_cast<int>(sequence->tracks.size())) {
    window_.showTransientMessage(tr("The timeline edit target is no longer available"));
    refreshTimelineView();
    return;
  }
  if (!clipIds.isEmpty()) {
    const QString prior_active = active_clip_id_.has_value()
                                     ? QString::fromStdString(active_clip_id_->toString())
                                     : QString{};
    const QString active =
        !prior_active.isEmpty() && clipIds.contains(prior_active) ? prior_active : clipIds.front();
    setClipSelection(clipIds, active);
  }
  const auto selection = selectedClipIds();
  if (selection.empty() || !active_clip_id_.has_value()) {
    window_.showTransientMessage(tr("Select one or more clips before editing"));
    refreshTimelineView();
    return;
  }

  try {
    const auto mode = static_cast<desktop_ui::TimelineWidget::EditMode>(editMode);
    const auto intent = static_cast<desktop_ui::TimelineWidget::EditIntent>(editIntent);
    const edit::InsertMode insert_mode =
        intent == desktop_ui::TimelineWidget::EditIntent::Ripple ? edit::InsertMode::Ripple
        : intent == desktop_ui::TimelineWidget::EditIntent::Overwrite
            ? edit::InsertMode::Overwrite
            : edit::InsertMode::RejectOverlap;
    const edit::Time start_delta = timelineTime(startDelta);
    const edit::Time duration_delta = timelineTime(durationDelta);
    const auto trackForClip = [sequence](const edit::EntityId& id) -> const edit::Track* {
      for (const auto& track : sequence->tracks) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [&id](const auto& clip) { return clip.id == id; })) {
          return &track;
        }
      }
      return nullptr;
    };
    const auto sourceRangeForTimelineRange = [](const edit::Clip& clip,
                                                const edit::TimeRange& timelineRange) {
      const edit::Time head_delta = timelineRange.start - clip.timeline_range.start;
      const edit::Time tail_delta = timelineRange.end() - clip.timeline_range.end();
      const auto source_delta = [&clip](const edit::Time delta) {
        return delta
            .scaled(clip.playback_rate.numerator(), clip.playback_rate.denominator(),
                    edit::RoundingMode::NearestTiesEven)
            .rescaledTo(clip.source_range.duration.timescale(),
                        edit::RoundingMode::NearestTiesEven);
      };
      edit::Time source_start = clip.source_range.start;
      edit::Time source_end = clip.source_range.end();
      if (!clip.reversed) {
        source_start = source_start + source_delta(head_delta);
        source_end = source_end + source_delta(tail_delta);
      } else {
        source_start = source_start - source_delta(tail_delta);
        source_end = source_end - source_delta(head_delta);
      }
      return edit::TimeRange(source_start, source_end - source_start);
    };

    std::vector<edit::EditCommand> commands;
    std::unordered_set<edit::EntityId> consumed;
    for (const auto& id : selection) {
      if (consumed.contains(id)) {
        continue;
      }
      const edit::Clip* clip = edit::findClip(*sequence, id);
      const edit::Track* source_track = trackForClip(id);
      if (clip == nullptr || source_track == nullptr) {
        window_.showTransientMessage(tr("A selected clip no longer exists"));
        refreshTimelineView();
        return;
      }
      const auto linked = expandLinkedSelection(*sequence, {id});
      consumed.insert(linked.begin(), linked.end());
      const bool include_linked = linked.size() > 1;
      if (mode == desktop_ui::TimelineWidget::EditMode::Move) {
        const edit::Track& destination =
            id == *active_clip_id_
                ? sequence->tracks[static_cast<std::size_t>(destinationTrackIndex)]
                : *source_track;
        commands.push_back(
            {.operation =
                 edit::MoveClipCommand{.sequence_id = sequence->id,
                                       .clip_id = clip->id,
                                       .destination_track_id = destination.id,
                                       .new_start = clip->timeline_range.start + start_delta,
                                       .mode = insert_mode,
                                       .include_linked = include_linked},
             .coalescing_key = {}});
      } else if (mode == desktop_ui::TimelineWidget::EditMode::TrimIn ||
                 mode == desktop_ui::TimelineWidget::EditMode::TrimOut) {
        const edit::TimeRange timeline_range(clip->timeline_range.start + start_delta,
                                             clip->timeline_range.duration + duration_delta);
        commands.push_back(
            {.operation = edit::TrimClipCommand{.sequence_id = sequence->id,
                                                .clip_id = clip->id,
                                                .timeline_range = timeline_range,
                                                .source_range = sourceRangeForTimelineRange(
                                                    *clip, timeline_range),
                                                .include_linked = include_linked,
                                                .mode = insert_mode},
             .coalescing_key = {}});
      }
    }

    const edit::Clip* active = edit::findClip(*sequence, *active_clip_id_);
    const edit::Track* active_track = trackForClip(*active_clip_id_);
    if (active == nullptr || active_track == nullptr) {
      throw std::invalid_argument("the active clip is no longer available");
    }
    if (mode == desktop_ui::TimelineWidget::EditMode::Roll) {
      const auto active_index = static_cast<std::size_t>(std::distance(
          active_track->clips.begin(),
          std::find_if(active_track->clips.begin(), active_track->clips.end(),
                       [active](const auto& clip) { return clip.id == active->id; })));
      const edit::Clip* left = nullptr;
      const edit::Clip* right = nullptr;
      edit::Time cut{};
      const bool incoming_edge = start_delta != edit::Time{} && duration_delta == -start_delta;
      const bool has_preceding =
          active_index > 0 && active_track->clips[active_index - 1].timeline_range.end() ==
                                  active->timeline_range.start;
      const bool has_following = active_index + 1 < active_track->clips.size() &&
                                 active->timeline_range.end() ==
                                     active_track->clips[active_index + 1].timeline_range.start;
      // The widget represents an incoming roll as an inverse start/duration
      // pair. Outgoing rolls use the shared-cut delta directly. Prefer the
      // matching edge, then retain the only available adjacent cut.
      if (incoming_edge && has_preceding) {
        left = &active_track->clips[active_index - 1];
        right = active;
        cut = active->timeline_range.start;
      } else if (!incoming_edge && has_following) {
        left = active;
        right = &active_track->clips[active_index + 1];
        cut = active->timeline_range.end();
      } else if (has_preceding) {
        left = &active_track->clips[active_index - 1];
        right = active;
        cut = active->timeline_range.start;
      } else if (has_following) {
        left = active;
        right = &active_track->clips[active_index + 1];
        cut = active->timeline_range.end();
      } else {
        throw std::invalid_argument("roll edit requires adjacent clips");
      }
      commands.clear();
      commands.push_back({.operation = edit::RollEditCommand{.sequence_id = sequence->id,
                                                             .left_clip_id = left->id,
                                                             .right_clip_id = right->id,
                                                             .new_cut_time = cut + start_delta},
                          .coalescing_key = {}});
    } else if (mode == desktop_ui::TimelineWidget::EditMode::Slip) {
      commands.clear();
      const edit::Time source_delta =
          start_delta
              .scaled(active->playback_rate.numerator(), active->playback_rate.denominator(),
                      edit::RoundingMode::NearestTiesEven)
              .rescaledTo(active->source_range.start.timescale(),
                          edit::RoundingMode::NearestTiesEven);
      commands.push_back(
          {.operation =
               edit::SlipClipCommand{.sequence_id = sequence->id,
                                     .clip_id = active->id,
                                     .new_source_start = active->source_range.start + source_delta,
                                     .include_linked =
                                         expandLinkedSelection(*sequence, {active->id}).size() > 1},
           .coalescing_key = {}});
    } else if (mode == desktop_ui::TimelineWidget::EditMode::Slide) {
      commands.clear();
      commands.push_back(
          {.operation =
               edit::SlideClipCommand{.sequence_id = sequence->id,
                                      .clip_id = active->id,
                                      .new_start = active->timeline_range.start + start_delta},
           .coalescing_key = {}});
    }
    (void)applyBatch(std::move(commands), tr("Could not apply the timeline edit"));
  } catch (const std::exception& exception) {
    window_.showTransientMessage(
        tr("Could not apply the timeline edit: %1").arg(QString::fromUtf8(exception.what())));
    refreshTimelineView();
  }
}

void EditorController::undo() {
  const auto result = editor_->undo(editor_->revision());
  if (!result) {
    window_.showTransientMessage(QString::fromStdString(result.error().message));
    return;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("history.undo");
    setDirty(true);
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    selected_marker_id_.reset();
    selected_gap_key_.clear();
    refreshViews();
  } catch (const std::exception& exception) {
    showError(tr("Could not persist undo"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::redo() {
  const auto result = editor_->redo(editor_->revision());
  if (!result) {
    window_.showTransientMessage(QString::fromStdString(result.error().message));
    return;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("history.redo");
    setDirty(true);
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    selected_marker_id_.reset();
    selected_gap_key_.clear();
    refreshViews();
  } catch (const std::exception& exception) {
    showError(tr("Could not persist redo"), QString::fromUtf8(exception.what()));
  }
}

void EditorController::seek(const qint64 position) {
  playhead_ = toUiTime(timelineTime(std::max<qint64>(position, 0)));
  if (audio_playback_ != nullptr && !audio_session_stale_ &&
      audio_playback_->requested_state() != audio::PlaybackState::Stopped) {
    const audio::PlaybackCommandReceipt receipt = audio_playback_->request_seek(playhead_);
    if (receipt.accepted) {
      audio_control_intent_ = AudioControlIntent::Seek;
      audio_command_version_ = receipt.version;
      audio_master_active_ = false;
      playback_timer_.start();
    } else {
      const QString failure = receipt.error.has_value()
                                  ? QString::fromStdString(receipt.error->message)
                                  : tr("the audio control queue rejected the seek");
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Audio device seek failed; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
    }
  }
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
  requestPreview();
}

void EditorController::setPlaybackRate(const double rate) {
  SessionEventLog::instance().log_backend("setPlaybackRate",
                                           "rate=" + QString::number(rate, 'g', 6).toStdString());
  playback_rate_ = rate;
  if (std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
    audio_recovery_pending_ = false;
    audio_start_pending_ = false;
    if (audio_playback_ != nullptr && !audio_session_stale_ &&
        audio_playback_->requested_state() != audio::PlaybackState::Stopped) {
      const audio::PlaybackCommandReceipt receipt = audio_playback_->request_pause();
      if (receipt.accepted) {
        audio_control_intent_ = AudioControlIntent::Pause;
        audio_command_version_ = receipt.version;
        audio_master_active_ = false;
        playback_timer_.start();
        return;
      }
      stopAudioPlayback();
    }
    audio_master_active_ = false;
    playback_timer_.stop();
    return;
  }

  if (std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon()) {
    if (audio_playback_ != nullptr && !audio_session_stale_) {
      const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
      if (diagnostics.requested_state == audio::PlaybackState::Paused ||
          diagnostics.effective_state == audio::PlaybackState::Paused) {
        const audio::PlaybackCommandReceipt receipt = audio_playback_->request_resume();
        if (receipt.accepted) {
          audio_control_intent_ = AudioControlIntent::Resume;
          audio_command_version_ = receipt.version;
          audio_master_active_ = false;
          playback_timer_.start();
          return;
        }
        stopAudioPlayback();
      } else if (diagnostics.effective_state == audio::PlaybackState::Playing &&
                 diagnostics.playback.device_running) {
        audio_master_active_ = true;
        playback_timer_.start();
        return;
      }
    }
    if (startAudioMasterPlayback()) {
      playback_timer_.start();
      return;
    }
  } else {
    if (audio_playback_ != nullptr) {
      playhead_ = std::max<qint64>(audio_playback_->sample_counter(), 0);
      window_.timeline()->setPlayhead(timelineValue(playheadTime()));
      requestPreview();
    }
    stopAudioPlayback();
    if (!shuttle_silence_announced_) {
      shuttle_silence_announced_ = true;
      window_.showTransientMessage(tr("Shuttle playback outside 1× is silent"));
    }
  }
  playback_clock_.restart();
  playback_timer_.start();
}

void EditorController::advancePlayback() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    stopAudioPlayback();
    playback_timer_.stop();
    return;
  }
  const qint64 end = std::max<qint64>(toUiTime(edit::sequenceDuration(*sequence)), 0);
  bool audio_clock_applied = false;

  if (audio_start_pending_) {
    bool ready_to_replace = audio_playback_ == nullptr;
    if (audio_playback_ != nullptr) {
      const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
      ready_to_replace = diagnostics.requested_state == audio::PlaybackState::Stopped &&
                         diagnostics.latest_status != audio::PlaybackCommandStatus::Pending &&
                         diagnostics.playback.state == audio::PlaybackState::Stopped;
    }
    if (ready_to_replace) {
      audio_playback_.reset();
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      audio_start_pending_ = false;
      if (std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon() &&
          startAudioMasterPlayback()) {
        return;
      }
      playback_clock_.restart();
    } else {
      return;
    }
  }

  if (audio_playback_ != nullptr && audio_control_intent_ != AudioControlIntent::None) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const bool matching_result = diagnostics.latest_result_version == audio_command_version_;
    const bool pending =
        matching_result && diagnostics.latest_status == audio::PlaybackCommandStatus::Pending;
    if (pending) {
      if (audio_control_intent_ == AudioControlIntent::Pause) {
        playhead_ = std::clamp<qint64>(diagnostics.playback.sample_counter, 0, end);
        window_.timeline()->setPlayhead(timelineValue(playheadTime()));
        requestPreview();
      }
      return;
    }

    if (matching_result && diagnostics.latest_status == audio::PlaybackCommandStatus::Failed) {
      const QString failure = diagnostics.latest_error.has_value()
                                  ? QString::fromStdString(diagnostics.latest_error->message)
                                  : tr("the audio control operation failed");
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Realtime audio stopped; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
      if (std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
        playback_timer_.stop();
        return;
      }
    } else if (matching_result) {
      const AudioControlIntent completed_intent = audio_control_intent_;
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      playhead_ = std::clamp<qint64>(diagnostics.playback.sample_counter, 0, end);
      if (diagnostics.effective_state == audio::PlaybackState::Playing &&
          diagnostics.playback.device_running &&
          std::abs(playback_rate_ - 1.0) < std::numeric_limits<double>::epsilon()) {
        audio_master_active_ = true;
        audio_clock_applied = true;
        if (!audio_status_announced_) {
          audio_status_announced_ = true;
          window_.showTransientMessage(
              tr("Realtime 48 kHz audio is the latency-compensated playback master clock"));
        }
      } else {
        audio_master_active_ = false;
        if (completed_intent == AudioControlIntent::Pause ||
            std::abs(playback_rate_) < std::numeric_limits<double>::epsilon()) {
          window_.timeline()->setPlayhead(timelineValue(playheadTime()));
          requestPreview();
          playback_timer_.stop();
          return;
        }
      }
    }
  }

  if (audio_master_active_ && audio_playback_ != nullptr) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const audio::PlaybackDiagnostics& playback = diagnostics.playback;
    if (playback.state == audio::PlaybackState::Failed || !playback.device_running) {
      const QString failure = playback.last_error.empty()
                                  ? tr("the audio device stopped")
                                  : QString::fromStdString(playback.last_error);
      playhead_ = std::clamp<qint64>(playback.sample_counter, 0, end);
      stopAudioPlayback();
      playback_clock_.restart();
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Realtime audio stopped; continuing with silent timer playback: %1").arg(failure),
            8'000);
      }
    } else {
      playhead_ = std::clamp<qint64>(playback.sample_counter, 0, end);
      audio_clock_applied = true;
      if (playback.xrun_count > last_audio_xrun_count_) {
        if (last_audio_xrun_count_ == 0) {
          window_.showTransientMessage(
              tr("Audio playback underrun detected; consider proxies or a larger audio buffer"),
              8'000);
        }
        last_audio_xrun_count_ = playback.xrun_count;
      }
      // Poll the playback meter and push per-channel peak levels to the
      // mixer. The meter is callback-safe and resets on read.
      if (audio_playback_ != nullptr) {
        const audio::PlaybackMeter::Reading meter = audio_playback_->read_meter();
        QVector<float> peak_dbfs;
        peak_dbfs.reserve(2);
        for (std::size_t c = 0; c < 2U; ++c) {
          const float linear = std::max(c == 0 ? meter.peak[c] : meter.peak[c], 1.0e-12F);
          peak_dbfs.push_back(20.0F * std::log10(linear));
        }
        const float peak = std::max(meter.peak[0], meter.peak[1]);
        const float rms = std::max(meter.rms[0], meter.rms[1]);
        const auto loudness = audio_playback_->read_loudness();
        const auto toDb = [](const float value) {
          return 20.0F * std::log10(std::max(value, 1.0e-12F));
        };
        window_.audioMixer()->setMasterMeter(toDb(peak), toDb(rms), loudness.integrated_lufs,
                                             meter.sample_count != 0U, loudness.integrated_valid,
                                             loudness.stale);
        if (playback_audio_renderer_ != nullptr) {
          // The renderer may be several blocks ahead in the pre-render ring.
          // Select telemetry using the latency-compensated device master clock,
          // never the most recently completed future block.
          const auto track_meters =
              playback_audio_renderer_->trackMetersAt(playback.sample_counter);
          QVector<desktop_ui::AudioTrackMeterView> meter_views;
          meter_views.reserve(static_cast<qsizetype>(track_meters.tracks.size()));
          for (const auto& track_meter : track_meters.tracks) {
            desktop_ui::AudioTrackMeterView view;
            view.id = QString::fromStdString(track_meter.track_id.toString());
            view.active = track_meter.active;
            view.stale = track_meters.stale;
            for (std::size_t channel = 0; channel < 2U; ++channel) {
              view.peak_dbfs[channel] = toDb(track_meter.peak[channel]);
              view.rms_dbfs[channel] = toDb(track_meter.rms[channel]);
            }
            meter_views.push_back(std::move(view));
          }
          window_.audioMixer()->setTrackMeters(meter_views);
        }
      }
    }
  }

  if (!audio_clock_applied) {
    const qint64 elapsed_ms = playback_clock_.restart();
    const auto delta =
        static_cast<qint64>(std::llround(playback_rate_ * static_cast<double>(kUiTimescale) *
                                         static_cast<double>(elapsed_ms) / 1000.0));
    playhead_ = std::clamp(playhead_ + delta, qint64{0}, end);
  }
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
  requestPreview(PreviewRequestPolicy::Coalesce);
  if ((playback_rate_ > 0.0 && playhead_ >= end) || (playback_rate_ < 0.0 && playhead_ <= 0)) {
    stopAudioPlayback();
    playback_timer_.stop();
    playback_rate_ = 0.0;
  }
}

void EditorController::seekCaption(const int visibleRow) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index < sequence->captions.size()) {
    seek(timelineValue(sequence->captions[caption_index].range.start));
  }
}

void EditorController::addCaptionAtPlayhead() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  edit::Caption caption;
  caption.range = edit::TimeRange(playheadTime(), edit::Time(2, 1));
  caption.text = "New caption";
  if (apply(edit::EditCommand{.operation = edit::AddCaptionCommand{.sequence_id = sequence->id,
                                                                   .caption = std::move(caption)},
                              .coalescing_key = {}},
            tr("Could not add caption"))) {
    refreshCaptionView();
  }
}

void EditorController::removeCaption(const int visibleRow) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index >= sequence->captions.size()) {
    return;
  }
  (void)apply(edit::EditCommand{.operation =
                                    edit::RemoveCaptionCommand{
                                        .sequence_id = sequence->id,
                                        .caption_id = sequence->captions[caption_index].id},
                                .coalescing_key = {}},
              tr("Could not delete caption"));
}

void EditorController::updateCaptionText(const int visibleRow, const QString& text) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || visibleRow < 0 ||
      static_cast<std::size_t>(visibleRow) >= visible_caption_indices_.size()) {
    refreshCaptionView();
    return;
  }
  const std::size_t caption_index =
      visible_caption_indices_.at(static_cast<std::size_t>(visibleRow));
  if (caption_index >= sequence->captions.size()) {
    refreshCaptionView();
    return;
  }
  const QString normalized = text.trimmed();
  if (normalized.isEmpty()) {
    window_.showTransientMessage(tr("Caption text cannot be empty"));
    refreshCaptionView();
    return;
  }
  edit::Caption caption = sequence->captions[caption_index];
  if (caption.text == normalized.toStdString()) {
    return;
  }
  caption.text = normalized.toStdString();
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateCaptionCommand{.sequence_id = sequence->id,
                                                                .caption = std::move(caption)},
                        .coalescing_key = {}},
      tr("Could not update caption"));
}

void EditorController::searchTranscript(const QString& query) {
  caption_search_ = query.trimmed();
  refreshCaptionView();
}

bool EditorController::selectedAudioInput(std::filesystem::path& path, edit::TimeRange& range,
                                          edit::EntityId& clipId) const {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return false;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr ||
      (clip->kind != edit::ClipKind::Audio && clip->kind != edit::ClipKind::Video)) {
    return false;
  }
  const edit::Asset* asset =
      edit::findAsset(*editor_->projectAt(editor_->revision()), clip->asset_id);
  if (asset == nullptr || !asset->has_audio || asset->source_uri.empty()) {
    return false;
  }
  path = pathFromUtf8String(asset->source_uri);
  range = clip->timeline_range;
  clipId = clip->id;
  return true;
}

void EditorController::refreshTranscriptionState() {
  const auto cache = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                     QStringLiteral("/models");
  QPointer<EditorController> guard(this);
  auto* watcher = new QFutureWatcher<bool>(this);
  connect(watcher, &QFutureWatcher<bool>::finished, this, [guard, watcher] {
    const bool ready = watcher->result();
    watcher->deleteLater();
    if (guard && guard->transcription_session_ == nullptr &&
        guard->model_download_reply_ == nullptr &&
        !guard->model_verification_watcher_.isRunning()) {
      guard->window_.captionsPanel()->setTranscriptionState(
          ready ? desktop_ui::TranscriptionState::Ready
                : desktop_ui::TranscriptionState::ModelMissing,
          ready ? QObject::tr("Model ready; transcription is on demand.")
                : QObject::tr("Model not installed; download is optional."));
    }
  });
  watcher->setFuture(QtConcurrent::run([cache] {
    auto fetcher = transcription::make_unavailable_model_fetcher();
    transcription::ModelManager manager(pathFromQString(cache), *fetcher);
    return manager.verify();
  }));
}

void EditorController::downloadTranscriptionModel(const QString& modelId) {
  if (model_download_reply_ != nullptr || transcription_session_ != nullptr ||
      model_verification_watcher_.isRunning()) {
    window_.showTransientMessage(tr("A transcription operation is already running"));
    return;
  }
  if (modelId != QString::fromUtf8(transcription::kWhisperModelId.data())) {
    window_.showTransientMessage(tr("That transcription model is not available"));
    return;
  }
  const QString cache = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                            .filePath(QStringLiteral("models"));
  QDir().mkpath(cache);
  const QString staging =
      QDir(cache).filePath(QStringLiteral(".staging-%1")
                               .arg(QString::fromStdString(edit::EntityId::generate().toString())));
  model_download_staging_ = staging;
  if (!QDir().mkpath(staging)) {
    model_download_staging_.clear();
    window_.showTransientMessage(tr("Could not create the model staging directory"));
    return;
  }
  const QString stagedPath =
      QDir(staging).filePath(QString::fromUtf8(transcription::kWhisperModelFilename.data()));
  model_download_file_ = new QFile(stagedPath, this);
  model_download_write_failed_ = false;
  model_download_bytes_written_ = 0;
  model_download_size_rejected_ = false;
  model_download_user_cancelled_ = false;
  if (!model_download_file_->open(QIODevice::WriteOnly)) {
    delete model_download_file_;
    model_download_file_ = nullptr;
    QDir(staging).removeRecursively();
    model_download_staging_.clear();
    window_.showTransientMessage(tr("Could not open the model staging file"));
    return;
  }
  const QUrl url(QString::fromUtf8(transcription::kWhisperModelUrl.data()));
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  model_download_reply_ = transcription_network_->get(request);
  model_download_reply_->setReadBufferSize(kModelNetworkReadBufferBytes);
  window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Downloading,
                                                 tr("Downloading model…"), 0);
  connect(model_download_reply_, &QNetworkReply::readyRead, this, [this] {
    if (model_download_reply_ == nullptr || model_download_size_rejected_)
      return;
    const qint64 contentLength = modelReplyContentLength(*model_download_reply_);
    const auto expected = transcription::kWhisperModelBytes;
    if (!modelDownloadSizeAllowed(model_download_bytes_written_, contentLength, expected)) {
      model_download_size_rejected_ = true;
      model_download_reply_->abort();
      return;
    }
    if (model_download_file_ != nullptr) {
      const qint64 available = model_download_reply_->bytesAvailable();
      const auto remaining = expected - model_download_bytes_written_;
      if (available < 0 || static_cast<std::uintmax_t>(available) > remaining) {
        model_download_size_rejected_ = true;
        model_download_reply_->abort();
        return;
      }
      const QByteArray bytes = model_download_reply_->readAll();
      if (!modelDownloadSizeAllowed(model_download_bytes_written_ +
                                        static_cast<std::uintmax_t>(bytes.size()),
                                    contentLength, expected)) {
        model_download_size_rejected_ = true;
        model_download_reply_->abort();
        return;
      }
      if (model_download_file_->write(bytes) != bytes.size()) {
        model_download_write_failed_ = true;
        model_download_reply_->abort();
      } else {
        model_download_bytes_written_ += static_cast<std::uintmax_t>(bytes.size());
      }
    }
  });
  connect(model_download_reply_, &QNetworkReply::downloadProgress, this,
          [this](const qint64 received, const qint64 total) {
            window_.captionsPanel()->setModelDownloadState(
                {.modelId = QString::fromUtf8(transcription::kWhisperModelId.data()),
                 .filename = QString::fromUtf8(transcription::kWhisperModelFilename.data()),
                 .digestAlgorithm = QStringLiteral("sha1"),
                 .digest = QString::fromUtf8(transcription::kWhisperModelDigest.data()),
                 .receivedBytes = received,
                 .totalBytes = total,
                 .status = tr("Downloading model…"),
                 .state = desktop_ui::TranscriptionState::Downloading});
          });
  connect(model_download_reply_, &QNetworkReply::finished, this, [this, staging, stagedPath] {
    QNetworkReply* reply = model_download_reply_;
    model_download_reply_ = nullptr;
    const qint64 contentLength = modelReplyContentLength(*reply);
    if (model_download_file_ != nullptr) {
      if (!model_download_size_rejected_) {
        const qint64 available = reply->bytesAvailable();
        const auto remaining = transcription::kWhisperModelBytes - model_download_bytes_written_;
        if (available < 0 || static_cast<std::uintmax_t>(available) > remaining) {
          model_download_size_rejected_ = true;
        } else {
          const QByteArray bytes = reply->readAll();
          if (model_download_file_->write(bytes) != bytes.size()) {
            model_download_write_failed_ = true;
          } else {
            model_download_bytes_written_ += static_cast<std::uintmax_t>(bytes.size());
          }
        }
      }
      model_download_file_->close();
      delete model_download_file_;
      model_download_file_ = nullptr;
    }
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool httpOk = httpStatus == 0 || (httpStatus >= 200 && httpStatus < 300);
    const bool download_ok = !model_download_write_failed_ && !model_download_size_rejected_ &&
                             model_download_bytes_written_ == transcription::kWhisperModelBytes &&
                             modelDownloadSizeAllowed(model_download_bytes_written_, contentLength,
                                                      transcription::kWhisperModelBytes) &&
                             httpOk && reply->error() == QNetworkReply::NoError;
    if (!download_ok) {
      QDir(staging).removeRecursively();
      model_download_staging_.clear();
      if (model_download_size_rejected_) {
        window_.captionsPanel()->setTranscriptionState(
            desktop_ui::TranscriptionState::Failed,
            tr("Model download exceeded the pinned size limit; the staged file was discarded."));
      } else if (model_download_user_cancelled_ &&
                 reply->error() == QNetworkReply::OperationCanceledError) {
        window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::ModelMissing,
                                                       tr("Model download cancelled."));
      } else if (model_download_write_failed_) {
        window_.captionsPanel()->setTranscriptionState(
            desktop_ui::TranscriptionState::Failed,
            tr("Could not write the model download; the staged file was discarded."));
      } else {
        window_.captionsPanel()->setTranscriptionState(
            desktop_ui::TranscriptionState::Failed,
            reply->error() == QNetworkReply::NoError && httpOk
                ? tr("Model verification failed; the staged file was discarded.")
                : tr("Model download failed: %1").arg(reply->errorString()));
      }
      reply->deleteLater();
      return;
    }
    const QString cacheRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("models"));
    model_download_cache_root_ = cacheRoot;
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Downloading,
                                                   tr("Verifying model…"), 100);
    model_verification_stop_source_ = std::stop_source{};
    const std::uint64_t generation = ++model_verification_generation_;
    const std::stop_token cancellation = model_verification_stop_source_.get_token();
    model_verification_watcher_.setFuture(
        QtConcurrent::run([staging, cacheRoot, generation, cancellation] {
          ModelVerificationOutcome outcome;
          outcome.generation = generation;
          auto fetcher = transcription::make_unavailable_model_fetcher();
          transcription::ModelManager staged(pathFromQString(staging), *fetcher);
          outcome.staged_verified = staged.verify(cancellation);
          outcome.cancelled = cancellation.stop_requested();
          if (outcome.staged_verified && !outcome.cancelled) {
            transcription::ModelManager existing(pathFromQString(cacheRoot), *fetcher);
            outcome.existing_verified = existing.verify(cancellation);
            outcome.cancelled = cancellation.stop_requested();
          }
          return outcome;
        }));
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Downloading,
                                                   tr("Verifying model…"), 100);
    reply->deleteLater();
  });
}

void EditorController::modelVerificationFinished() {
  const auto outcome = model_verification_watcher_.result();
  const QString staging = std::exchange(model_download_staging_, {});
  const QString cacheRoot = std::exchange(model_download_cache_root_, {});
  const QString stagedPath =
      QDir(staging).filePath(QString::fromUtf8(transcription::kWhisperModelFilename.data()));
  const QString destination =
      QDir(cacheRoot).filePath(QString::fromUtf8(transcription::kWhisperModelFilename.data()));
  if (outcome.generation != model_verification_generation_ || outcome.cancelled) {
    QDir(staging).removeRecursively();
    if (outcome.cancelled)
      window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::ModelMissing,
                                                     tr("Model verification cancelled."));
    return;
  }
  if (!outcome.staged_verified) {
    QDir(staging).removeRecursively();
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Failed,
        tr("Model verification failed; the staged file was discarded."));
    return;
  }
  if (outcome.existing_verified) {
    QDir(staging).removeRecursively();
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Ready, tr("Model ready; transcription is on demand."), 100);
    return;
  }
  if (QFileInfo::exists(destination)) {
    const QString backup = QDir(cacheRoot).filePath(
        QStringLiteral(".%1.invalid-%2")
            .arg(QString::fromUtf8(transcription::kWhisperModelFilename.data()))
            .arg(QString::fromStdString(edit::EntityId::generate().toString())));
    if (!QFile::rename(destination, backup) || !QFile::rename(stagedPath, destination)) {
      if (QFileInfo::exists(backup) && !QFileInfo::exists(destination))
        QFile::rename(backup, destination);
      QDir(staging).removeRecursively();
      window_.captionsPanel()->setTranscriptionState(
          desktop_ui::TranscriptionState::Failed,
          tr("Could not safely replace the invalid installed model."));
    } else {
      QFile::remove(backup);
      QDir(staging).removeRecursively();
      window_.captionsPanel()->setTranscriptionState(
          desktop_ui::TranscriptionState::Ready,
          tr("Verified model replaced the invalid installed model."), 100);
    }
    return;
  }
  if (!QFile::rename(stagedPath, destination)) {
    QDir(staging).removeRecursively();
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Failed,
                                                   tr("Could not install the verified model."));
    return;
  }
  QDir(staging).removeRecursively();
  window_.captionsPanel()->setTranscriptionState(
      desktop_ui::TranscriptionState::Ready, tr("Model ready; transcription is on demand."), 100);
}

void EditorController::startTranscription(const desktop_ui::TranscriptionOptionsView& options) {
  if (transcription_session_ != nullptr || model_download_reply_ != nullptr ||
      model_verification_watcher_.isRunning())
    return;
  std::filesystem::path input;
  edit::TimeRange range;
  edit::EntityId clipId;
  if (!selectedAudioInput(input, range, clipId)) {
    window_.showTransientMessage(tr("Select an audio clip before transcribing."));
    return;
  }
  const edit::Sequence* selectedSequence = currentSequence();
  const edit::Clip* selected =
      selectedSequence == nullptr ? nullptr : edit::findClip(*selectedSequence, clipId);
  if (selected == nullptr) {
    window_.showTransientMessage(tr("The selected clip is no longer available."));
    return;
  }
  const auto cache = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                     QStringLiteral("/models");
  // Do not checksum the ~148 MiB model on the GUI thread. The worker owns the
  // authoritative verification immediately before inference and reports a
  // typed model error if the cache was removed or corrupted after the async
  // readiness check.
  jobs::v1::TranscribeOptions transcribeOptions;
  transcribeOptions.set_schema_version(transcription::kTranscriptionSchemaVersion);
  transcribeOptions.set_model_id(options.modelId.toStdString());
  transcribeOptions.set_language(options.language.isEmpty() ? "auto"
                                                            : options.language.toStdString());
  transcribeOptions.set_translate(options.translate);
  transcribeOptions.set_thread_count(
      static_cast<std::uint32_t>(std::clamp(options.threadCount, 0, 256)));
  // Timed words are the canonical caption/edit contract; the worker always
  // emits them even if an older UI payload omitted the option.
  transcribeOptions.set_word_timestamps(true);
  transcribeOptions.set_prefer_vulkan(options.preferVulkan);
  try {
    const auto startCentiseconds =
        selected->source_range.start.rescaledTo(100U, edit::RoundingMode::Floor).value();
    const auto endCentiseconds =
        selected->source_range.end().rescaledTo(100U, edit::RoundingMode::Ceil).value();
    const auto durationCentiseconds = endCentiseconds - startCentiseconds;
    if (startCentiseconds < 0 || endCentiseconds <= startCentiseconds ||
        durationCentiseconds <= 0) {
      window_.showTransientMessage(tr("The selected clip has an invalid source range."));
      return;
    }
    transcribeOptions.set_source_start_centiseconds(startCentiseconds);
    transcribeOptions.set_source_duration_centiseconds(durationCentiseconds);
  } catch (const std::exception&) {
    window_.showTransientMessage(tr("The selected source range is too large to transcribe."));
    return;
  }
  jobs::v1::JobSpec spec;
  transcription_job_id_ = QString::fromStdString(edit::EntityId::generate().toString());
  spec.set_job_id(transcription_job_id_.toStdString());
  spec.set_kind(jobs::v1::JOB_KIND_TRANSCRIBE);
  spec.add_input_uris(utf8StringFromPath(input));
  spec.set_preset_id("video-editor.transcribe.whisper-base.v1");
  if (!transcribeOptions.SerializeToString(spec.mutable_options())) {
    window_.showTransientMessage(tr("Could not encode transcription options."));
    return;
  }

  // Publish every piece of request and revision state before starting the
  // process. QProcess start/failure signals can arrive as soon as control
  // returns to the event loop, and must never observe the previous job's
  // frame or output tail.
  transcription_base_revision_ = editor_->revision().value;
  transcription_clip_id_ = clipId;
  transcription_clip_range_ = range;
  transcription_source_range_ = selected->source_range;
  transcription_playback_rate_ = selected->playback_rate;
  transcription_reversed_ = selected->reversed;
  pending_caption_additions_.clear();
  transcription_terminal_ = false;
  transcription_succeeded_ = false;
  transcription_reported_failure_ = false;
  caption_proposals_.clear();
  proposal_cut_ranges_.clear();
  proposal_caption_indices_.clear();
  window_.captionsPanel()->setReviewProposals({});
  window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Running,
                                                 tr("Transcribing selected audio…"), 0);

  auto* session = new WorkerHostSession(this);
  transcription_session_ = session;
  session->setEventHandler([this](const jobs::v1::WorkerEvent& event) {
    handleTranscriptionEvent(event);
  });
  session->setStartFailedHandler([this](const QString&) {
    transcription_terminal_ = true;
    transcription_succeeded_ = false;
    transcription_reported_failure_ = true;
    finishTranscriptionSession(true);
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Failed,
        tr("The transcription worker could not be started."));
  });
  session->setFinishedHandler([this](const bool abnormal, int, QProcess::ExitStatus) {
    finishTranscriptionSession(abnormal);
  });
  WorkerHostSession::LaunchOptions launch;
  launch.application_directory = QCoreApplication::applicationDirPath();
  launch.configured_path = qEnvironmentVariable("VIDEO_EDITOR_WORKER_HOST");
  launch.environment = QProcessEnvironment::systemEnvironment();
  launch.environment.insert(QStringLiteral("VIDEO_EDITOR_TRANSCRIPTION_MODEL_DIR"), cache);
  if (!session->start(spec, launch)) {
    transcription_session_ = nullptr;
    session->deleteLater();
    window_.showTransientMessage(tr("Could not start transcription."));
  }
}

void EditorController::cancelTranscription() {
  if (model_verification_watcher_.isRunning()) {
    model_verification_stop_source_.request_stop();
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Cancelling,
                                                   tr("Cancelling model verification…"));
  } else if (model_download_reply_ != nullptr) {
    model_download_user_cancelled_ = true;
    model_download_reply_->abort();
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Cancelling,
                                                   tr("Cancelling model download…"));
  } else if (transcription_session_ != nullptr) {
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Cancelling,
                                                   tr("Cancelling transcription…"));
    transcription_session_->cancel();
  }
}

void EditorController::handleTranscriptionEvent(const jobs::v1::WorkerEvent& event) {
  if (!event.has_event() || event.event().job_id() != transcription_job_id_.toStdString())
    return;
  const auto& job = event.event();
  const int percent = std::clamp(static_cast<int>(std::lround(job.progress() * 100.0)), 0, 100);
  if (job.state() == jobs::v1::JOB_STATE_RUNNING) {
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Running,
        tr("Transcribing selected audio · %1").arg(QString::fromStdString(job.phase())), percent);
    return;
  }
  if (job.state() != jobs::v1::JOB_STATE_SUCCEEDED) {
    transcription_terminal_ = true;
    if (job.has_error()) {
      transcription_reported_failure_ = true;
      window_.captionsPanel()->setTranscriptionState(
          desktop_ui::TranscriptionState::Failed,
          QString::fromStdString(job.error().user_message()));
    }
    return;
  }
  jobs::v1::TranscriptionResult result;
  if (!result.ParseFromString(job.result()) ||
      result.GetReflection()->GetUnknownFields(result).field_count() != 0 ||
      result.schema_version() != transcription::kTranscriptionSchemaVersion) {
    transcription_reported_failure_ = true;
    window_.captionsPanel()->setTranscriptionState(desktop_ui::TranscriptionState::Failed,
                                                   tr("Transcription result was invalid."));
    return;
  }
  transcription_terminal_ = true;
  transcription_succeeded_ = true;
  const auto* sequence = currentSequence();
  if (sequence == nullptr || transcription_clip_range_.duration <= edit::Time{} ||
      result.duration_centiseconds() <= 0)
    return;
  edit::Caption caption;
  caption.language = result.detected_language();
  caption.provenance.source = edit::CaptionWordSource::LocalTranscription;
  caption.provenance.model_identity = result.model_id() + ":" + result.model_digest();
  std::vector<edit::CaptionWord> source_words;
  source_words.reserve(static_cast<std::size_t>(result.words_size()));
  for (const auto& item : result.words()) {
    const QString wordText = QString::fromStdString(item.text()).trimmed();
    if (wordText.isEmpty())
      continue;
    constexpr std::int64_t kCentisecondSamples = 160;
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    if (item.start_centiseconds() > kMax / kCentisecondSamples ||
        item.start_centiseconds() < kMin / kCentisecondSamples ||
        item.end_centiseconds() > kMax / kCentisecondSamples ||
        item.end_centiseconds() < kMin / kCentisecondSamples) {
      continue;
    }
    const edit::Time sourceStart =
        edit::Time(item.start_centiseconds() * kCentisecondSamples, 16'000)
            .rescaledTo(transcription_source_range_.start.timescale(),
                        edit::RoundingMode::NearestTiesEven);
    const edit::Time sourceEnd = edit::Time(item.end_centiseconds() * kCentisecondSamples, 16'000)
                                     .rescaledTo(transcription_source_range_.start.timescale(),
                                                 edit::RoundingMode::NearestTiesEven);
    if (sourceEnd <= sourceStart) {
      continue;
    }
    edit::CaptionWord word;
    word.text = wordText.toStdString();
    word.range = edit::TimeRange(sourceStart, sourceEnd - sourceStart);
    word.probability = std::clamp(static_cast<double>(item.probability()), 0.0, 1.0);
    source_words.push_back(std::move(word));
  }
  caption.words = mapTranscriptionWordsToTimeline(
      source_words, transcription_source_range_, transcription_clip_range_,
      transcription_playback_rate_, transcription_reversed_);
  if (caption.words.empty())
    return;
  const QStringList words = [&caption] {
    QStringList word_texts;
    for (const auto& word : caption.words)
      word_texts.push_back(QString::fromStdString(word.text));
    return word_texts;
  }();
  caption.text = words.join(QLatin1Char(' ')).toStdString();
  caption.range =
      edit::TimeRange(caption.words.front().range.start,
                      caption.words.back().range.end() - caption.words.front().range.start);

  // Keep the worker's canonical timed result reviewable, but use the shared caption reflow
  // policy so long transcripts become bounded 42-character/two-line cues without losing word
  // timings.
  const auto document =
      caption_service::fromEditCaptions(std::span<const edit::Caption>(&caption, 1),
                                        caption_service::SubtitleFormat::Srt, caption.language);
  const auto reflowed = caption_service::reflow(document);
  if (!reflowed) {
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Failed,
        tr("The timed transcript could not be formatted for captions: %1")
            .arg(QString::fromStdString(reflowed.error().message)));
    return;
  }
  auto additions = caption_service::toEditCaptions(reflowed.value(), caption.style);
  if (additions.empty())
    return;
  const std::size_t first_addition = pending_caption_additions_.size();
  for (auto& addition : additions) {
    addition.style = caption.style;
    pending_caption_additions_.push_back(std::move(addition));
  }
  const bool stale = editor_->revision().value != transcription_base_revision_;
  const auto isFiller = [](QString token) {
    token = token.toLower();
    token.remove(QRegularExpression(QStringLiteral("[^a-z]")));
    return token == QStringLiteral("um") || token == QStringLiteral("uh") ||
           token == QStringLiteral("erm");
  };
  for (std::size_t index = first_addition; index < pending_caption_additions_.size(); ++index) {
    const auto& addition = pending_caption_additions_[index];
    const QString addition_text = QString::fromStdString(addition.text);
    caption_proposals_.push_back(
        {.id = QString::fromStdString(addition.id.toString()),
         .kind = QStringLiteral("Transcript suggestion"),
         .summary = tr("Add %1").arg(addition_text),
         .previewRange =
             tr("%1 → %2")
                 .arg(timecodeText(toUiTime(addition.range.start), sequence->frame_rate))
                 .arg(timecodeText(toUiTime(addition.range.end()), sequence->frame_rate)),
         .confidence =
             stale ? tr("Stale result; regenerate before applying") : tr("Review before applying"),
         .selected = true});
    proposal_cut_ranges_.push_back({});
    proposal_caption_indices_.push_back(static_cast<int>(index));
    for (const auto& word : addition.words) {
      if (!isFiller(QString::fromStdString(word.text)))
        continue;
      caption_proposals_.push_back(
          {.id = QString::fromStdString(word.id.toString()),
           .kind = QStringLiteral("Transcript filler"),
           .summary = tr("Remove filler “%1”").arg(QString::fromStdString(word.text)),
           .previewRange = tr("%1 → %2")
                               .arg(timecodeText(toUiTime(word.range.start), sequence->frame_rate))
                               .arg(timecodeText(toUiTime(word.range.end()), sequence->frame_rate)),
           .confidence = tr("Off by default; review transcript context"),
           .selected = false});
      proposal_cut_ranges_.push_back(word.range);
      proposal_caption_indices_.push_back(-1);
    }
  }
  window_.captionsPanel()->setReviewProposals(caption_proposals_);
  if (!stale)
    launchCaptionReviewAnalysis();
  window_.captionsPanel()->setTranscriptionState(
      desktop_ui::TranscriptionState::Ready,
      stale ? tr("Result is stale because the project changed; regenerate before applying.")
            : tr("Transcript ready for review; no edits have been applied."),
      100);
}

void EditorController::finishTranscriptionSession(const bool abnormal) {
  if (transcription_session_ == nullptr)
    return;
  WorkerHostSession* session = transcription_session_;
  transcription_session_ = nullptr;
  session->deleteLater();
  if ((abnormal || !transcription_terminal_) && !transcription_reported_failure_ &&
      !transcription_succeeded_ && window_.captionsPanel() != nullptr) {
    window_.captionsPanel()->setTranscriptionState(
        desktop_ui::TranscriptionState::Failed,
        abnormal ? tr("The transcription worker stopped unexpectedly.")
                 : tr("Transcription ended without a reviewable result."));
  }
}

void EditorController::launchCaptionReviewAnalysis() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || editor_ == nullptr)
    return;
  const auto snapshotResult = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshotResult) {
    window_.showTransientMessage(tr("Measured silence is unavailable for this revision."));
    return;
  }
  if (caption_analysis_watcher_.isRunning())
    caption_analysis_stop_source_.request_stop();
  caption_analysis_stop_source_ = std::stop_source{};
  const std::uint64_t generation = ++caption_analysis_generation_;
  const std::uint64_t baseRevision = editor_->revision().value;
  const edit::TimeRange range = transcription_clip_range_;
  const auto snapshot = std::move(snapshotResult).value();
  const auto renderer = std::make_shared<audio_render::TimelineAudioRenderer>(audio_registry_);
  const std::stop_token cancellation = caption_analysis_stop_source_.get_token();
  caption_analysis_future_ = QtConcurrent::run([snapshot = std::move(snapshot), renderer, range,
                                                generation, baseRevision, cancellation]() {
    CaptionAnalysisOutcome outcome;
    outcome.generation = generation;
    outcome.base_revision = baseRevision;
    const auto toSample = [](const edit::Time time) {
      return time
          .rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::NearestTiesEven)
          .value();
    };
    const std::int64_t start = toSample(range.start);
    const std::int64_t end = toSample(range.end());
    if (end <= start) {
      outcome.error = QObject::tr("Measured silence range is empty.");
      return outcome;
    }
    constexpr std::size_t kBlockSamples = 96'000U;
    const std::size_t total = static_cast<std::size_t>(end - start);
    audio_render::SilenceOptions options;
    options.analysis_window_samples = 480U;
    options.minimum_silence_samples = 2'400U;
    options.merge_gap_samples = 2'400U;
    audio_render::SilenceAccumulator detector(options);
    std::size_t rendered_samples = 0;
    while (rendered_samples < total) {
      if (cancellation.stop_requested()) {
        outcome.error = QObject::tr("Measured silence analysis cancelled.");
        return outcome;
      }
      const std::size_t remaining = total - rendered_samples;
      const std::size_t count = std::min(kBlockSamples, remaining);
      const auto rendered = renderer->render(
          snapshot, {.start_sample = start + static_cast<std::int64_t>(rendered_samples),
                     .sample_count = count,
                     .cancellation = cancellation});
      if (!rendered) {
        outcome.error = QString::fromStdString(rendered.error().message);
        return outcome;
      }
      if (!detector.add(rendered.value())) {
        outcome.error = QString::fromStdString(detector.error()->message);
        return outcome;
      }
      rendered_samples += count;
    }
    const auto detected = detector.finish();
    if (!detected) {
      outcome.error = QString::fromStdString(detected.error().message);
      return outcome;
    }
    outcome.silence_ranges = detected.value();
    return outcome;
  });
  caption_analysis_watcher_.setFuture(caption_analysis_future_);
}

void EditorController::captionAnalysisFinished() {
  const CaptionAnalysisOutcome outcome = caption_analysis_watcher_.result();
  if (outcome.generation != caption_analysis_generation_ ||
      outcome.base_revision != editor_->revision().value ||
      outcome.base_revision != transcription_base_revision_) {
    return;
  }
  if (!outcome.error.isEmpty()) {
    window_.showTransientMessage(tr("Measured silence unavailable: %1").arg(outcome.error));
    return;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr)
    return;
  const std::int64_t clipStartSample =
      transcription_clip_range_.start
          .rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::NearestTiesEven)
          .value();
  const std::int64_t clipEndSample =
      transcription_clip_range_.end()
          .rescaledTo(audio_render::kTimelineAudioSampleRate, edit::RoundingMode::NearestTiesEven)
          .value();
  constexpr std::int64_t kSilencePaddingSamples = 240; // 5 ms safe cut padding.
  for (const auto& silence : outcome.silence_ranges) {
    const audio_render::SilenceRange padded{
        std::max(clipStartSample, silence.start_sample + kSilencePaddingSamples),
        std::min(clipEndSample, silence.end_sample - kSilencePaddingSamples)};
    if (padded.end_sample <= padded.start_sample)
      continue;
    const edit::TimeRange range = padded.time_range();
    caption_proposals_.push_back(
        {.id = QStringLiteral("silence-%1-%2").arg(silence.start_sample).arg(silence.end_sample),
         .kind = QStringLiteral("Measured silence"),
         .summary = tr("Remove measured silent range"),
         .previewRange = tr("%1 → %2")
                             .arg(timecodeText(toUiTime(range.start), sequence->frame_rate))
                             .arg(timecodeText(toUiTime(range.end()), sequence->frame_rate)),
         .confidence = tr("Measured from rendered 48 kHz timeline audio"),
         .selected = true});
    proposal_cut_ranges_.push_back(range);
    proposal_caption_indices_.push_back(-1);
  }
  window_.captionsPanel()->setReviewProposals(caption_proposals_);
}

void EditorController::applyCaptionReview() {
  if (pending_caption_additions_.empty() && proposal_cut_ranges_.isEmpty())
    return;
  if (editor_->revision().value != transcription_base_revision_) {
    window_.showTransientMessage(
        tr("The project changed; regenerate this transcript before applying."));
    return;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr)
    return;
  std::vector<edit::Caption> selected;
  std::vector<edit::TimeRange> selectedRanges;
  for (qsizetype index = 0; index < caption_proposals_.size(); ++index) {
    if (!caption_proposals_.at(index).selected)
      continue;
    const int captionIndex = proposal_caption_indices_.value(index, -1);
    if (captionIndex >= 0 && captionIndex < static_cast<int>(pending_caption_additions_.size())) {
      selected.push_back(pending_caption_additions_.at(static_cast<std::size_t>(captionIndex)));
    }
    const edit::TimeRange cut = proposal_cut_ranges_.value(index);
    if (!cut.empty())
      selectedRanges.push_back(cut);
  }
  std::sort(selectedRanges.begin(), selectedRanges.end(),
            [](const edit::TimeRange& left, const edit::TimeRange& right) {
              return left.start < right.start;
            });
  std::vector<edit::TimeRange> merged;
  for (const auto& range : selectedRanges) {
    if (!merged.empty() && merged.back().end() >= range.start) {
      const edit::Time end = std::max(merged.back().end(), range.end());
      merged.back().duration = end - merged.back().start;
    } else {
      merged.push_back(range);
    }
  }
  if (selected.empty() && merged.empty())
    return;
  std::vector<edit::EditCommand> commands;
  if (!merged.empty()) {
    const auto snapshot = editor_->snapshot(sequence->id, editor_->revision());
    if (!snapshot) {
      window_.showTransientMessage(tr("Could not capture the review revision."));
      return;
    }
    auto proposal = caption_service::buildTimelineCutProposal(snapshot.value(), merged);
    if (!proposal || !proposal.value().timeline_cuts.has_value()) {
      window_.showTransientMessage(tr("Could not build the selected timeline cut proposal."));
      return;
    }
    commands.push_back({.operation = *proposal.value().timeline_cuts, .coalescing_key = {}});
    auto changes = proposal.value().caption_changes;
    for (const auto& caption : selected) {
      const auto mapped = caption_service::mapCaptionThroughCuts(caption, merged);
      if (mapped.has_value())
        changes.added.push_back(*mapped);
    }
    if (!changes.added.empty() || !changes.updated.empty() || !changes.removed.empty()) {
      commands.push_back({.operation = std::move(changes), .coalescing_key = {}});
    }
  } else if (!selected.empty()) {
    edit::ApplyCaptionChangeSetCommand changes;
    changes.sequence_id = sequence->id;
    changes.added = std::move(selected);
    commands.push_back({.operation = std::move(changes), .coalescing_key = {}});
  }
  if (applyBatch(std::move(commands), tr("Could not apply caption review suggestions"))) {
    pending_caption_additions_.clear();
    caption_proposals_.clear();
    proposal_cut_ranges_.clear();
    proposal_caption_indices_.clear();
    window_.captionsPanel()->setReviewProposals({});
  }
}

void EditorController::discardCaptionReview() {
  pending_caption_additions_.clear();
  caption_proposals_.clear();
  proposal_cut_ranges_.clear();
  proposal_caption_indices_.clear();
  window_.captionsPanel()->setReviewProposals({});
  window_.showTransientMessage(tr("Transcript suggestions discarded"));
}

void EditorController::captionStyleEdited(const QString& captionId,
                                          const desktop_ui::CaptionStyleView& style) {
  const auto id = parseId(captionId);
  const edit::Sequence* sequence = currentSequence();
  if (!id.has_value() || sequence == nullptr)
    return;
  const auto it = std::find_if(sequence->captions.cbegin(), sequence->captions.cend(),
                               [&id](const edit::Caption& caption) { return caption.id == *id; });
  if (it == sequence->captions.cend())
    return;
  edit::Caption caption = *it;
  caption.style.font_family = style.fontFamily.toStdString();
  caption.style.font_size = style.fontSize;
  caption.style.text_color = {style.textColor.redF(), style.textColor.greenF(),
                              style.textColor.blueF(), style.textColor.alphaF()};
  caption.style.background_color = {style.backgroundColor.redF(), style.backgroundColor.greenF(),
                                    style.backgroundColor.blueF(), style.backgroundColor.alphaF()};
  caption.style.bold = style.bold;
  caption.style.italic = style.italic;
  caption.style.vertical_position = style.verticalPosition;
  caption.style.safe_margin = style.safeMargin;
  caption.style.outline_width = style.outlineWidth;
  caption.style.outline_color = {style.outlineColor.redF(), style.outlineColor.greenF(),
                                 style.outlineColor.blueF(), style.outlineColor.alphaF()};
  caption.style.alignment =
      style.alignment == QStringLiteral("left")
          ? edit::CaptionAlignment::Left
          : (style.alignment == QStringLiteral("right") ? edit::CaptionAlignment::Right
                                                        : edit::CaptionAlignment::Center);
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateCaptionCommand{sequence->id, std::move(caption)},
                        .coalescing_key = {}},
      tr("Could not update caption style"));
}

void EditorController::updateSelectedClipProperty(const QString& parameterId,
                                                  const QVariant& value) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    window_.showTransientMessage(tr("Select a clip before changing its properties"));
    return;
  }
  const edit::Clip* selected = edit::findClip(*sequence, *active_clip_id_);
  if (selected == nullptr) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    refreshInspectorView();
    window_.showTransientMessage(tr("The selected clip is no longer available"));
    return;
  }

  const std::string coalescing_key =
      "inspector:" + selected->id.toString() + ":" + parameterId.toStdString();
  if (parameterId == QStringLiteral("positionX") || parameterId == QStringLiteral("positionY") ||
      parameterId == QStringLiteral("scale") || parameterId == QStringLiteral("scaleX") ||
      parameterId == QStringLiteral("scaleY") || parameterId == QStringLiteral("rotation") ||
      parameterId == QStringLiteral("opacity") || parameterId == QStringLiteral("anchorX") ||
      parameterId == QStringLiteral("anchorY") || parameterId == QStringLiteral("cropLeft") ||
      parameterId == QStringLiteral("cropTop") || parameterId == QStringLiteral("cropRight") ||
      parameterId == QStringLiteral("cropBottom")) {
    edit::Transform transform = selected->transform;
    if (parameterId == QStringLiteral("positionX")) {
      transform.position.x = value.toDouble();
    } else if (parameterId == QStringLiteral("positionY")) {
      transform.position.y = value.toDouble();
    } else if (parameterId == QStringLiteral("scale")) {
      const double uniform_scale = value.toDouble() / 100.0;
      transform.scale = {uniform_scale, uniform_scale};
    } else if (parameterId == QStringLiteral("scaleX")) {
      transform.scale.x = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("scaleY")) {
      transform.scale.y = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("rotation")) {
      transform.rotation_degrees = value.toDouble();
    } else if (parameterId == QStringLiteral("opacity")) {
      transform.opacity = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("anchorX")) {
      transform.anchor_x = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("anchorY")) {
      transform.anchor_y = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropLeft")) {
      transform.crop_left = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropTop")) {
      transform.crop_top = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropRight")) {
      transform.crop_right = value.toDouble() / 100.0;
    } else if (parameterId == QStringLiteral("cropBottom")) {
      transform.crop_bottom = value.toDouble() / 100.0;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipTransformCommand{.sequence_id = sequence->id,
                                                                     .clip_id = selected->id,
                                                                     .transform = transform},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip"));
    return;
  }

  if (parameterId == QStringLiteral("blendMode")) {
    const QString mode_id = value.toString();
    edit::BlendMode mode = edit::BlendMode::Normal;
    if (mode_id == QStringLiteral("add")) {
      mode = edit::BlendMode::Add;
    } else if (mode_id == QStringLiteral("multiply")) {
      mode = edit::BlendMode::Multiply;
    } else if (mode_id == QStringLiteral("screen")) {
      mode = edit::BlendMode::Screen;
    } else if (mode_id == QStringLiteral("overlay")) {
      mode = edit::BlendMode::Overlay;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipBlendModeCommand{.sequence_id = sequence->id,
                                                                     .clip_id = selected->id,
                                                                     .blend_mode = mode},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip blend mode"));
    return;
  }

  if (parameterId == QStringLiteral("audioGain") || parameterId == QStringLiteral("audioPan") ||
      parameterId == QStringLiteral("fadeIn") || parameterId == QStringLiteral("fadeOut")) {
    double gain_db = selected->audio_gain_db;
    double pan = selected->audio_pan;
    edit::Time fade_in = selected->fade_in;
    edit::Time fade_out = selected->fade_out;
    if (parameterId == QStringLiteral("audioGain")) {
      gain_db = value.toDouble();
    } else if (parameterId == QStringLiteral("audioPan")) {
      pan = value.toDouble() / 100.0;
    } else {
      const edit::Time fade_samples(
          static_cast<std::int64_t>(std::llround(value.toDouble() * 48'000.0)), 48'000);
      if (parameterId == QStringLiteral("fadeIn")) {
        fade_in = fade_samples;
      } else {
        fade_out = fade_samples;
      }
    }
    (void)apply(
        edit::EditCommand{.operation =
                              edit::SetClipAudioPropertiesCommand{.sequence_id = sequence->id,
                                                                  .clip_id = selected->id,
                                                                  .gain_db = gain_db,
                                                                  .pan = pan,
                                                                  .fade_in = fade_in,
                                                                  .fade_out = fade_out},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip audio"));
    return;
  }

  if (parameterId == QStringLiteral("speed") || parameterId == QStringLiteral("reverse")) {
    // Speed is expressed as a percentage of normal in the UI (100 = 1x).
    const double speed_percent =
        (parameterId == QStringLiteral("speed")) ? value.toDouble() : selected_speed_percent_;
    const bool reversed =
        (parameterId == QStringLiteral("reverse")) ? value.toBool() : selected->reversed;
    selected_speed_percent_ = speed_percent;
    // Convert percentage to an exact rational rate. 100% = 1/1, 200% = 2/1,
    // 50% = 1/2. Use a reduced fraction of (percent, 100).
    const auto percent = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(std::llround(speed_percent), 1, 100'000));
    std::uint32_t num = percent;
    std::uint32_t den = 100U;
    const auto g = static_cast<std::uint32_t>(std::gcd(num, den));
    if (g > 1U) {
      num /= g;
      den /= g;
    }
    const edit::Rate rate{num, den};
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipSpeedCommand{.sequence_id = sequence->id,
                                                                 .clip_id = selected->id,
                                                                 .playback_rate = rate,
                                                                 .reversed = reversed},
                          .coalescing_key = coalescing_key},
        tr("Could not update the selected clip speed"));
    return;
  }

  if (parameterId == QStringLiteral("titleText") || parameterId == QStringLiteral("titleFont") ||
      parameterId == QStringLiteral("titleSize") || parameterId == QStringLiteral("titleAlign") ||
      parameterId == QStringLiteral("titleBold") || parameterId == QStringLiteral("titleItalic")) {
    if (selected->kind != edit::ClipKind::Title || !selected->title.has_value()) {
      return;
    }
    edit::Title title = *selected->title;
    if (parameterId == QStringLiteral("titleText")) {
      title.text = value.toString().toStdString();
    } else if (parameterId == QStringLiteral("titleFont")) {
      title.font_family = value.toString().toStdString();
    } else if (parameterId == QStringLiteral("titleSize")) {
      title.font_size = value.toDouble();
    } else if (parameterId == QStringLiteral("titleAlign")) {
      const QString align = value.toString();
      title.horizontal_alignment =
          align == QStringLiteral("left")    ? edit::TitleHorizontalAlignment::Left
          : align == QStringLiteral("right") ? edit::TitleHorizontalAlignment::Right
                                             : edit::TitleHorizontalAlignment::Center;
    } else if (parameterId == QStringLiteral("titleBold")) {
      title.bold = value.toBool();
    } else if (parameterId == QStringLiteral("titleItalic")) {
      title.italic = value.toBool();
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetClipTitleCommand{.sequence_id = sequence->id,
                                                                 .clip_id = selected->id,
                                                                 .title = title},
                          .coalescing_key = coalescing_key},
        tr("Could not update the title"));
  }
}

void EditorController::toggleSelectedClipKeyframe(const QString& parameterId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || active_clip_id_.has_value() == false) {
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    return;
  }
  // Find the effect parameter matching this Inspector field. The Inspector
  // parameter IDs (positionX, scale, etc.) map to transform/effect fields;
  // for now keyframing is wired only for effect parameters that carry the
  // same id. A full keyframe-curve editor is a follow-up; this toggles a
  // keyframe at the current playhead for the named parameter.
  // Effect keyframes are clip-local offsets.  Moving a clip therefore moves its
  // animation with it; the playhead is converted to that local coordinate here.
  const edit::Time playhead = playheadTime();
  const edit::Time clip_start = clip->timeline_range.start.rescaledTo(
      playhead.timescale(), edit::RoundingMode::NearestTiesEven);
  const edit::Time key_time =
      std::clamp(playhead - clip_start, edit::Time{},
                 clip->timeline_range.duration.rescaledTo(playhead.timescale(),
                                                          edit::RoundingMode::NearestTiesEven));
  for (const auto& effect : clip->effects) {
    auto it = effect.parameters.find(parameterId.toStdString());
    if (it == effect.parameters.end()) {
      continue;
    }
    edit::EffectParameter parameter = it->second;
    const auto existing =
        std::find_if(parameter.keyframes.begin(), parameter.keyframes.end(),
                     [&](const edit::Keyframe& key) { return key.time == key_time; });
    if (existing != parameter.keyframes.end()) {
      parameter.keyframes.erase(existing);
    } else {
      edit::Keyframe key;
      key.time = key_time;
      key.value = parameter.value;
      key.interpolation = edit::KeyframeInterpolation::Linear;
      parameter.keyframes.push_back(key);
      std::sort(parameter.keyframes.begin(), parameter.keyframes.end(),
                [](const edit::Keyframe& a, const edit::Keyframe& b) { return a.time < b.time; });
    }
    (void)apply(
        edit::EditCommand{.operation =
                              edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                  .clip_id = clip->id,
                                                                  .effect_id = effect.id,
                                                                  .parameter = parameter},
                          .coalescing_key = {}},
        tr("Could not toggle the keyframe"));
    return;
  }
}

void EditorController::addEffect(const QString& effectId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    window_.showTransientMessage(tr("Select a clip before adding an effect"));
    return;
  }
  if (effectId.startsWith(QStringLiteral("transition."))) {
    window_.showTransientMessage(tr("Transitions are added by dragging them between clips"));
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    return;
  }
  edit::Effect effect = effectPreset(effectId);
  const bool audio_effect = effectId.startsWith(QStringLiteral("audio."));
  if (audio_effect && clip->kind != edit::ClipKind::Audio) {
    window_.showTransientMessage(tr("Select an audio clip for this effect"));
    return;
  }
  if (!audio_effect && clip->kind != edit::ClipKind::Video && clip->kind != edit::ClipKind::Title) {
    window_.showTransientMessage(tr("Select a video clip for this effect"));
    return;
  }
  (void)apply(
      edit::EditCommand{.operation = edit::AddClipEffectCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect = std::move(effect)},
                        .coalescing_key = {}},
      tr("Could not add the effect"));
}

void EditorController::updateSelectedEffectParameter(const QString& effectId,
                                                     const QString& parameterId,
                                                     const QVariant& value) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto parameter = effect->parameters.find(parameterId.toStdString());
  if (parameter == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = parameter->second;
  updated.value = effectValueFromUi(updated.value, value);
  (void)apply(
      edit::EditCommand{
          .operation = edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                           .clip_id = clip->id,
                                                           .effect_id = effect->id,
                                                           .parameter = std::move(updated)},
          .coalescing_key = "effect:" + effectId.toStdString() + ":" + parameterId.toStdString()},
      tr("Could not update the effect parameter"));
}

void EditorController::toggleSelectedEffectKeyframe(const QString& effectId,
                                                    const QString& parameterId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto found = effect->parameters.find(parameterId.toStdString());
  if (found == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = found->second;
  const edit::Time playhead = playheadTime();
  const edit::Time clip_start = clip->timeline_range.start.rescaledTo(
      playhead.timescale(), edit::RoundingMode::NearestTiesEven);
  const edit::Time clip_duration = clip->timeline_range.duration.rescaledTo(
      playhead.timescale(), edit::RoundingMode::NearestTiesEven);
  const edit::Time max_keyframe_time =
      clip_duration.isZero() ? edit::Time{}
                             : clip_duration - edit::Time(1, clip_duration.timescale());
  const edit::Time current_time =
      std::clamp(playhead - clip_start, edit::Time{}, max_keyframe_time);
  const auto existing =
      std::find_if(updated.keyframes.begin(), updated.keyframes.end(),
                   [&current_time](const auto& keyframe) { return keyframe.time == current_time; });
  if (existing == updated.keyframes.end()) {
    updated.keyframes.push_back({.time = current_time,
                                 .value = updated.value,
                                 .interpolation = edit::KeyframeInterpolation::Linear});
  } else {
    updated.keyframes.erase(existing);
  }
  std::sort(updated.keyframes.begin(), updated.keyframes.end(),
            [](const auto& left, const auto& right) { return left.time < right.time; });
  (void)apply(
      edit::EditCommand{.operation =
                            edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect_id = effect->id,
                                                                .parameter = std::move(updated)},
                        .coalescing_key = {}},
      tr("Could not toggle the effect keyframe"));
}

void EditorController::selectEffectKeyframe(const QString& effectId, const QString& parameterId,
                                            const qint64 time) {
  Q_UNUSED(effectId)
  Q_UNUSED(parameterId)
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    return;
  }
  const edit::Time local_time = timelineTime(std::max<qint64>(0, time));
  const edit::Time duration = clip->timeline_range.duration;
  const edit::Time max_local =
      duration.isZero() ? edit::Time{} : duration - edit::Time(1, duration.timescale());
  const edit::Time clamped = std::clamp(local_time, edit::Time{}, max_local);
  seek(timelineValue(clip->timeline_range.start + clamped));
}

void EditorController::updateSelectedEffectKeyframe(const QString& effectId,
                                                    const QString& parameterId,
                                                    const QString& keyframeId, const qint64 time,
                                                    const double value) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const auto parsed_keyframe = parseId(keyframeId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value() || !parsed_keyframe.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto found = effect->parameters.find(parameterId.toStdString());
  if (found == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = found->second;
  const auto keyframe = std::find_if(
      updated.keyframes.begin(), updated.keyframes.end(),
      [&parsed_keyframe](const auto& candidate) { return candidate.id == *parsed_keyframe; });
  if (keyframe == updated.keyframes.end()) {
    return;
  }
  const edit::Time clip_duration = clip->timeline_range.duration.rescaledTo(
      timeline_time_scale_, edit::RoundingMode::NearestTiesEven);
  const edit::Time new_time =
      timelineTime(std::clamp<qint64>(time, 0, timelineValue(clip_duration)));
  const bool duplicate =
      std::any_of(updated.keyframes.cbegin(), updated.keyframes.cend(),
                  [&new_time, &parsed_keyframe](const auto& candidate) {
                    return candidate.id != *parsed_keyframe && candidate.time == new_time;
                  });
  if (duplicate) {
    window_.showTransientMessage(tr("Two keyframes cannot occupy the same time"));
    return;
  }
  keyframe->time = new_time;
  keyframe->value = effectValueFromDouble(keyframe->value, value);
  std::sort(updated.keyframes.begin(), updated.keyframes.end(),
            [](const auto& left, const auto& right) { return left.time < right.time; });
  (void)apply(
      edit::EditCommand{.operation =
                            edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect_id = effect->id,
                                                                .parameter = std::move(updated)},
                        .coalescing_key = {}},
      tr("Could not update the keyframe"));
}

void EditorController::updateSelectedEffectInterpolation(
    const QString& effectId, const QString& parameterId, const QString& keyframeId,
    const desktop_ui::KeyframeInterpolationView interpolation) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const auto parsed_keyframe = parseId(keyframeId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value() || !parsed_keyframe.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto found = effect->parameters.find(parameterId.toStdString());
  if (found == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = found->second;
  const auto keyframe = std::find_if(
      updated.keyframes.begin(), updated.keyframes.end(),
      [&parsed_keyframe](const auto& candidate) { return candidate.id == *parsed_keyframe; });
  if (keyframe == updated.keyframes.end()) {
    return;
  }
  keyframe->interpolation = interpolation == desktop_ui::KeyframeInterpolationView::Hold
                                ? edit::KeyframeInterpolation::Hold
                            : interpolation == desktop_ui::KeyframeInterpolationView::Bezier
                                ? edit::KeyframeInterpolation::Bezier
                                : edit::KeyframeInterpolation::Linear;
  (void)apply(
      edit::EditCommand{.operation =
                            edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect_id = effect->id,
                                                                .parameter = std::move(updated)},
                        .coalescing_key = {}},
      tr("Could not update keyframe interpolation"));
}

void EditorController::removeSelectedEffectKeyframe(const QString& effectId,
                                                    const QString& parameterId,
                                                    const QString& keyframeId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const auto parsed_keyframe = parseId(keyframeId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value() || !parsed_keyframe.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto found = effect->parameters.find(parameterId.toStdString());
  if (found == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = found->second;
  const auto removed = std::remove_if(
      updated.keyframes.begin(), updated.keyframes.end(),
      [&parsed_keyframe](const auto& candidate) { return candidate.id == *parsed_keyframe; });
  if (removed == updated.keyframes.end()) {
    return;
  }
  updated.keyframes.erase(removed, updated.keyframes.end());
  (void)apply(
      edit::EditCommand{.operation =
                            edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect_id = effect->id,
                                                                .parameter = std::move(updated)},
                        .coalescing_key = {}},
      tr("Could not delete the keyframe"));
}

void EditorController::updateSelectedEffectControlPoints(const QString& effectId,
                                                         const QString& parameterId,
                                                         const QString& keyframeId,
                                                         const QPointF& incoming,
                                                         const QPointF& outgoing) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    return;
  }
  const auto parsed_effect = parseId(effectId);
  const auto parsed_keyframe = parseId(keyframeId);
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr || !parsed_effect.has_value() || !parsed_keyframe.has_value()) {
    return;
  }
  const auto effect = std::find_if(
      clip->effects.cbegin(), clip->effects.cend(),
      [&parsed_effect](const auto& candidate) { return candidate.id == *parsed_effect; });
  if (effect == clip->effects.cend()) {
    return;
  }
  const auto found = effect->parameters.find(parameterId.toStdString());
  if (found == effect->parameters.end()) {
    return;
  }
  edit::EffectParameter updated = found->second;
  const auto keyframe = std::find_if(
      updated.keyframes.begin(), updated.keyframes.end(),
      [&parsed_keyframe](const auto& candidate) { return candidate.id == *parsed_keyframe; });
  if (keyframe == updated.keyframes.end()) {
    return;
  }
  keyframe->incoming_control = {.x = incoming.x(), .y = incoming.y()};
  keyframe->outgoing_control = {.x = outgoing.x(), .y = outgoing.y()};
  (void)apply(
      edit::EditCommand{.operation =
                            edit::SetClipEffectParameterCommand{.sequence_id = sequence->id,
                                                                .clip_id = clip->id,
                                                                .effect_id = effect->id,
                                                                .parameter = std::move(updated)},
                        .coalescing_key = {}},
      tr("Could not update keyframe curve handles"));
}

void EditorController::addTitleClip() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  // Insert a 5-second title clip on the first video track at the playhead.
  const edit::Time start = playheadTime();
  const edit::Time duration(5, 1);
  edit::Clip clip;
  clip.kind = edit::ClipKind::Title;
  clip.name = "Title";
  clip.timeline_range = edit::TimeRange(start, duration);
  clip.source_range = edit::TimeRange(edit::Time{}, duration);
  clip.playback_rate = edit::Rate{1, 1};
  clip.title = edit::Title{};
  // Place on the first video track that is not locked.
  edit::EntityId track_id;
  for (const auto& track : sequence->tracks) {
    if (track.kind == edit::TrackKind::Video && !track.locked) {
      track_id = track.id;
      break;
    }
  }
  if (track_id.isNil()) {
    window_.showTransientMessage(tr("No unlocked video track for a title"));
    return;
  }
  edit::InsertClipCommand insert{
      .sequence_id = sequence->id, .track_id = track_id, .clip = std::move(clip)};
  (void)apply(edit::EditCommand{.operation = std::move(insert), .coalescing_key = {}},
              tr("Could not add a title"));
}

void EditorController::setTransitionSelection(const QString& transitionId) {
  selected_transition_id_ = parseId(transitionId);
}

void EditorController::updateTransitionDuration(const QString& transitionId,
                                                const qint64 duration) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  const edit::Transition* transition = edit::findTransition(*sequence, *parsed_id);
  if (transition == nullptr) {
    return;
  }
  edit::Transition updated = *transition;
  const edit::Time new_duration = timelineTime(std::max<qint64>(1, duration));
  // Preserve the end of the transition range; adjust the start.
  const edit::Time end = updated.range.start + updated.range.duration;
  updated.range = edit::TimeRange(end - new_duration, new_duration);
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateTransitionCommand{.sequence_id = sequence->id,
                                                                   .transition = updated},
                        .coalescing_key = {}},
      tr("Could not update the transition duration"));
}

void EditorController::removeTransition(const QString& transitionId) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  (void)apply(
      edit::EditCommand{.operation = edit::RemoveTransitionCommand{.sequence_id = sequence->id,
                                                                   .transition_id = *parsed_id},
                        .coalescing_key = {}},
      tr("Could not remove the transition"));
  selected_transition_id_.reset();
}

void EditorController::changeTransitionPreset(const QString& transitionId, const QString& kind) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto parsed_id = parseId(transitionId);
  if (!parsed_id.has_value()) {
    return;
  }
  const edit::Transition* transition = edit::findTransition(*sequence, *parsed_id);
  if (transition == nullptr) {
    return;
  }
  edit::Transition updated = *transition;
  updated.kind = (kind == QStringLiteral("dip_to_black")) ? edit::TransitionKind::DipToBlack
                                                          : edit::TransitionKind::CrossDissolve;
  (void)apply(
      edit::EditCommand{.operation = edit::UpdateTransitionCommand{.sequence_id = sequence->id,
                                                                   .transition = updated},
                        .coalescing_key = {}},
      tr("Could not change the transition preset"));
}

void EditorController::setAudioTrackMuted(const int trackIndex, const bool muted) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioStateCommand{.sequence_id = sequence->id,
                                                                       .track_id = track.id,
                                                                       .muted = muted,
                                                                       .solo = track.solo},
                          .coalescing_key = "mixer:" + track.id.toString() + ":mute"},
        tr("Could not update track mute"));
    return;
  }
  refreshMixerView();
}

void EditorController::setAudioTrackSolo(const int trackIndex, const bool soloed) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioStateCommand{.sequence_id = sequence->id,
                                                                       .track_id = track.id,
                                                                       .muted = track.muted,
                                                                       .solo = soloed},
                          .coalescing_key = "mixer:" + track.id.toString() + ":solo"},
        tr("Could not update track solo"));
    return;
  }
  refreshMixerView();
}

void EditorController::setAudioTrackGain(const int trackIndex, const double gainDb) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioMixCommand{.sequence_id = sequence->id,
                                                                     .track_id = track.id,
                                                                     .gain_db = gainDb,
                                                                     .pan = track.audio_pan},
                          .coalescing_key = "mixer:" + track.id.toString() + ":gain"},
        tr("Could not update track gain"));
    return;
  }
  refreshMixerView();
}

void EditorController::setAudioTrackPan(const int trackIndex, const double pan) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0) {
    refreshMixerView();
    return;
  }
  int audio_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio) {
      continue;
    }
    if (audio_index++ != trackIndex) {
      continue;
    }
    (void)apply(
        edit::EditCommand{.operation = edit::SetTrackAudioMixCommand{.sequence_id = sequence->id,
                                                                     .track_id = track.id,
                                                                     .gain_db = track.audio_gain_db,
                                                                     .pan = pan},
                          .coalescing_key = "mixer:" + track.id.toString() + ":pan"},
        tr("Could not update track pan"));
    return;
  }
  refreshMixerView();
}

void EditorController::addAudioTrackEffect(const int trackIndex, const QString& effectType) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || trackIndex < 0 || !effectType.startsWith(QStringLiteral("audio."))) {
    refreshMixerView();
    return;
  }
  int audioIndex = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio || audioIndex++ != trackIndex) {
      continue;
    }
    edit::Effect effect;
    effect.type = effectType.toStdString();
    const auto add = [&effect](const char* id, const double value) {
      effect.parameters.emplace(id,
                                edit::EffectParameter{.id = id, .value = value, .keyframes = {}});
    };
    if (effectType == QStringLiteral("audio.eq")) {
      add("frequency_hz", 1'000.0);
      add("quality", 1.0);
      add("gain_db", 0.0);
    } else if (effectType == QStringLiteral("audio.compressor")) {
      add("threshold_db", -18.0);
      add("ratio", 4.0);
      add("attack_ms", 10.0);
      add("release_ms", 100.0);
      add("makeup_db", 0.0);
    } else if (effectType == QStringLiteral("audio.dialogue_denoise")) {
      add("strength", 0.5);
      add("threshold_db", -45.0);
    } else if (effectType == QStringLiteral("audio.limiter")) {
      add("ceiling_db", -1.0);
    } else {
      return;
    }
    (void)applyTrackCommand(
        edit::EditCommand{.operation = edit::AddTrackEffectCommand{.sequence_id = sequence->id,
                                                                   .track_id = track.id,
                                                                   .effect = std::move(effect)},
                          .coalescing_key = {}},
        tr("Could not add track effect"));
    return;
  }
  refreshMixerView();
}

void EditorController::removeAudioTrackEffect(const int trackIndex, const QString& effectId) {
  const edit::Sequence* sequence = currentSequence();
  const auto parsed = parseId(effectId);
  if (sequence == nullptr || trackIndex < 0 || !parsed.has_value()) {
    refreshMixerView();
    return;
  }
  int audioIndex = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio || audioIndex++ != trackIndex) {
      continue;
    }
    (void)applyTrackCommand(
        edit::EditCommand{.operation = edit::RemoveTrackEffectCommand{.sequence_id = sequence->id,
                                                                      .track_id = track.id,
                                                                      .effect_id = *parsed},
                          .coalescing_key = {}},
        tr("Could not remove track effect"));
    return;
  }
  refreshMixerView();
}

void EditorController::updateAudioTrackEffectParameter(const int trackIndex,
                                                       const QString& effectId,
                                                       const QString& parameterId,
                                                       const QVariant& value) {
  const edit::Sequence* sequence = currentSequence();
  const auto parsed = parseId(effectId);
  if (sequence == nullptr || trackIndex < 0 || !parsed.has_value()) {
    refreshMixerView();
    return;
  }
  int audioIndex = 0;
  for (const edit::Track& track : sequence->tracks) {
    if (track.kind != edit::TrackKind::Audio || audioIndex++ != trackIndex) {
      continue;
    }
    const auto effect =
        std::find_if(track.effects.cbegin(), track.effects.cend(),
                     [&parsed](const edit::Effect& candidate) { return candidate.id == *parsed; });
    if (effect == track.effects.cend()) {
      return;
    }
    const auto parameter = effect->parameters.find(parameterId.toStdString());
    if (parameter == effect->parameters.end()) {
      return;
    }
    edit::EffectParameter updated = parameter->second;
    updated.value = effectValueFromUi(updated.value, value);
    (void)applyTrackCommand(
        edit::EditCommand{.operation =
                              edit::SetTrackEffectParameterCommand{.sequence_id = sequence->id,
                                                                   .track_id = track.id,
                                                                   .effect_id = effect->id,
                                                                   .parameter = std::move(updated)},
                          .coalescing_key = "track-effect:" + effectId.toStdString() + ":" +
                                            parameterId.toStdString()},
        tr("Could not update track effect"));
    return;
  }
  refreshMixerView();
}

void EditorController::analyzeLoudnessNormalization() {
  if (normalization_watcher_.isRunning()) {
    return;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    window_.audioMixer()->setNormalizationStatus(tr("Create or open a sequence first."));
    return;
  }
  auto snapshotResult = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshotResult) {
    window_.audioMixer()->setNormalizationStatus(tr("Could not snapshot the current revision."));
    return;
  }
  normalization_stop_source_ = std::stop_source{};
  auto snapshot = std::make_shared<edit::TimelineSnapshot>(std::move(snapshotResult).value());
  auto originals = audio_registry_;
  const auto reviewRevision = editor_->revision();
  const double targetLufs = normalization_target_lufs_;
  active_normalization_generation_ = normalization_completion_gate_.begin();
  const auto generation = active_normalization_generation_;
  window_.audioMixer()->setNormalizationBusy(true);
  normalization_review_ = {};
  const std::stop_token cancellation = normalization_stop_source_.get_token();
  normalization_future_ = QtConcurrent::run([snapshot, originals, reviewRevision, targetLufs,
                                             generation, cancellation] {
    NormalizationReview review;
    review.revision = reviewRevision;
    const auto result =
        audio_render::compute_normalization_gain(*snapshot, originals, targetLufs, cancellation);
    if (!result) {
      review.error = QString::fromStdString(result.error().message);
      return review;
    }
    review.valid = true;
    review.measured_lufs = result.value().integrated_lufs;
    review.gain_db = result.value().gain_db;
    review.target_lufs = targetLufs;
    const auto& analyzedSequence = snapshot->sequence();
    const bool hasSolo = std::any_of(analyzedSequence.tracks.cbegin(),
                                     analyzedSequence.tracks.cend(), [](const edit::Track& track) {
                                       return track.kind == edit::TrackKind::Audio && track.solo;
                                     });
    const auto contributes = [hasSolo](const edit::Track& track) {
      return track.kind == edit::TrackKind::Audio && !track.muted && (!hasSolo || track.solo) &&
             std::any_of(track.clips.cbegin(), track.clips.cend(),
                         [](const edit::Clip& clip) { return clip.kind == edit::ClipKind::Audio; });
    };
    const auto unsafe =
        std::find_if(analyzedSequence.tracks.cbegin(), analyzedSequence.tracks.cend(),
                     [&](const edit::Track& track) {
                       const double adjusted = track.audio_gain_db + review.gain_db;
                       return contributes(track) && (adjusted < -96.0 || adjusted > 24.0);
                     });
    if (unsafe != analyzedSequence.tracks.cend()) {
      review.valid = false;
      review.error =
          QObject::tr("The proposed gain would put %1 outside the safe track range (−96 to +24 "
                      "dB).")
              .arg(QString::fromStdString(unsafe->name));
    }
    Q_UNUSED(generation)
    return review;
  });
  normalization_watcher_.setFuture(normalization_future_);
  SessionEventLog::instance().log_backend("analyzeLoudnessNormalization", "start");
}

void EditorController::applyLoudnessNormalization() {
  if (!normalization_review_.valid) {
    return;
  }
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || normalization_review_.revision != editor_->revision()) {
    normalization_review_.valid = false;
    window_.audioMixer()->setNormalizationStatus(tr("The timeline changed; analyze again."));
    return;
  }
  std::vector<edit::EditCommand> commands;
  const bool hasSolo =
      std::any_of(sequence->tracks.cbegin(), sequence->tracks.cend(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Audio && track.solo;
      });
  for (const edit::Track& track : sequence->tracks) {
    const bool hasAudioClip =
        std::any_of(track.clips.cbegin(), track.clips.cend(),
                    [](const edit::Clip& clip) { return clip.kind == edit::ClipKind::Audio; });
    if (track.kind != edit::TrackKind::Audio || track.muted || (hasSolo && !track.solo) ||
        !hasAudioClip) {
      continue;
    }
    const double adjustedGain = track.audio_gain_db + normalization_review_.gain_db;
    if (adjustedGain < -96.0 || adjustedGain > 24.0) {
      normalization_review_.valid = false;
      window_.audioMixer()->setNormalizationStatus(
          tr("Track gain changed; analyze loudness again."));
      return;
    }
    commands.push_back(
        edit::EditCommand{.operation = edit::SetTrackAudioMixCommand{.sequence_id = sequence->id,
                                                                     .track_id = track.id,
                                                                     .gain_db = adjustedGain,
                                                                     .pan = track.audio_pan},
                          .coalescing_key = {}});
  }
  if (commands.empty() || !applyBatch(std::move(commands), tr("Could not apply normalization"))) {
    return;
  }
  normalization_review_.valid = false;
  window_.audioMixer()->setNormalizationStatus(tr("Normalization applied as one undoable edit."));
}

void EditorController::selectAudioOutputDevice(const QString& deviceId) {
  selected_audio_device_id_ = deviceId;
  audio_recovery_pending_ = false;
  QSettings settings;
  settings.setValue(QStringLiteral("audio/outputDeviceId"), deviceId);
  settings.sync();
  window_.showTransientMessage(tr("Audio output changes apply on the next playback start."));
}

void EditorController::setNormalizationTarget(const double targetLufs) {
  normalization_completion_gate_.invalidate();
  normalization_target_lufs_ = std::clamp(targetLufs, -24.0, -9.0);
  QSettings settings;
  settings.setValue(QStringLiteral("audio/normalizationTargetLufs"), normalization_target_lufs_);
  settings.sync();
  normalization_review_.valid = false;
  window_.audioMixer()->setNormalizationStatus(tr("Target changed; analyze again."));
}

void EditorController::refreshAudioDevices() {
  if (audio_devices_watcher_.isRunning()) {
    return;
  }
  audio_devices_future_ = QtConcurrent::run([] {
    audio::MiniaudioDeviceEnumerator enumerator;
    return enumerator.enumerate();
  });
  audio_devices_watcher_.setFuture(audio_devices_future_);
}

void EditorController::persistSnapshot(const std::string_view reason) {
  const auto project = editor_->projectAt(editor_->revision());
  const project_codec::ProjectBytes bytes = project_codec::serialize_project(*project);
  const auto metadata = store_->metadata();
  store_->append_command("project.snapshot.v3", std::span<const std::byte>(bytes),
                         metadata.head_revision, project_codec::kCurrentSchemaVersion);
  store_->update_heartbeat();
  (void)reason;
}

bool EditorController::apply(edit::EditCommand command, const QString& failureContext) {
  const std::string operation = edit::commandName(command);
  const auto result = editor_->apply(std::move(command), editor_->revision());
  if (!result) {
    SessionEventLog::instance().log_backend("apply", "failure op=" + operation);
    window_.showTransientMessage(QStringLiteral("%1: %2").arg(
        failureContext, QString::fromStdString(result.error().message)));
    return false;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("edit.command");
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    Q_UNUSED(rollback);
    showError(tr("Project write failed"), QString::fromUtf8(exception.what()));
    refreshViews();
    return false;
  }
  setDirty(true);
  refreshViews();
  SessionEventLog::instance().log_backend("apply", "success op=" + operation);
  return true;
}

bool EditorController::applyBatch(std::vector<edit::EditCommand> commands,
                                  const QString& failureContext) {
  if (commands.empty()) {
    return true;
  }
  const std::size_t command_count = commands.size();
  const std::string batch_key = "batch:" + edit::EntityId::generate().toString();
  for (edit::EditCommand& command : commands) {
    command.coalescing_key = batch_key;
  }
  const auto result =
      editor_->applyBatch(std::move(commands), editor_->revision(), "Timeline batch", batch_key);
  if (!result) {
    SessionEventLog::instance().log_backend("applyBatch",
                                            "failure count=" + std::to_string(command_count));
    window_.showTransientMessage(QStringLiteral("%1: %2").arg(
        failureContext, QString::fromStdString(result.error().message)));
    refreshViews();
    return false;
  }
  stopAudioPlayback();
  playback_timer_.stop();
  playback_rate_ = 0.0;
  try {
    persistSnapshot("edit.batch");
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    (void)rollback;
    showError(tr("Project write failed"), QString::fromUtf8(exception.what()));
    refreshViews();
    return false;
  }
  setDirty(true);
  refreshViews();
  SessionEventLog::instance().log_backend("applyBatch",
                                          "success count=" + std::to_string(command_count));
  return true;
}

std::uint32_t EditorController::timelineTimeScale(const edit::Sequence& sequence) const {
  const std::uint64_t base = static_cast<std::uint64_t>(kUiTimescale);
  const std::uint64_t numerator = std::max<std::uint32_t>(1U, sequence.frame_rate.numerator());
  const std::uint64_t divisor = std::gcd(base, numerator);
  const std::uint64_t scale = (base / divisor) * numerator;
  return scale > std::numeric_limits<std::uint32_t>::max()
             ? static_cast<std::uint32_t>(kUiTimescale)
             : static_cast<std::uint32_t>(scale);
}

edit::Time EditorController::timelineTime(const qint64 value) const {
  return edit::Time(value, timeline_time_scale_);
}

qint64 EditorController::timelineValue(const edit::Time time) const {
  return static_cast<qint64>(
      time.rescaledTo(timeline_time_scale_, edit::RoundingMode::NearestTiesEven).value());
}

std::vector<edit::EntityId> EditorController::selectedClipIds() const {
  std::vector<edit::EntityId> result;
  result.reserve(selected_clip_ids_.size());
  for (const auto& id : selected_clip_ids_) {
    result.push_back(id);
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<edit::EntityId>
EditorController::expandLinkedSelection(const edit::Sequence& sequence,
                                        const std::vector<edit::EntityId>& clipIds) const {
  std::unordered_set<edit::EntityId> result(clipIds.begin(), clipIds.end());
  for (const auto& id : clipIds) {
    const edit::Clip* clip = edit::findClip(sequence, id);
    if (clip == nullptr || !clip->linked_group.has_value()) {
      continue;
    }
    for (const auto& track : sequence.tracks) {
      for (const auto& candidate : track.clips) {
        if (candidate.linked_group == clip->linked_group) {
          result.insert(candidate.id);
        }
      }
    }
  }
  std::vector<edit::EntityId> ordered(result.begin(), result.end());
  std::sort(ordered.begin(), ordered.end());
  return ordered;
}

void EditorController::pruneTimelineSelection(const edit::Sequence& sequence) {
  for (auto it = selected_clip_ids_.begin(); it != selected_clip_ids_.end();) {
    if (edit::findClip(sequence, *it) == nullptr) {
      it = selected_clip_ids_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_clip_id_.has_value() && !selected_clip_ids_.contains(*active_clip_id_)) {
    active_clip_id_.reset();
  }
  if (!active_clip_id_.has_value() && !selected_clip_ids_.empty()) {
    active_clip_id_ = *std::min_element(selected_clip_ids_.begin(), selected_clip_ids_.end());
  }
  if (selected_marker_id_.has_value() &&
      std::none_of(
          sequence.markers.begin(), sequence.markers.end(),
          [this](const edit::Marker& marker) { return marker.id == *selected_marker_id_; })) {
    selected_marker_id_.reset();
  }
  if (!selected_gap_key_.isEmpty()) {
    bool still_current = false;
    const auto snapshot = editor_->snapshot(sequence.id, editor_->revision());
    if (snapshot) {
      const edit::Time limit = edit::sequenceDuration(sequence);
      for (const auto& track : sequence.tracks) {
        for (const auto& gap : snapshot.value().gaps(track.id, limit)) {
          if (gapKey(track.id, gap.timeline_range) == selected_gap_key_) {
            still_current = true;
            break;
          }
        }
        if (still_current) {
          break;
        }
      }
    }
    if (!still_current) {
      selected_gap_key_.clear();
    }
  }
}

void EditorController::setClipSelection(const QStringList& clipIds, const QString& activeClipId) {
  selected_clip_ids_.clear();
  for (const auto& text : clipIds) {
    if (const auto id = parseId(text); id.has_value()) {
      selected_clip_ids_.insert(*id);
    }
  }
  active_clip_id_ = parseId(activeClipId);
  if (!active_clip_id_.has_value() || !selected_clip_ids_.contains(*active_clip_id_)) {
    active_clip_id_ = selected_clip_ids_.empty()
                          ? std::nullopt
                          : std::optional<edit::EntityId>(*std::min_element(
                                selected_clip_ids_.begin(), selected_clip_ids_.end()));
  }
  selected_marker_id_.reset();
  selected_gap_key_.clear();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::selectMarker(const QString& markerId) {
  selected_marker_id_ = parseId(markerId);
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_gap_key_.clear();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::selectGap(const QString& gapKey) {
  selected_gap_key_ = gapKey;
  selected_clip_ids_.clear();
  active_clip_id_.reset();
  selected_marker_id_.reset();
  refreshTimelineView();
  refreshInspectorView();
}

void EditorController::nudgeTimelineSelection(const QStringList& clipIds, const int frameCount,
                                              const int editIntent) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || frameCount == 0) {
    return;
  }
  const QString prior_active =
      active_clip_id_.has_value() ? QString::fromStdString(active_clip_id_->toString()) : QString{};
  const QString active = !prior_active.isEmpty() && clipIds.contains(prior_active)
                             ? prior_active
                             : (clipIds.isEmpty() ? QString{} : clipIds.front());
  setClipSelection(clipIds, active);
  const auto selection = selectedClipIds();
  if (selection.empty()) {
    return;
  }
  const auto intent = static_cast<desktop_ui::TimelineWidget::EditIntent>(editIntent);
  const edit::InsertMode mode = intent == desktop_ui::TimelineWidget::EditIntent::Ripple
                                    ? edit::InsertMode::Ripple
                                : intent == desktop_ui::TimelineWidget::EditIntent::Overwrite
                                    ? edit::InsertMode::Overwrite
                                    : edit::InsertMode::RejectOverlap;
  std::unordered_set<edit::EntityId> consumed;
  std::vector<edit::EditCommand> commands;
  for (const auto& id : selection) {
    if (consumed.contains(id)) {
      continue;
    }
    const edit::Clip* clip = edit::findClip(*sequence, id);
    if (clip == nullptr) {
      continue;
    }
    const auto linked = expandLinkedSelection(*sequence, {id});
    consumed.insert(linked.begin(), linked.end());
    const auto track =
        std::find_if(sequence->tracks.begin(), sequence->tracks.end(), [&id](const auto& item) {
          return std::any_of(item.clips.begin(), item.clips.end(),
                             [&id](const auto& candidate) { return candidate.id == id; });
        });
    if (track == sequence->tracks.end()) {
      continue;
    }
    // Nudge is a translation, not a re-quantization. In particular, a clip
    // deliberately placed between frames must retain that offset and every
    // linked group receives the same rational frame delta.
    const edit::Time target_start =
        clip->timeline_range.start + sequence->frame_rate.frameTime(frameCount);
    if (target_start.isNegative()) {
      window_.showTransientMessage(tr("Cannot nudge the selected clips before the timeline start"));
      return;
    }
    commands.push_back({.operation = edit::MoveClipCommand{.sequence_id = sequence->id,
                                                           .clip_id = id,
                                                           .destination_track_id = track->id,
                                                           .new_start = target_start,
                                                           .mode = mode,
                                                           .include_linked = linked.size() > 1},
                        .coalescing_key = {}});
  }
  (void)applyBatch(std::move(commands), tr("Could not nudge the timeline selection"));
}

bool EditorController::applyTrackCommand(edit::EditCommand command, const QString& failureContext) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return false;
  }
  return apply(std::move(command), failureContext);
}

void EditorController::addTrack(const int trackKind) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    return;
  }
  const auto kind = static_cast<desktop_ui::TrackKind>(trackKind);
  edit::Track track;
  track.kind = kind == desktop_ui::TrackKind::Audio     ? edit::TrackKind::Audio
               : kind == desktop_ui::TrackKind::Caption ? edit::TrackKind::Caption
                                                        : edit::TrackKind::Video;
  const char* prefix = track.kind == edit::TrackKind::Audio     ? "Audio"
                       : track.kind == edit::TrackKind::Caption ? "Captions"
                                                                : "Video";
  const auto ordinal =
      1 + std::count_if(sequence->tracks.begin(), sequence->tracks.end(),
                        [&track](const auto& item) { return item.kind == track.kind; });
  track.name = std::string(prefix) + " " + std::to_string(ordinal);
  (void)applyTrackCommand({.operation = edit::AddTrackCommand{.sequence_id = sequence->id,
                                                              .track = std::move(track),
                                                              .index = std::nullopt},
                           .coalescing_key = {}},
                          tr("Could not add a track"));
}

void EditorController::renameTrack(const QString& trackId, const QString& name) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value()) {
    return;
  }
  (void)applyTrackCommand(
      {.operation = edit::RenameTrackCommand{.sequence_id = sequence->id,
                                             .track_id = *id,
                                             .name = name.trimmed().toStdString()},
       .coalescing_key = {}},
      tr("Could not rename the track"));
}

void EditorController::reorderTrack(const QString& trackId, const int destinationIndex) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value() || destinationIndex < 0) {
    return;
  }
  (void)applyTrackCommand(
      {.operation = edit::ReorderTrackCommand{.sequence_id = sequence->id,
                                              .track_id = *id,
                                              .index = static_cast<std::size_t>(destinationIndex)},
       .coalescing_key = {}},
      tr("Could not reorder the track"));
}

void EditorController::setTrackLocked(const QString& trackId, const bool locked) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackLockedCommand{.sequence_id = sequence->id,
                                                                    .track_id = *id,
                                                                    .locked = locked},
                           .coalescing_key = {}},
                          tr("Could not change the track lock"));
}

void EditorController::setTrackVisible(const QString& trackId, const bool visible) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackVisibilityCommand{.sequence_id = sequence->id,
                                                                        .track_id = *id,
                                                                        .visible = visible},
                           .coalescing_key = {}},
                          tr("Could not change track visibility"));
}

void EditorController::setTrackTargeted(const QString& trackId, const bool targeted) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand({.operation = edit::SetTrackTargetedCommand{.sequence_id = sequence->id,
                                                                      .track_id = *id,
                                                                      .targeted = targeted},
                           .coalescing_key = {}},
                          tr("Could not change track targeting"));
}

void EditorController::removeTrack(const QString& trackId) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(trackId);
  if (sequence == nullptr || !id.has_value())
    return;
  (void)applyTrackCommand(
      {.operation = edit::RemoveTrackCommand{.sequence_id = sequence->id, .track_id = *id},
       .coalescing_key = {}},
      tr("Could not remove the track"));
}

void EditorController::addMarker(const qint64 start) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr)
    return;
  edit::Marker marker;
  marker.range = edit::TimeRange(timelineTime(std::max<qint64>(0, start)), edit::Time{});
  marker.label = tr("Marker").toStdString();
  if (apply({.operation = edit::AddMarkerCommand{.sequence_id = sequence->id, .marker = marker},
             .coalescing_key = {}},
            tr("Could not add a marker"))) {
    selected_marker_id_ = marker.id;
    selected_clip_ids_.clear();
    active_clip_id_.reset();
  }
}

void EditorController::moveMarker(const QString& markerId, const qint64 start) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  const auto found = std::find_if(sequence->markers.begin(), sequence->markers.end(),
                                  [&id](const auto& marker) { return marker.id == *id; });
  if (found == sequence->markers.end()) {
    window_.showTransientMessage(tr("The marker no longer exists"));
    refreshTimelineView();
    return;
  }
  auto marker = *found;
  marker.range.start = timelineTime(std::max<qint64>(0, start));
  (void)apply({.operation = edit::UpdateMarkerCommand{.sequence_id = sequence->id,
                                                      .marker = std::move(marker)},
               .coalescing_key = {}},
              tr("Could not move the marker"));
}

void EditorController::renameMarker(const QString& markerId, const QString& name) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  const auto found = std::find_if(sequence->markers.begin(), sequence->markers.end(),
                                  [&id](const auto& marker) { return marker.id == *id; });
  if (found == sequence->markers.end())
    return;
  auto marker = *found;
  marker.label = name.trimmed().toStdString();
  (void)apply({.operation = edit::UpdateMarkerCommand{.sequence_id = sequence->id,
                                                      .marker = std::move(marker)},
               .coalescing_key = {}},
              tr("Could not rename the marker"));
}

void EditorController::removeMarker(const QString& markerId) {
  const edit::Sequence* sequence = currentSequence();
  const auto id = parseId(markerId);
  if (sequence == nullptr || !id.has_value())
    return;
  if (apply({.operation = edit::RemoveMarkerCommand{.sequence_id = sequence->id, .marker_id = *id},
             .coalescing_key = {}},
            tr("Could not remove the marker"))) {
    selected_marker_id_.reset();
  }
}

void EditorController::closeGap(const QString& gapKeyText) {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || gapKeyText.isEmpty()) {
    return;
  }
  const auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    window_.showTransientMessage(tr("Could not resolve the current timeline gaps"));
    refreshTimelineView();
    return;
  }
  const edit::TimelineSnapshot snapshot = snapshot_result.value();
  const edit::Time limit = edit::sequenceDuration(*sequence);
  for (const auto& track : sequence->tracks) {
    for (const auto& gap : snapshot.gaps(track.id, limit)) {
      if (gapKey(track.id, gap.timeline_range) != gapKeyText) {
        continue;
      }
      if (apply({.operation = edit::CloseGapCommand{.sequence_id = sequence->id,
                                                    .track_id = track.id,
                                                    .gap = gap.timeline_range},
                 .coalescing_key = {}},
                tr("Could not close the gap"))) {
        selected_gap_key_.clear();
      }
      return;
    }
  }
  window_.showTransientMessage(tr("That gap changed before it could be closed"));
  selected_gap_key_.clear();
  refreshTimelineView();
}

void EditorController::refreshViews() {
  if (!editor_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  window_.setProjectDisplayName(QString::fromStdString(project->name));
  const edit::Sequence* sequence = currentSequence();
  window_.deliverPanel()->setExportEnabled(
      export_in_flight_ || (sequence != nullptr && !edit::sequenceDuration(*sequence).isZero()));
  refreshMediaView();
  refreshTimelineView();
  refreshInspectorView();
  refreshMixerView();
  refreshCaptionView();
  requestPreview();
}

void EditorController::refreshMediaView() {
  const auto project = editor_->projectAt(editor_->revision());
  QVector<MediaItemView> items;
  items.reserve(static_cast<qsizetype>(project->assets.size()));
  for (const edit::Asset& asset : project->assets) {
    const auto container = asset.metadata.find("container");
    QString format =
        container == asset.metadata.end() ? QString{} : QString::fromStdString(container->second);
    if (asset.has_video) {
      format += QStringLiteral(" %1×%2").arg(asset.width).arg(asset.height);
    }
    const auto* record = findImported(imported_assets_, asset.id.toString());
    const bool proxy_available =
        record != nullptr && record->proxy.has_value() && record->proxy->complete;
    const bool proxy_recommended =
        record != nullptr && assets::AssetService::should_recommend_proxy(*record);
    const auto title = media_metadata_titles_.find(asset.id.toString());
    const auto thumbnail = media_thumbnails_.find(asset.id.toString());
    items.push_back({
        .id = QString::fromStdString(asset.id.toString()),
        .displayName = QString::fromStdString(asset.name),
        .filePath = qStringFromPath(pathFromUtf8String(asset.source_uri)),
        .durationText = durationText(asset.duration),
        .formatText = format.trimmed(),
        .metadataTitle = title == media_metadata_titles_.end() ? QString{} : title->second,
        .thumbnail = thumbnail == media_thumbnails_.end() ? QImage{} : thumbnail->second,
        .offline = record == nullptr || record->availability == assets::AssetAvailability::Missing,
        .contentChanged =
            record != nullptr && record->availability == assets::AssetAvailability::Changed,
        .proxyAvailable = proxy_available,
        .proxyRecommended = proxy_recommended,
        .proxyGenerating = proxy_jobs_.contains(asset.id.toString()),
    });
  }
  window_.setMediaItems(items);
}

void EditorController::refreshTimelineView() {
  const auto project = editor_->projectAt(editor_->revision());
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr) {
    window_.setTimelineView(kUiTimescale * 10, kUiTimescale, {}, {});
    return;
  }
  timeline_time_scale_ = timelineTimeScale(*sequence);
  pruneTimelineSelection(*sequence);
  QVector<TimelineTrackView> tracks;
  QVector<TimelineClipView> clips;
  QVector<desktop_ui::TimelineMarkerView> markers;
  QVector<desktop_ui::TimelineGapView> gaps;
  tracks.reserve(static_cast<qsizetype>(sequence->tracks.size()));
  markers.reserve(static_cast<qsizetype>(sequence->markers.size()));
  std::size_t track_index = 0;
  for (const edit::Track& track : sequence->tracks) {
    tracks.push_back({
        .id = QString::fromStdString(track.id.toString()),
        .displayName = QString::fromStdString(track.name),
        .kind = uiTrackKind(track.kind),
        .muted = track.muted,
        .soloed = track.solo,
        .locked = track.locked,
        .visible = track.visible,
        .targeted = track.targeted,
    });
    for (const edit::Clip& clip : track.clips) {
      const auto* record = findImported(imported_assets_, clip.asset_id.toString());
      const auto waveform = media_waveforms_.find(clip.asset_id.toString());
      clips.push_back({
          .id = QString::fromStdString(clip.id.toString()),
          .displayName = QString::fromStdString(clip.name),
          .trackIndex = static_cast<int>(track_index),
          .start = timelineValue(clip.timeline_range.start),
          .duration = std::max<qint64>(1, timelineValue(clip.timeline_range.duration)),
          .color = colorForTrack(track.kind, track_index),
          .selected = selected_clip_ids_.contains(clip.id),
          .offline = record == nullptr || record->availability == assets::AssetAvailability::Missing,
          .proxy = record != nullptr && record->proxy.has_value() && record->proxy->complete,
          .waveform = waveform == media_waveforms_.end()
                          ? QVector<desktop_ui::WaveformBucketView>{}
                          : waveform->second,
      });
    }
    ++track_index;
  }
  for (const auto& marker : sequence->markers) {
    markers.push_back(
        {.id = QString::fromStdString(marker.id.toString()),
         .displayName = QString::fromStdString(marker.label),
         .start = timelineValue(marker.range.start),
         .duration = timelineValue(marker.range.duration),
         .color = QColor::fromRgbF(
             static_cast<float>(marker.color.red), static_cast<float>(marker.color.green),
             static_cast<float>(marker.color.blue), static_cast<float>(marker.color.alpha)),
         .selected = selected_marker_id_.has_value() && marker.id == *selected_marker_id_});
  }
  const auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (snapshot_result) {
    const edit::Time limit = edit::sequenceDuration(*sequence);
    const auto snapshot = snapshot_result.value();
    for (std::size_t index = 0; index < sequence->tracks.size(); ++index) {
      const auto& track = sequence->tracks[index];
      for (const auto& gap : snapshot.gaps(track.id, limit)) {
        const QString key = gapKey(track.id, gap.timeline_range);
        gaps.push_back({.key = key,
                        .trackId = QString::fromStdString(track.id.toString()),
                        .trackIndex = static_cast<int>(index),
                        .start = timelineValue(gap.timeline_range.start),
                        .duration = timelineValue(gap.timeline_range.duration),
                        .selected = key == selected_gap_key_});
      }
    }
  }
  const qint64 duration = std::max<qint64>(timelineValue(edit::sequenceDuration(*sequence)),
                                           static_cast<qint64>(timeline_time_scale_) * 10);
  window_.setTimelineView(duration, timeline_time_scale_, std::move(tracks), std::move(clips),
                          std::move(markers), std::move(gaps));
  window_.timeline()->setSnapResolver([this](const desktop_ui::TimelineSnapRequest& request) {
    const edit::Sequence* current = currentSequence();
    if (current == nullptr) {
      return desktop_ui::TimelineSnapResult{
          .time = request.proposedTime, .kind = desktop_ui::TimelineSnapKind::None, .label = {}};
    }
    edit::SnapRequest snap_request;
    snap_request.proposed_time = timelineTime(request.proposedTime);
    const double pixels_per_second = std::max(1.0, window_.timeline()->pixelsPerSecond());
    const auto threshold_units = static_cast<std::int64_t>(
        std::ceil(static_cast<double>(std::max(0, request.thresholdPixels)) / pixels_per_second *
                  static_cast<double>(timeline_time_scale_)));
    snap_request.threshold =
        edit::Time(std::max<std::int64_t>(0, threshold_units), timeline_time_scale_);
    snap_request.playhead = playheadTime();
    for (const auto& text : request.excludedClipIds) {
      if (const auto id = parseId(text); id.has_value()) {
        snap_request.excluded_clip_ids.insert(*id);
      }
    }
    if (const auto marker_id = parseId(request.excludedMarkerId); marker_id.has_value()) {
      snap_request.excluded_marker_ids.insert(*marker_id);
    }
    const auto candidate = edit::nearestSnapCandidate(*current, snap_request);
    if (!candidate.has_value()) {
      return desktop_ui::TimelineSnapResult{
          .time = request.proposedTime, .kind = desktop_ui::TimelineSnapKind::None, .label = {}};
    }
    const auto kind = [candidate] {
      switch (candidate->kind) {
      case edit::SnapTargetKind::Playhead:
        return desktop_ui::TimelineSnapKind::Playhead;
      case edit::SnapTargetKind::Marker:
        return desktop_ui::TimelineSnapKind::Marker;
      case edit::SnapTargetKind::ClipEdge:
        return desktop_ui::TimelineSnapKind::ClipEdge;
      case edit::SnapTargetKind::FrameGrid:
      default:
        return desktop_ui::TimelineSnapKind::Frame;
      }
    }();
    return desktop_ui::TimelineSnapResult{
        .time = timelineValue(candidate->time),
        .kind = kind,
        .label = QString::fromStdString(candidate->entity_id.has_value()
                                            ? candidate->entity_id->toString()
                                            : candidate->time.toString())};
  });
  window_.timeline()->setFrameRate(sequence->frame_rate.numerator(),
                                   sequence->frame_rate.denominator());
  window_.timeline()->setPlayhead(timelineValue(playheadTime()));
  window_.programViewer()->setTimecode(timecodeText(playhead_, sequence->frame_rate));
}

void EditorController::refreshInspectorView() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !active_clip_id_.has_value()) {
    window_.inspector()->clearSelection();
    if (!selected_media_id_.isEmpty()) {
      presentAssetMetadata(selected_media_id_);
    } else {
      window_.inspector()->clearAssetMetadata();
    }
    return;
  }
  const edit::Clip* clip = edit::findClip(*sequence, *active_clip_id_);
  if (clip == nullptr) {
    selected_clip_ids_.clear();
    active_clip_id_.reset();
    window_.inspector()->clearSelection();
    if (!selected_media_id_.isEmpty()) {
      presentAssetMetadata(selected_media_id_);
    } else {
      window_.inspector()->clearAssetMetadata();
    }
    return;
  }

  window_.inspector()->setSelectionName(QString::fromStdString(clip->name));
  window_.inspector()->setClipCapabilities(clip->kind == edit::ClipKind::Video ||
                                               clip->kind == edit::ClipKind::Title,
                                           clip->kind == edit::ClipKind::Audio);
  window_.inspector()->setTitleControlsVisible(clip->kind == edit::ClipKind::Title);
  window_.inspector()->setSpeedControlsVisible(clip->kind == edit::ClipKind::Video ||
                                               clip->kind == edit::ClipKind::Audio);
  if (clip->kind == edit::ClipKind::Title && clip->title.has_value()) {
    const edit::Title& title = *clip->title;
    window_.inspector()->setParameter(QStringLiteral("titleText"),
                                      QString::fromStdString(title.text));
    window_.inspector()->setParameter(QStringLiteral("titleFont"),
                                      QString::fromStdString(title.font_family));
    window_.inspector()->setParameter(QStringLiteral("titleSize"), title.font_size);
    const QString align = title.horizontal_alignment == edit::TitleHorizontalAlignment::Left
                              ? QStringLiteral("left")
                          : title.horizontal_alignment == edit::TitleHorizontalAlignment::Right
                              ? QStringLiteral("right")
                              : QStringLiteral("center");
    window_.inspector()->setParameter(QStringLiteral("titleAlign"), align);
    window_.inspector()->setParameter(QStringLiteral("titleBold"), title.bold);
    window_.inspector()->setParameter(QStringLiteral("titleItalic"), title.italic);
  }
  // Speed is shown as a percentage of normal (100 = 1x).
  const double speed_percent = static_cast<double>(clip->playback_rate.numerator()) * 100.0 /
                               static_cast<double>(clip->playback_rate.denominator());
  selected_speed_percent_ = speed_percent;
  window_.inspector()->setParameter(QStringLiteral("speed"), speed_percent);
  window_.inspector()->setParameter(QStringLiteral("reverse"), clip->reversed);
  window_.inspector()->setParameter(QStringLiteral("positionX"), clip->transform.position.x);
  window_.inspector()->setParameter(QStringLiteral("positionY"), clip->transform.position.y);
  window_.inspector()->setParameter(QStringLiteral("scale"), clip->transform.scale.x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("scaleX"), clip->transform.scale.x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("scaleY"), clip->transform.scale.y * 100.0);
  window_.inspector()->setParameter(QStringLiteral("rotation"), clip->transform.rotation_degrees);
  window_.inspector()->setParameter(QStringLiteral("opacity"), clip->transform.opacity * 100.0);
  window_.inspector()->setParameter(QStringLiteral("anchorX"), clip->transform.anchor_x * 100.0);
  window_.inspector()->setParameter(QStringLiteral("anchorY"), clip->transform.anchor_y * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropLeft"), clip->transform.crop_left * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropTop"), clip->transform.crop_top * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropRight"),
                                    clip->transform.crop_right * 100.0);
  window_.inspector()->setParameter(QStringLiteral("cropBottom"),
                                    clip->transform.crop_bottom * 100.0);
  const QString blend_mode = [clip] {
    switch (clip->blend_mode) {
    case edit::BlendMode::Add:
      return QStringLiteral("add");
    case edit::BlendMode::Multiply:
      return QStringLiteral("multiply");
    case edit::BlendMode::Screen:
      return QStringLiteral("screen");
    case edit::BlendMode::Overlay:
      return QStringLiteral("overlay");
    case edit::BlendMode::Normal:
    default:
      return QStringLiteral("normal");
    }
  }();
  window_.inspector()->setParameter(QStringLiteral("blendMode"), blend_mode);
  window_.inspector()->setParameter(QStringLiteral("audioGain"), clip->audio_gain_db);
  window_.inspector()->setParameter(QStringLiteral("audioPan"), clip->audio_pan * 100.0);
  window_.inspector()->setParameter(QStringLiteral("fadeIn"),
                                    static_cast<double>(clip->fade_in.value()) /
                                        static_cast<double>(clip->fade_in.timescale()));
  window_.inspector()->setParameter(QStringLiteral("fadeOut"),
                                    static_cast<double>(clip->fade_out.value()) /
                                        static_cast<double>(clip->fade_out.timescale()));
  QVector<desktop_ui::EffectParameterView> effect_parameters;
  for (const auto& effect : clip->effects) {
    const QString effect_id = QString::fromStdString(effect.id.toString());
    const QString effect_name = effectDisplayName(effect.type);
    for (const auto& [parameter_id, parameter] : effect.parameters) {
      desktop_ui::EffectParameterView view;
      view.effectId = effect_id;
      view.effectName = effect_name;
      view.parameterId = QString::fromStdString(parameter_id);
      view.displayName = view.parameterId;
      view.value = effectValueForUi(parameter.value);
      view.duration = toUiTime(clip->timeline_range.duration);
      view.keyframes.reserve(static_cast<qsizetype>(parameter.keyframes.size()));
      for (const auto& keyframe : parameter.keyframes) {
        const auto interpolation = [&keyframe] {
          switch (keyframe.interpolation) {
          case edit::KeyframeInterpolation::Hold:
            return desktop_ui::KeyframeInterpolationView::Hold;
          case edit::KeyframeInterpolation::Bezier:
            return desktop_ui::KeyframeInterpolationView::Bezier;
          case edit::KeyframeInterpolation::Linear:
          default:
            return desktop_ui::KeyframeInterpolationView::Linear;
          }
        }();
        const auto value = [&keyframe] {
          if (const auto* number = std::get_if<double>(&keyframe.value)) {
            return *number;
          }
          if (const auto* integer = std::get_if<std::int64_t>(&keyframe.value)) {
            return static_cast<double>(*integer);
          }
          return 0.0;
        }();
        view.keyframes.push_back(
            {.id = QString::fromStdString(keyframe.id.toString()),
             .time = toUiTime(keyframe.time),
             .value = value,
             .interpolation = interpolation,
             .incomingControl = QPointF{keyframe.incoming_control.x, keyframe.incoming_control.y},
             .outgoingControl = QPointF{keyframe.outgoing_control.x, keyframe.outgoing_control.y}});
      }
      effect_parameters.push_back(std::move(view));
    }
  }
  window_.inspector()->setEffectParameters(effect_parameters);
  presentAssetMetadata(QString::fromStdString(clip->asset_id.toString()));
}

void EditorController::refreshMixerView() {
  QVector<desktop_ui::AudioTrackView> tracks;
  const edit::Sequence* sequence = currentSequence();
  if (sequence != nullptr) {
    for (const edit::Track& track : sequence->tracks) {
      if (track.kind == edit::TrackKind::Audio) {
        desktop_ui::AudioTrackView view{.id = QString::fromStdString(track.id.toString()),
                                        .displayName = QString::fromStdString(track.name),
                                        .muted = track.muted,
                                        .soloed = track.solo,
                                        .gain_db = track.audio_gain_db,
                                        .pan = track.audio_pan,
                                        .effects = {}};
        for (const auto& effect : track.effects) {
          desktop_ui::AudioTrackView::Effect effectView;
          effectView.id = QString::fromStdString(effect.id.toString());
          effectView.type = QString::fromStdString(effect.type);
          effectView.displayName = effectDisplayName(effect.type);
          for (const auto& [parameterId, parameter] : effect.parameters) {
            effectView.parameters.push_back({.id = QString::fromStdString(parameterId),
                                             .value = effectValueForUi(parameter.value)});
          }
          view.effects.push_back(std::move(effectView));
        }
        tracks.push_back(std::move(view));
      }
    }
  }
  window_.audioMixer()->setTracks(tracks);
}

void EditorController::refreshCaptionView() {
  const edit::Sequence* sequence = currentSequence();
  visible_caption_indices_.clear();
  if (sequence == nullptr) {
    window_.captionsPanel()->setCaptionRows(QVector<desktop_ui::CaptionRowView>{});
    return;
  }

  if (caption_search_.isEmpty()) {
    visible_caption_indices_.reserve(sequence->captions.size());
    for (std::size_t index = 0; index < sequence->captions.size(); ++index) {
      visible_caption_indices_.push_back(index);
    }
  } else {
    const auto document = caption_service::fromEditCaptions(
        sequence->captions, caption_service::SubtitleFormat::WebVtt);
    auto matches = caption_service::search(document, caption_search_.toStdString());
    if (matches) {
      for (const auto& hit : matches.value()) {
        if (visible_caption_indices_.empty() || visible_caption_indices_.back() != hit.cue_index) {
          visible_caption_indices_.push_back(hit.cue_index);
        }
      }
    }
  }

  QVector<desktop_ui::CaptionRowView> rows;
  rows.reserve(static_cast<qsizetype>(visible_caption_indices_.size()));
  for (const std::size_t index : visible_caption_indices_) {
    if (index >= sequence->captions.size()) {
      continue;
    }
    const edit::Caption& caption = sequence->captions[index];
    desktop_ui::CaptionRowView row;
    row.id = QString::fromStdString(caption.id.toString());
    row.timecode = timecodeText(toUiTime(caption.range.start), sequence->frame_rate) +
                   QStringLiteral("  →  ") +
                   timecodeText(toUiTime(caption.range.end()), sequence->frame_rate);
    row.text = QString::fromStdString(caption.text);
    row.language = QString::fromStdString(caption.language);
    row.start = toUiTime(caption.range.start);
    row.end = toUiTime(caption.range.end());
    row.style.fontFamily = QString::fromStdString(caption.style.font_family);
    row.style.fontSize = caption.style.font_size;
    row.style.textColor = QColor::fromRgbF(static_cast<float>(caption.style.text_color.red),
                                           static_cast<float>(caption.style.text_color.green),
                                           static_cast<float>(caption.style.text_color.blue),
                                           static_cast<float>(caption.style.text_color.alpha));
    row.style.backgroundColor =
        QColor::fromRgbF(static_cast<float>(caption.style.background_color.red),
                         static_cast<float>(caption.style.background_color.green),
                         static_cast<float>(caption.style.background_color.blue),
                         static_cast<float>(caption.style.background_color.alpha));
    row.style.bold = caption.style.bold;
    row.style.italic = caption.style.italic;
    row.style.alignment =
        caption.style.alignment == edit::CaptionAlignment::Left
            ? QStringLiteral("left")
            : (caption.style.alignment == edit::CaptionAlignment::Right ? QStringLiteral("right")
                                                                        : QStringLiteral("center"));
    row.style.verticalPosition = caption.style.vertical_position;
    row.style.safeMargin = caption.style.safe_margin;
    row.style.outlineWidth = caption.style.outline_width;
    row.style.outlineColor =
        QColor::fromRgbF(static_cast<float>(caption.style.outline_color.red),
                         static_cast<float>(caption.style.outline_color.green),
                         static_cast<float>(caption.style.outline_color.blue),
                         static_cast<float>(caption.style.outline_color.alpha));
    for (const auto& word : caption.words) {
      row.words.push_back({.id = QString::fromStdString(word.id.toString()),
                           .text = QString::fromStdString(word.text),
                           .start = toUiTime(word.range.start),
                           .end = toUiTime(word.range.end()),
                           .probability = word.probability});
    }
    rows.push_back(std::move(row));
  }
  window_.captionsPanel()->setCaptionRows(rows);
}

void EditorController::rebuildPlaybackRegistry() {
  for (const edit::EntityId& id : registered_playback_assets_) {
    (void)playback_registry_->unregister_asset(id);
    frame_provider_->invalidate(id);
  }
  registered_playback_assets_.clear();
  for (const edit::EntityId& id : registered_audio_assets_) {
    (void)audio_registry_->unregister_asset(id);
  }
  registered_audio_assets_.clear();
  if (!editor_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  for (const edit::Asset& asset : project->assets) {
    const auto* record = findImported(imported_assets_, asset.id.toString());
    playback::AssetPlaybackSources sources{
        .original = {.path = pathFromUtf8String(asset.source_uri), .video_stream_index = -1},
        .proxy = record != nullptr && record->proxy.has_value() && record->proxy->complete
                     ? std::optional<playback::AssetStreamLocation>{
                           playback::AssetStreamLocation{.path = record->proxy->proxy_uri,
                                                         .video_stream_index = -1}}
                     : std::nullopt};
    if (playback_registry_->register_asset(asset.id, std::move(sources))) {
      registered_playback_assets_.push_back(asset.id);
    }
    if (asset.has_audio) {
      if (audio_registry_->register_original(
              asset.id,
              audio_render::OriginalAudioMedia{.path = pathFromUtf8String(asset.source_uri),
                                               .audio_stream_index = -1})) {
        registered_audio_assets_.push_back(asset.id);
      }
    }
  }
}

bool EditorController::startAudioMasterPlayback() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr || !audio::MiniaudioOutputDevice::available()) {
    if (!audio_fallback_announced_) {
      audio_fallback_announced_ = true;
      window_.showTransientMessage(
          tr("The realtime audio-device adapter is unavailable; playback will be silent"), 8'000);
    }
    SessionEventLog::instance().log_backend("startAudioMasterPlayback", "failure unavailable");
    return false;
  }

  if (audio_playback_ != nullptr) {
    const audio::AsyncPlaybackDiagnostics diagnostics = audio_playback_->diagnostics();
    const bool stopped = diagnostics.requested_state == audio::PlaybackState::Stopped &&
                         diagnostics.latest_status != audio::PlaybackCommandStatus::Pending &&
                         diagnostics.playback.state == audio::PlaybackState::Stopped;
    if (!stopped) {
      static_cast<void>(audio_playback_->request_stop());
      audio_master_active_ = false;
      audio_session_stale_ = true;
      audio_control_intent_ = AudioControlIntent::None;
      audio_command_version_ = 0;
      audio_start_pending_ = true;
      SessionEventLog::instance().log_backend("startAudioMasterPlayback", "success restart");
      return true;
    }
    audio_playback_.reset();
  }

  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    SessionEventLog::instance().log_backend("startAudioMasterPlayback", "failure snapshot");
    return false;
  }

  try {
    edit::TimelineSnapshot snapshot = std::move(snapshot_result).value();
    const std::int64_t end_sample =
        snapshot.duration()
            .rescaledTo(audio::kPlaybackAudioFormat.sample_rate, edit::RoundingMode::Ceil)
            .value();
    if (playhead_ < 0 || playhead_ >= end_sample) {
      SessionEventLog::instance().log_backend("startAudioMasterPlayback", "failure playhead");
      return false;
    }

    auto timeline_renderer = std::make_shared<audio_render::TimelineAudioRenderer>(audio_registry_);
    auto provider = std::make_shared<TimelinePlaybackAudioProvider>(
        timeline_renderer, std::move(snapshot), end_sample);
    audio::RealtimePlaybackConfiguration configuration{
        .ring_capacity_frames = 192'000,
        .render_block_frames = 24'000,
        .prefill_frames = 48'000,
        .prefill_timeout = std::chrono::milliseconds(2'000),
        .device_id = selected_audio_device_id_.toStdString(),
    };
    auto candidate = std::make_unique<audio::AsyncRealtimeAudioPlayback>(
        std::move(provider), configuration, std::make_unique<audio::MiniaudioOutputDevice>());
    const audio::PlaybackCommandReceipt receipt = candidate->request_start(playhead_);
    if (!receipt.accepted) {
      const QString failure = receipt.error.has_value()
                                  ? QString::fromStdString(receipt.error->message)
                                  : tr("the audio control queue rejected the start request");
      if (!audio_fallback_announced_) {
        audio_fallback_announced_ = true;
        window_.showTransientMessage(
            tr("Could not start realtime audio; using silent timer playback: %1").arg(failure),
            8'000);
      }
      SessionEventLog::instance().log_backend("startAudioMasterPlayback", "failure start");
      return false;
    }

    playback_audio_renderer_ = std::move(timeline_renderer);
    audio_playback_ = std::move(candidate);
    audio_master_active_ = false;
    audio_start_pending_ = false;
    audio_session_stale_ = false;
    audio_control_intent_ = AudioControlIntent::Start;
    audio_command_version_ = receipt.version;
    last_audio_xrun_count_ = 0;
    SessionEventLog::instance().log_backend("startAudioMasterPlayback", "success");
    return true;
  } catch (const std::exception& exception) {
    if (!audio_fallback_announced_) {
      audio_fallback_announced_ = true;
      window_.showTransientMessage(
          tr("Could not prepare realtime audio; using silent timer playback: %1")
              .arg(QString::fromUtf8(exception.what())),
          8'000);
    }
    SessionEventLog::instance().log_backend("startAudioMasterPlayback", "failure exception");
    return false;
  }
}

void EditorController::stopAudioPlayback() noexcept {
  audio_recovery_pending_ = false;
  audio_master_active_ = false;
  audio_start_pending_ = false;
  audio_session_stale_ = true;
  audio_control_intent_ = AudioControlIntent::None;
  audio_command_version_ = 0;
  last_audio_xrun_count_ = 0;
  if (audio_playback_ != nullptr) {
    static_cast<void>(audio_playback_->request_stop());
  }
  window_.audioMixer()->setTrackMeters({});
  playback_audio_renderer_.reset();
}

void EditorController::requestPreview(const PreviewRequestPolicy policy) {
  if (!editor_ || !renderer_ || !frame_provider_) {
    return;
  }
  ++preview_request_serial_;
  const auto invalidate_in_flight = [this] {
    ++preview_epoch_;
    renderer_->begin_epoch(preview_epoch_);
    if (gpu_timeline_renderer_ != nullptr) {
      gpu_timeline_renderer_->begin_epoch(preview_epoch_);
    }
    frame_provider_->begin_epoch(preview_epoch_);
  };
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr ||
      std::none_of(sequence->tracks.begin(), sequence->tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Video && !track.clips.empty();
      })) {
    invalidate_in_flight();
    gpu_preview_active_ = false;
    window_.programViewer()->clearFrame();
    return;
  }
  requested_preview_position_ = playhead_;
  if (preview_in_flight_) {
    if (policy == PreviewRequestPolicy::Replace) {
      invalidate_in_flight();
    }
    return;
  }
  launchPreviewRequest();
}

void EditorController::launchPreviewRequest() {
  const edit::Sequence* sequence = currentSequence();
  if (sequence == nullptr ||
      std::none_of(sequence->tracks.begin(), sequence->tracks.end(), [](const edit::Track& track) {
        return track.kind == edit::TrackKind::Video && !track.clips.empty();
      })) {
    window_.programViewer()->clearFrame();
    return;
  }
  auto snapshot_result = editor_->snapshot(sequence->id, editor_->revision());
  if (!snapshot_result) {
    window_.programViewer()->clearFrame();
    return;
  }
  const std::uint64_t request_serial = preview_request_serial_;
  const std::uint64_t epoch = ++preview_epoch_;
  renderer_->begin_epoch(epoch);
  if (gpu_timeline_renderer_ != nullptr) {
    gpu_timeline_renderer_->begin_epoch(epoch);
  }
  frame_provider_->begin_epoch(epoch);
  const edit::Time requested_time(requested_preview_position_,
                                  static_cast<std::uint32_t>(kUiTimescale));
  auto snapshot = std::move(snapshot_result).value();
  const auto renderer = renderer_;
  const auto gpu_renderer = gpu_fallback_latched_ ? nullptr : gpu_renderer_;
  const auto gpu_timeline_renderer = gpu_fallback_latched_ ? nullptr : gpu_timeline_renderer_;
  preview_in_flight_ = true;

  using PreviewWatcher = QFutureWatcher<PreviewOutcome>;
  auto* watcher = new PreviewWatcher(this);
  connect(watcher, &PreviewWatcher::finished, this, [this, watcher, request_serial] {
    const PreviewOutcome outcome = watcher->result();
    watcher->deleteLater();
    preview_in_flight_ = false;
    if (outcome.epoch == preview_epoch_) {
      gpu_preview_active_ = outcome.gpu_used;
      if (outcome.gpu_failed && !gpu_fallback_latched_) {
        gpu_fallback_latched_ = true;
        gpu_timeline_renderer_.reset();
        gpu_renderer_.reset();
        window_.programViewer()->setTitle(tr("Program · CPU fallback"));
        window_.showTransientMessage(
            tr("GPU preview failed; using the CPU renderer for this session: %1")
                .arg(outcome.gpu_diagnostic),
            8'000);
      } else if (outcome.gpu_used) {
        window_.programViewer()->setTitle(tr("Program · %1 GPU").arg(outcome.gpu_backend));
        if (!gpu_status_announced_) {
          gpu_status_announced_ = true;
          window_.showTransientMessage(
              tr("GPU preview active through libplacebo (%1)").arg(outcome.gpu_backend));
        }
      } else if (!outcome.gpu_used && !outcome.gpu_failed && !outcome.gpu_diagnostic.isEmpty() &&
                 outcome.gpu_backend != QStringLiteral("CPU")) {
        // Unsupported timeline features fall back for this frame only. Keep
        // the ready backend visible without claiming that the displayed image
        // was GPU-rendered.
        window_.programViewer()->setTitle(
            tr("Program · CPU frame · %1 GPU ready").arg(outcome.gpu_backend));
      }
      if (!outcome.image.isNull()) {
        window_.programViewer()->setFrame(outcome.image);
        ++preview_presentation_count_;
      } else if (!outcome.error.isEmpty()) {
        window_.programViewer()->clearFrame();
        window_.showTransientMessage(outcome.error);
      }
    }
    if (outcome.epoch != preview_epoch_ || request_serial != preview_request_serial_) {
      launchPreviewRequest();
    }
  });
  watcher->setFuture(QtConcurrent::run([renderer, gpu_renderer, gpu_timeline_renderer,
                                        snapshot = std::move(snapshot), requested_time,
                                        epoch]() mutable {
    const render::PreviewProfile profile{
        .scale = render::PreviewScale::Half, .bypass_expensive_effects = true, .use_proxies = true};
    const auto cpu_fallback = [&](QString backend, QString diagnostic,
                                  const bool gpu_failed) -> PreviewOutcome {
      auto result = renderer->request_frame(snapshot, requested_time, profile, epoch);
      if (!result) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QString::fromStdString(result.error->message),
                              .gpu_backend = std::move(backend),
                              .gpu_diagnostic = std::move(diagnostic),
                              .gpu_used = false,
                              .gpu_failed = gpu_failed};
      }
      const auto* cpu =
          std::get_if<std::shared_ptr<const render::CpuFrame>>(&result.value->storage);
      if (cpu == nullptr || !*cpu) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QObject::tr("CPU preview returned unsupported storage"),
                              .gpu_backend = std::move(backend),
                              .gpu_diagnostic = std::move(diagnostic),
                              .gpu_used = false,
                              .gpu_failed = gpu_failed};
      }
      return PreviewOutcome{.epoch = epoch,
                            .image = EditorController::displayImage(**cpu),
                            .error = {},
                            .gpu_backend = std::move(backend),
                            .gpu_diagnostic = std::move(diagnostic),
                            .gpu_used = false,
                            .gpu_failed = gpu_failed};
    };

    if (gpu_renderer != nullptr && gpu_timeline_renderer != nullptr) {
      const render::GpuCapabilities capabilities = gpu_renderer->capabilities();
      const QString backend = gpuBackendName(capabilities.backend);
      if (!capabilities.available() || !capabilities.offscreen_rendering) {
        return cpu_fallback(backend, QString::fromStdString(capabilities.diagnostic), true);
      }

      auto gpu_frame =
          gpu_timeline_renderer->request_frame(snapshot, requested_time, profile, epoch);
      if (gpu_frame) {
        auto downloaded = gpu_renderer->download(*gpu_frame.value);
        if (downloaded) {
          const auto* gpu_cpu =
              std::get_if<std::shared_ptr<const render::CpuFrame>>(&downloaded.value->storage);
          if (gpu_cpu != nullptr && *gpu_cpu) {
            return PreviewOutcome{.epoch = epoch,
                                  .image = EditorController::displayImage(**gpu_cpu),
                                  .error = {},
                                  .gpu_backend = backend,
                                  .gpu_diagnostic = {},
                                  .gpu_used = true,
                                  .gpu_failed = false};
          }
        }
        const QString failure = downloaded.error.has_value()
                                    ? QString::fromStdString(downloaded.error->message)
                                    : QObject::tr("GPU readback returned no CPU frame");
        return cpu_fallback(backend, failure, true);
      }

      const render::RenderError& failure = *gpu_frame.error;
      if (failure.code == render::RenderErrorCode::StaleRequest) {
        return PreviewOutcome{.epoch = epoch,
                              .image = {},
                              .error = QString::fromStdString(failure.message),
                              .gpu_backend = backend,
                              .gpu_diagnostic = {},
                              .gpu_used = false,
                              .gpu_failed = false};
      }
      const bool should_latch = failure.code == render::RenderErrorCode::GpuUnavailable ||
                                failure.code == render::RenderErrorCode::GpuUploadFailed ||
                                failure.code == render::RenderErrorCode::GpuRenderFailed ||
                                failure.code == render::RenderErrorCode::GpuDownloadFailed ||
                                failure.code == render::RenderErrorCode::GpuDeviceLost;
      return cpu_fallback(backend, QString::fromStdString(failure.message), should_latch);
    }
    return cpu_fallback(QStringLiteral("CPU"), {}, false);
  }));
}

QImage EditorController::displayImage(const render::CpuFrame& frame) {
  QImage image(frame.width(), frame.height(), QImage::Format_RGBA8888);
  if (image.isNull()) {
    return {};
  }
  const auto encode = [](float linear) {
    linear = std::max(0.0F, linear);
    const float encoded =
        linear <= 0.0031308F ? linear * 12.92F : (1.055F * std::pow(linear, 1.0F / 2.4F)) - 0.055F;
    return static_cast<uchar>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
  };
  for (int y = 0; y < frame.height(); ++y) {
    uchar* output = image.scanLine(y);
    for (int x = 0; x < frame.width(); ++x) {
      const auto pixel = frame.pixel(x, y);
      const float alpha = std::clamp(pixel[3], 0.0F, 1.0F);
      const float inverse_alpha = alpha > 0.0F ? 1.0F / alpha : 0.0F;
      output[(x * 4) + 0] = encode(pixel[0] * inverse_alpha);
      output[(x * 4) + 1] = encode(pixel[1] * inverse_alpha);
      output[(x * 4) + 2] = encode(pixel[2] * inverse_alpha);
      output[(x * 4) + 3] = static_cast<uchar>(std::lround(alpha * 255.0F));
    }
  }
  return image;
}

const edit::Sequence* EditorController::currentSequence() const {
  if (!editor_) {
    return nullptr;
  }
  const auto project = editor_->projectAt(editor_->revision());
  return project->sequences.empty() ? nullptr : &project->sequences.front();
}

const edit::Asset* EditorController::assetByTextId(const QString& text) const {
  const auto id = parseId(text);
  if (!id.has_value()) {
    return nullptr;
  }
  const auto project = editor_->projectAt(editor_->revision());
  return edit::findAsset(*project, *id);
}

edit::Time EditorController::playheadTime() const {
  return edit::Time(playhead_, static_cast<std::uint32_t>(kUiTimescale));
}

void EditorController::setDirty(const bool dirty) {
  dirty_ = dirty;
  window_.setProjectDirty(dirty_);
}

void EditorController::showError(const QString& title, const QString& message) {
  QMessageBox::critical(&window_, title, message);
}

void EditorController::waitForInFlightCacheJob(const bool cancel) {
  if (cancel) {
    ++cache_job_generation_;
    cache_job_stop_source_.request_stop();
  }
  if (cache_job_future_.isRunning()) {
    cache_job_future_.waitForFinished();
  }
  if (cancel) {
    cache_job_running_ = false;
    cache_job_queue_.clear();
    cache_job_stop_source_ = std::stop_source();
  }
}

void EditorController::dropCachedPreview(const std::string& asset_id) {
  media_thumbnails_.erase(asset_id);
  media_waveforms_.erase(asset_id);
  media_metadata_titles_.erase(asset_id);
}

void EditorController::loadCachedPreviews(const assets::AssetRecord& record) {
  if (media_cache_ == nullptr || cache_job_running_) {
    return;
  }
  if (recordHasVideo(record)) {
    const media_cache::ThumbnailOptions options;
    const media_cache::CacheKey key{.asset_id = record.id,
                                    .kind = media_cache::CacheKind::Thumbnail,
                                    .parameter_hash = media_cache::thumbnail_parameter_hash(options)};
    const auto present = media_cache_->contains(key);
    if (present && present.value()) {
      if (auto thumbnail = media_cache::load_thumbnail(record.id, options, *media_cache_)) {
        media_thumbnails_[record.id] = imageFromJpeg(thumbnail.value().jpeg_bytes);
      }
    }
  }
  if (recordHasAudio(record)) {
    const media_cache::WaveformOptions options;
    const media_cache::CacheKey key{.asset_id = record.id,
                                    .kind = media_cache::CacheKind::Waveform,
                                    .parameter_hash = media_cache::waveform_parameter_hash(options)};
    const auto present = media_cache_->contains(key);
    if (present && present.value()) {
      if (auto waveform = media_cache::load_waveform(record.id, options, *media_cache_)) {
        media_waveforms_[record.id] = waveformBucketsForUi(waveform.value());
      }
    }
  }
  if (auto metadata = media_cache::load_metadata(record.id, *media_cache_)) {
    media_metadata_titles_[record.id] = QString::fromStdString(metadata.value().title);
  }
}

bool EditorController::reconstructMediaState() {
  waitForInFlightCacheJob(true);
  imported_assets_.clear();
  media_thumbnails_.clear();
  media_waveforms_.clear();
  media_metadata_titles_.clear();
  cache_job_queue_.clear();
  cache_disk_full_ = false;
  proxy_auto_queue_.clear();
  selected_media_id_.clear();
  for (auto& [asset_id, job] : proxy_jobs_) {
    Q_UNUSED(asset_id)
    job.cancel_requested = true;
    if (job.session != nullptr) {
      job.session->cancel();
    }
  }

  if (!editor_) {
    return false;
  }

  MediaReconstructionOptions options;
  options.legacy_proxy_directory = proxyCacheDirectory();
  std::unordered_set<std::string> seen_directories;
  if (checkpoint_path_.has_value()) {
    appendUniqueSearchDirectory(options.search_directories, seen_directories,
                                checkpoint_path_->parent_path());
  }
  const QString last_relink =
      QSettings().value(QStringLiteral("mediaCache/lastRelinkDirectory")).toString();
  if (!last_relink.isEmpty()) {
    appendUniqueSearchDirectory(options.search_directories, seen_directories,
                                pathFromQString(last_relink));
  }
  const auto project = editor_->projectAt(editor_->revision());
  for (const edit::Asset& asset : project->assets) {
    appendUniqueSearchDirectory(options.search_directories, seen_directories,
                                path_from_utf8(asset.source_uri).parent_path());
  }

  imported_assets_ =
      reconstruct_media_records(project->assets, media_cache_.get(), options);
  bool relinked = false;
  for (const assets::AssetRecord& record : imported_assets_) {
    const edit::Asset* model = nullptr;
    for (const edit::Asset& asset : project->assets) {
      if (asset.id.toString() == record.id) {
        model = &asset;
        break;
      }
    }
    if (model == nullptr || record.availability != assets::AssetAvailability::Online) {
      continue;
    }
    if (utf8_from_path(record.uri) == model->source_uri) {
      continue;
    }
    if (apply(edit::EditCommand{.operation = relink_command_from_record(record),
                                .coalescing_key = {}},
              tr("Could not persist the recovered media path"))) {
      relinked = true;
    }
  }

  rebuildPlaybackRegistry();
  for (const assets::AssetRecord& record : imported_assets_) {
    loadCachedPreviews(record);
    enqueueMediaCacheJobs(record);
  }
  scheduleRecommendedProxies();
  return relinked;
}

void EditorController::enqueueMediaCacheJobs(const assets::AssetRecord& asset) {
  if (cache_disk_full_ || media_cache_ == nullptr ||
      asset.availability != assets::AssetAvailability::Online) {
    return;
  }
  if (recordHasVideo(asset) && !media_thumbnails_.contains(asset.id)) {
    cache_job_queue_.emplace_back(asset.id, kCacheJobThumbnail);
  }
  if (recordHasAudio(asset) && !media_waveforms_.contains(asset.id)) {
    cache_job_queue_.emplace_back(asset.id, kCacheJobWaveform);
  }
  pumpCacheJobs();
}

void EditorController::pumpCacheJobs() {
  if (cache_job_running_ || cache_job_queue_.empty() || media_cache_ == nullptr || cache_disk_full_) {
    return;
  }
  const auto job = cache_job_queue_.front();
  cache_job_queue_.pop_front();
  const assets::AssetRecord* record = findImported(imported_assets_, job.first);
  if (record == nullptr || record->availability != assets::AssetAvailability::Online) {
    pumpCacheJobs();
    return;
  }

  cache_job_running_ = true;
  const std::string asset_id = record->id;
  const std::filesystem::path uri = record->uri;
  const int kind = job.second;
  const std::uint64_t generation = cache_job_generation_;
  const std::stop_token stop_token = cache_job_stop_source_.get_token();
  media_cache::CacheStore* store = media_cache_.get();

  auto* watcher = new QFutureWatcher<CacheJobOutcome>(this);
  connect(watcher, &QFutureWatcher<CacheJobOutcome>::finished, this, [this, watcher] {
    const CacheJobOutcome outcome = watcher->result();
    watcher->deleteLater();
    cache_job_running_ = false;
    if (outcome.generation != cache_job_generation_) {
      pumpCacheJobs();
      return;
    }
    if (outcome.disk_full) {
      cache_disk_full_ = true;
      cache_job_queue_.clear();
      showError(tr("Media cache is full"),
                outcome.error.isEmpty() ? tr("The media cache has no remaining space.")
                                        : outcome.error);
      return;
    }
    if (outcome.succeeded) {
      if (outcome.kind == kCacheJobThumbnail && !outcome.thumbnail.isNull()) {
        media_thumbnails_[outcome.asset_id] = outcome.thumbnail;
      } else if (outcome.kind == kCacheJobWaveform) {
        media_waveforms_[outcome.asset_id] = outcome.waveform;
      }
      refreshMediaView();
      refreshTimelineView();
    } else if (!outcome.cancelled && !outcome.error.isEmpty()) {
      window_.showTransientMessage(
          tr("Could not cache media preview: %1").arg(outcome.error));
    }
    if (auto* browser = window_.cacheBrowser(); browser != nullptr && browser->isVisible()) {
      refreshCacheInventory();
    }
    pumpCacheJobs();
  });

  cache_job_future_ = QtConcurrent::run(
      [asset_id, uri, kind, store, stop_token, generation]() -> CacheJobOutcome {
        CacheJobOutcome outcome;
        outcome.asset_id = asset_id;
        outcome.kind = kind;
        outcome.generation = generation;
        if (kind == kCacheJobThumbnail) {
          auto generated = media_cache::generate_thumbnail(uri, -1, {}, asset_id, *store, stop_token);
          if (!generated) {
            outcome.cancelled = generated.error().code == media_cache::ThumbnailErrorCode::Cancelled;
            outcome.disk_full = generated.error().code == media_cache::ThumbnailErrorCode::StoreFailed &&
                                storeErrorLooksFull(generated.error().message);
            outcome.error = QString::fromStdString(generated.error().message);
            return outcome;
          }
          outcome.thumbnail = imageFromJpeg(generated.value().jpeg_bytes);
          outcome.succeeded = !outcome.thumbnail.isNull();
          return outcome;
        }
        auto generated = media_cache::generate_waveform(uri, -1, {}, asset_id, *store, stop_token);
        if (!generated) {
          outcome.cancelled = generated.error().code == media_cache::WaveformErrorCode::Cancelled;
          outcome.disk_full = generated.error().code == media_cache::WaveformErrorCode::StoreFailed &&
                              storeErrorLooksFull(generated.error().message);
          outcome.error = QString::fromStdString(generated.error().message);
          return outcome;
        }
        outcome.waveform = waveformBucketsForUi(generated.value());
        outcome.succeeded = true;
        return outcome;
      });
  watcher->setFuture(cache_job_future_);
}

void EditorController::scheduleRecommendedProxies() {
  for (const assets::AssetRecord& record : imported_assets_) {
    if (record.availability != assets::AssetAvailability::Online) {
      continue;
    }
    if (!assets::AssetService::should_recommend_proxy(record)) {
      continue;
    }
    if (record.proxy.has_value() && record.proxy->complete) {
      continue;
    }
    if (proxy_jobs_.contains(record.id)) {
      continue;
    }
    const QString id = QString::fromStdString(record.id);
    if (std::find(proxy_auto_queue_.begin(), proxy_auto_queue_.end(), id) ==
        proxy_auto_queue_.end()) {
      proxy_auto_queue_.push_back(id);
    }
  }
  pumpProxyQueue();
}

void EditorController::pumpProxyQueue() {
  if (!proxy_jobs_.empty()) {
    return;
  }
  while (!proxy_auto_queue_.empty()) {
    const QString id = proxy_auto_queue_.front();
    proxy_auto_queue_.pop_front();
    generateProxy(id);
    if (!proxy_jobs_.empty()) {
      return;
    }
  }
}

void EditorController::reregisterAssetMedia(const assets::AssetRecord& record) {
  const auto model_id = edit::EntityId::parse(record.id);
  if (!model_id.has_value()) {
    return;
  }
  playback::AssetPlaybackSources sources{
      .original = {.path = record.uri, .video_stream_index = -1},
      .proxy = record.proxy.has_value() && record.proxy->complete
                   ? std::optional<playback::AssetStreamLocation>{playback::AssetStreamLocation{
                         .path = record.proxy->proxy_uri, .video_stream_index = -1}}
                   : std::nullopt};
  (void)playback_registry_->register_asset(*model_id, std::move(sources));
  frame_provider_->invalidate(*model_id);
  if (recordHasAudio(record)) {
    (void)audio_registry_->register_original(
        *model_id, audio_render::OriginalAudioMedia{.path = record.uri, .audio_stream_index = -1});
  }
}

void EditorController::relinkMedia(const QString& assetId) {
  QSettings settings;
  const QString start_directory =
      settings.value(QStringLiteral("mediaCache/lastRelinkDirectory")).toString();
  const QString path = QFileDialog::getOpenFileName(&window_, tr("Relink media"), start_directory);
  if (path.isEmpty()) {
    return;
  }

  const std::string key = assetId.toStdString();
  assets::AssetRecord existing;
  if (const auto* record = findImported(imported_assets_, key); record != nullptr) {
    existing = *record;
  } else {
    const edit::Asset* asset = assetByTextId(assetId);
    if (asset == nullptr) {
      window_.showTransientMessage(tr("That media item is no longer in the project"));
      return;
    }
    existing.id = asset->id.toString();
    existing.uri = pathFromUtf8String(asset->source_uri);
    existing.fingerprint.quick_sha256 = asset->fingerprint;
  }

  assets::AssetService service;
  auto relinked = service.relink(existing, pathFromQString(path), false);
  if (!relinked) {
    const bool fingerprint_mismatch =
        relinked.error().message.find("fingerprint") != std::string::npos;
    if (fingerprint_mismatch) {
      const auto answer = QMessageBox::question(
          &window_, tr("Relink media"),
          tr("The replacement file does not match the original fingerprint. Relink anyway?"),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (answer != QMessageBox::Yes) {
        return;
      }
      relinked = service.relink(existing, pathFromQString(path), true);
    }
    if (!relinked) {
      showError(tr("Could not relink media"), QString::fromStdString(relinked.error().message));
      return;
    }
  }

  assets::AssetRecord result = std::move(relinked).value();
  result.proxy.reset();
  if (!apply(edit::EditCommand{.operation = relink_command_from_record(result), .coalescing_key = {}},
             tr("Could not relink media"))) {
    return;
  }

  if (auto* record = findImported(imported_assets_, key); record != nullptr) {
    *record = result;
  } else {
    imported_assets_.push_back(result);
  }
  dropCachedPreview(result.id);
  reregisterAssetMedia(result);
  settings.setValue(QStringLiteral("mediaCache/lastRelinkDirectory"),
                    QFileInfo(path).absolutePath());
  enqueueMediaCacheJobs(result);
  scheduleRecommendedProxies();
  refreshViews();
}

void EditorController::selectMedia(const QString& mediaId) {
  selected_media_id_ = mediaId;
  refreshInspectorView();
}

void EditorController::presentAssetMetadata(const QString& assetId) {
  if (assetId.isEmpty()) {
    window_.inspector()->clearAssetMetadata();
    return;
  }
  desktop_ui::AssetMetadataView view;
  view.assetId = assetId;
  const std::string key = assetId.toStdString();
  if (media_cache_ != nullptr && !cache_job_running_) {
    if (auto loaded = media_cache::load_metadata(key, *media_cache_)) {
      view.title = QString::fromStdString(loaded.value().title);
      view.notes = QString::fromStdString(loaded.value().notes);
      view.rating = loaded.value().rating;
      for (const auto& tag : loaded.value().tags) {
        view.tags.push_back(QString::fromStdString(tag));
      }
      media_metadata_titles_[key] = view.title;
      window_.inspector()->setAssetMetadata(view);
      return;
    }
  }
  if (const auto title = media_metadata_titles_.find(key); title != media_metadata_titles_.end()) {
    view.title = title->second;
  }
  window_.inspector()->setAssetMetadata(view);
}

void EditorController::saveAssetMetadata(const desktop_ui::AssetMetadataView& metadata) {
  if (metadata.assetId.isEmpty()) {
    return;
  }
  if (media_cache_ == nullptr) {
    window_.showTransientMessage(tr("Media cache is unavailable"));
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  media_cache::MetadataDocument document;
  document.title = metadata.title.toStdString();
  document.notes = metadata.notes.toStdString();
  document.rating = metadata.rating;
  document.tags.reserve(static_cast<std::size_t>(metadata.tags.size()));
  for (const QString& tag : metadata.tags) {
    document.tags.push_back(tag.toStdString());
  }
  const auto saved = media_cache::save_metadata(metadata.assetId.toStdString(), document, *media_cache_);
  if (!saved) {
    showError(tr("Could not save media metadata"), QString::fromStdString(saved.error().message));
    return;
  }
  media_metadata_titles_[metadata.assetId.toStdString()] = metadata.title;
  refreshMediaView();
}

void EditorController::showMediaCacheBrowser() {
  refreshCacheInventory();
}

void EditorController::refreshCacheInventory() {
  auto* browser = window_.cacheBrowser();
  if (browser == nullptr) {
    return;
  }
  if (media_cache_ == nullptr) {
    browser->setInventory({});
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; inventory will refresh when idle"));
    return;
  }
  const auto inspected = media_cache_->inspect();
  if (!inspected) {
    showError(tr("Could not inspect the media cache"),
              QString::fromStdString(inspected.error().message));
    return;
  }
  desktop_ui::CacheInventoryView view;
  view.totalBytes = static_cast<qint64>(inspected.value().total_bytes);
  view.budgetBytes = static_cast<qint64>(inspected.value().budget_bytes);
  const auto project = editor_ ? editor_->projectAt(editor_->revision()) : nullptr;
  for (const auto& entry : inspected.value().entries) {
    desktop_ui::CacheEntryView row;
    row.assetId = QString::fromStdString(entry.key.asset_id);
    row.kindText = cacheKindText(entry.key.kind);
    row.bytes = static_cast<qint64>(entry.bytes);
    row.lastAccessUtcMs = entry.last_access_utc_ms;
    row.displayName = row.assetId;
    if (project != nullptr) {
      for (const edit::Asset& asset : project->assets) {
        if (asset.id.toString() == entry.key.asset_id) {
          row.displayName = QString::fromStdString(asset.name);
          break;
        }
      }
    }
    view.entries.push_back(std::move(row));
  }
  browser->setInventory(view);
}

void EditorController::handleCacheBudgetChanged(const qint64 budgetBytes) {
  const qint64 budget = std::max<qint64>(0, budgetBytes);
  QSettings().setValue(QStringLiteral("mediaCache/budgetBytes"), budget);
  if (media_cache_ == nullptr) {
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  cache_disk_full_ = false;
  media_cache_->set_budget_bytes(static_cast<std::uint64_t>(budget));
  (void)media_cache_->evict_to_budget();
  media_thumbnails_.clear();
  media_waveforms_.clear();
  for (const assets::AssetRecord& record : imported_assets_) {
    loadCachedPreviews(record);
    enqueueMediaCacheJobs(record);
  }
  refreshCacheInventory();
  refreshViews();
}

void EditorController::removeCacheEntry(const QString& assetId, const QString& kindText) {
  if (media_cache_ == nullptr) {
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  const auto kind = cacheKindFromText(kindText);
  if (!kind.has_value()) {
    return;
  }
  (void)media_cache_->remove_kind(assetId.toStdString(), *kind);
  if (*kind == media_cache::CacheKind::Thumbnail) {
    media_thumbnails_.erase(assetId.toStdString());
  } else if (*kind == media_cache::CacheKind::Waveform) {
    media_waveforms_.erase(assetId.toStdString());
  } else if (*kind == media_cache::CacheKind::Metadata) {
    media_metadata_titles_.erase(assetId.toStdString());
  } else if (*kind == media_cache::CacheKind::Proxy || *kind == media_cache::CacheKind::ProxyPtsMap) {
    if (auto* record = findImported(imported_assets_, assetId.toStdString()); record != nullptr) {
      record->proxy.reset();
      reregisterAssetMedia(*record);
    }
  }
  refreshCacheInventory();
  refreshViews();
}

void EditorController::removeCacheAsset(const QString& assetId) {
  if (media_cache_ == nullptr) {
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  (void)media_cache_->remove_asset(assetId.toStdString());
  dropCachedPreview(assetId.toStdString());
  if (auto* record = findImported(imported_assets_, assetId.toStdString()); record != nullptr) {
    record->proxy.reset();
    reregisterAssetMedia(*record);
  }
  refreshCacheInventory();
  refreshViews();
}

void EditorController::evictCacheToBudget() {
  if (media_cache_ == nullptr) {
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  (void)media_cache_->evict_to_budget();
  media_thumbnails_.clear();
  media_waveforms_.clear();
  for (const assets::AssetRecord& record : imported_assets_) {
    loadCachedPreviews(record);
  }
  refreshCacheInventory();
  refreshViews();
}

void EditorController::clearMediaCache() {
  if (media_cache_ == nullptr) {
    return;
  }
  if (cache_job_running_) {
    window_.showTransientMessage(tr("Media cache is busy; try again in a moment"));
    return;
  }
  (void)media_cache_->clear();
  media_thumbnails_.clear();
  media_waveforms_.clear();
  media_metadata_titles_.clear();
  cache_disk_full_ = false;
  for (assets::AssetRecord& record : imported_assets_) {
    record.proxy.reset();
    reregisterAssetMedia(record);
  }
  refreshCacheInventory();
  refreshViews();
}

} // namespace video_editor::app
