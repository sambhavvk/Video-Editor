// SPDX-License-Identifier: MPL-2.0

#include "video_editor/agent_host/agent_host.h"

#include "video_editor/agent_protocol/edit_command_json.h"
#include "video_editor/asset_service/asset_service.h"
#include "video_editor/asset_service/edit_asset.h"
#include "video_editor/edit_model/commands.h"
#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/timeline_editor.h"
#include "video_editor/media_codec/png_encode.h"
#include "video_editor/playback/asset_registry.h"
#include "video_editor/playback/ffmpeg_frame_provider.h"
#include "video_editor/project_codec/project_codec.h"
#include "video_editor/project_store/project_store.hpp"
#include "video_editor/render_engine/cpu_frame_encode.h"
#include "video_editor/render_engine/cpu_renderer.h"
#include "video_editor/render_engine/scope_analyzer.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace video_editor::agent_host {
namespace {

constexpr const char* kAgentWorkingSuffix = ".agent.working.sqlite";

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value) {
  const auto* first = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(first, first + value.size()));
}

[[nodiscard]] std::string utf8FromPath(const std::filesystem::path& value) {
  const auto utf8 = value.generic_u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] QString stdToQstr(const std::string& value) {
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

[[nodiscard]] std::string qstrToStd(const QString& value) {
  const QByteArray utf8 = value.toUtf8();
  return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

[[nodiscard]] QJsonObject encodeTime(const edit::Time& time) {
  QJsonObject object;
  object.insert(QStringLiteral("value"), static_cast<qint64>(time.value()));
  object.insert(QStringLiteral("timescale"), static_cast<int>(time.timescale()));
  return object;
}

[[nodiscard]] edit::Result<edit::Time, std::string> decodeTime(const QJsonValue& value,
                                                                 const char* context) {
  if (!value.isObject()) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": expected time object");
  }
  const QJsonObject object = value.toObject();
  if (!object.contains(QStringLiteral("value")) || !object.contains(QStringLiteral("timescale"))) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": time requires value and timescale");
  }
  const qint64 raw_value = object.value(QStringLiteral("value")).toInteger();
  const int raw_timescale = object.value(QStringLiteral("timescale")).toInt();
  if (raw_timescale <= 0) {
    return edit::Result<edit::Time, std::string>::failure(
        std::string(context) + ": timescale must be positive");
  }
  return edit::Result<edit::Time, std::string>::success(
      edit::Time{raw_value, static_cast<std::uint32_t>(raw_timescale)});
}

[[nodiscard]] QJsonObject encodeRate(const edit::Rate& rate) {
  QJsonObject object;
  object.insert(QStringLiteral("numerator"), static_cast<int>(rate.numerator()));
  object.insert(QStringLiteral("denominator"), static_cast<int>(rate.denominator()));
  return object;
}

[[nodiscard]] QString encodeTrackKind(const edit::TrackKind kind) {
  switch (kind) {
  case edit::TrackKind::Video:
    return QStringLiteral("video");
  case edit::TrackKind::Audio:
    return QStringLiteral("audio");
  case edit::TrackKind::Caption:
    return QStringLiteral("caption");
  }
  return QStringLiteral("video");
}

[[nodiscard]] QString encodeClipKind(const edit::ClipKind kind) {
  switch (kind) {
  case edit::ClipKind::Video:
    return QStringLiteral("video");
  case edit::ClipKind::Audio:
    return QStringLiteral("audio");
  case edit::ClipKind::Title:
    return QStringLiteral("title");
  case edit::ClipKind::NestedSequence:
    return QStringLiteral("nested_sequence");
  }
  return QStringLiteral("video");
}

[[nodiscard]] QJsonObject encodeTimeRange(const edit::TimeRange& range) {
  QJsonObject object;
  object.insert(QStringLiteral("start"), encodeTime(range.start));
  object.insert(QStringLiteral("duration"), encodeTime(range.duration));
  return object;
}

[[nodiscard]] QJsonObject encodeClipSummary(const edit::Clip& clip) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(clip.id.toString()));
  object.insert(QStringLiteral("asset_id"), stdToQstr(clip.asset_id.toString()));
  object.insert(QStringLiteral("name"), stdToQstr(clip.name));
  object.insert(QStringLiteral("kind"), encodeClipKind(clip.kind));
  object.insert(QStringLiteral("timeline_range"), encodeTimeRange(clip.timeline_range));
  return object;
}

[[nodiscard]] QJsonObject encodeTrackSummary(const edit::Track& track) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(track.id.toString()));
  object.insert(QStringLiteral("kind"), encodeTrackKind(track.kind));
  object.insert(QStringLiteral("name"), stdToQstr(track.name));
  object.insert(QStringLiteral("locked"), track.locked);
  object.insert(QStringLiteral("targeted"), track.targeted);
  QJsonArray clips;
  for (const edit::Clip& clip : track.clips) {
    clips.append(encodeClipSummary(clip));
  }
  object.insert(QStringLiteral("clips"), clips);
  return object;
}

[[nodiscard]] QJsonObject encodeSequenceSummary(const edit::Sequence& sequence) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(sequence.id.toString()));
  object.insert(QStringLiteral("name"), stdToQstr(sequence.name));
  object.insert(QStringLiteral("width"), static_cast<int>(sequence.width));
  object.insert(QStringLiteral("height"), static_cast<int>(sequence.height));
  object.insert(QStringLiteral("frame_rate"), encodeRate(sequence.frame_rate));
  QJsonArray tracks;
  for (const edit::Track& track : sequence.tracks) {
    tracks.append(encodeTrackSummary(track));
  }
  object.insert(QStringLiteral("tracks"), tracks);
  return object;
}

[[nodiscard]] QJsonObject encodeAssetSummary(const edit::Asset& asset) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(asset.id.toString()));
  object.insert(QStringLiteral("name"), stdToQstr(asset.name));
  object.insert(QStringLiteral("source_uri"), stdToQstr(asset.source_uri));
  object.insert(QStringLiteral("has_video"), asset.has_video);
  object.insert(QStringLiteral("has_audio"), asset.has_audio);
  object.insert(QStringLiteral("duration"), encodeTime(asset.duration));
  return object;
}

[[nodiscard]] QJsonObject encodeProjectSummary(const edit::Project& project) {
  QJsonObject object;
  object.insert(QStringLiteral("id"), stdToQstr(project.id.toString()));
  object.insert(QStringLiteral("name"), stdToQstr(project.name));
  QJsonArray sequences;
  for (const edit::Sequence& sequence : project.sequences) {
    sequences.append(encodeSequenceSummary(sequence));
  }
  object.insert(QStringLiteral("sequences"), sequences);
  QJsonArray assets;
  for (const edit::Asset& asset : project.assets) {
    assets.append(encodeAssetSummary(asset));
  }
  object.insert(QStringLiteral("assets"), assets);
  return object;
}

[[nodiscard]] QString editErrorCodeName(const edit::EditErrorCode code) {
  switch (code) {
  case edit::EditErrorCode::RevisionConflict:
    return QStringLiteral("RevisionConflict");
  case edit::EditErrorCode::RevisionNotFound:
    return QStringLiteral("RevisionNotFound");
  case edit::EditErrorCode::EntityNotFound:
    return QStringLiteral("EntityNotFound");
  case edit::EditErrorCode::DuplicateId:
    return QStringLiteral("DuplicateId");
  case edit::EditErrorCode::InvalidArgument:
    return QStringLiteral("InvalidArgument");
  case edit::EditErrorCode::InvalidTrackKind:
    return QStringLiteral("InvalidTrackKind");
  case edit::EditErrorCode::TrackLocked:
    return QStringLiteral("TrackLocked");
  case edit::EditErrorCode::Overlap:
    return QStringLiteral("Overlap");
  case edit::EditErrorCode::AssetInUse:
    return QStringLiteral("AssetInUse");
  case edit::EditErrorCode::NothingToUndo:
    return QStringLiteral("NothingToUndo");
  case edit::EditErrorCode::NothingToRedo:
    return QStringLiteral("NothingToRedo");
  case edit::EditErrorCode::ArithmeticOverflow:
    return QStringLiteral("ArithmeticOverflow");
  }
  return QStringLiteral("InvalidArgument");
}

[[nodiscard]] QJsonObject makeError(const QString& code, const QString& message) {
  QJsonObject response;
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);
  response.insert(QStringLiteral("error"), error);
  return response;
}

[[nodiscard]] QJsonObject makeError(const QString& code, const QString& message,
                                    const edit::EditError& edit_error) {
  QJsonObject response = makeError(code, message);
  QJsonObject error = response.value(QStringLiteral("error")).toObject();
  if (edit_error.expected_revision.has_value()) {
    error.insert(QStringLiteral("expected_revision"),
                 static_cast<qint64>(edit_error.expected_revision->value));
  }
  if (edit_error.actual_revision.has_value()) {
    error.insert(QStringLiteral("actual_revision"),
                 static_cast<qint64>(edit_error.actual_revision->value));
  }
  response.insert(QStringLiteral("error"), error);
  return response;
}

[[nodiscard]] QJsonObject makeSuccess() {
  QJsonObject response;
  response.insert(QStringLiteral("ok"), true);
  return response;
}

[[nodiscard]] std::vector<std::byte> binaryPayload(const store::JournalEntry& entry) {
  if (const auto* bytes = std::get_if<store::BinaryPayload>(&entry.payload)) {
    return *bytes;
  }
  const auto& text = std::get<std::string>(entry.payload);
  std::vector<std::byte> bytes(text.size());
  std::transform(text.begin(), text.end(), bytes.begin(),
                 [](const char value) { return static_cast<std::byte>(value); });
  return bytes;
}

[[nodiscard]] std::optional<std::uint32_t> projectSnapshotSchema(
    const store::JournalEntry& entry) {
  if (entry.command_type == "project.snapshot.v1") {
    return 1U;
  }
  if (entry.command_type == "project.snapshot.v2") {
    return 2U;
  }
  if (entry.command_type == "project.snapshot.v3") {
    return 3U;
  }
  if (entry.command_type == "project.snapshot.v4") {
    return 4U;
  }
  if (entry.command_type == "project.snapshot.v5") {
    return 5U;
  }
  if (entry.command_type == "project.snapshot.v6") {
    return 6U;
  }
  return std::nullopt;
}

[[nodiscard]] const store::JournalEntry* latestProjectSnapshot(
    const std::vector<store::JournalEntry>& entries) {
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

[[nodiscard]] bool isStillImageUriExtension(const std::string& uri) {
  const auto dot = uri.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }
  std::string extension = uri.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "bmp" ||
         extension == "gif" || extension == "webp" || extension == "tiff" || extension == "tif";
}

[[nodiscard]] bool containerLooksLikeImage(const edit::Asset& asset) {
  const auto container = asset.metadata.find("container");
  if (container == asset.metadata.end()) {
    return false;
  }
  return container->second.find("png") != std::string::npos ||
         container->second.find("jpeg") != std::string::npos;
}

[[nodiscard]] bool isStillOverlayAsset(const edit::Asset& asset) {
  const auto media_kind = asset.metadata.find("media_kind");
  if (media_kind != asset.metadata.end()) {
    if (media_kind->second == "still" || media_kind->second == "image_sequence") {
      return true;
    }
  }
  return asset.has_video && !asset.has_audio &&
         (containerLooksLikeImage(asset) || isStillImageUriExtension(asset.source_uri));
}

[[nodiscard]] bool trackOverlapsRange(const edit::Track& track, const edit::TimeRange& range) {
  return std::any_of(track.clips.begin(), track.clips.end(), [&](const edit::Clip& clip) {
    return clip.timeline_range.overlaps(range);
  });
}

[[nodiscard]] edit::Result<edit::InsertMode, std::string> parseInsertMode(
    const QString& mode_text) {
  if (mode_text == QStringLiteral("ripple")) {
    return edit::Result<edit::InsertMode, std::string>::success(edit::InsertMode::Ripple);
  }
  if (mode_text == QStringLiteral("overwrite")) {
    return edit::Result<edit::InsertMode, std::string>::success(edit::InsertMode::Overwrite);
  }
  if (mode_text == QStringLiteral("reject_overlap")) {
    return edit::Result<edit::InsertMode, std::string>::success(edit::InsertMode::RejectOverlap);
  }
  return edit::Result<edit::InsertMode, std::string>::failure("mode must be ripple, overwrite, or "
                                                              "reject_overlap");
}

[[nodiscard]] edit::Project makeDefaultAgentProject(const std::string& name) {
  edit::Project project;
  project.id = edit::EntityId::generate();
  project.name = name.empty() ? "Untitled Project" : name;
  edit::Sequence sequence;
  sequence.name = "Sequence 1";
  sequence.frame_rate = edit::Rate{30, 1};
  sequence.width = 1920;
  sequence.height = 1080;
  edit::Track video;
  video.kind = edit::TrackKind::Video;
  video.name = "V1";
  video.targeted = true;
  edit::Track audio;
  audio.kind = edit::TrackKind::Audio;
  audio.name = "A1";
  audio.targeted = true;
  sequence.tracks = {video, audio};
  project.sequences = {sequence};
  return project;
}

void removeSqliteSidecars(const std::filesystem::path& database) {
  std::error_code ignored;
  std::filesystem::remove(database, ignored);
  auto wal = database;
  wal += "-wal";
  auto shm = database;
  shm += "-shm";
  std::filesystem::remove(wal, ignored);
  std::filesystem::remove(shm, ignored);
}

template <std::size_t Size>
[[nodiscard]] QJsonArray encodeUInt32Array(const std::array<std::uint32_t, Size>& values) {
  QJsonArray array;
  for (const std::uint32_t value : values) {
    array.append(static_cast<qint64>(value));
  }
  return array;
}

[[nodiscard]] QJsonArray encodeFloatArray(const std::array<float, 256>& values) {
  QJsonArray array;
  for (const float value : values) {
    array.append(static_cast<double>(value));
  }
  return array;
}

[[nodiscard]] QJsonArray encodeVectorscope(
    const std::array<std::array<std::uint32_t, render::ScopeAnalysis::kVectorscopeSize>,
                     render::ScopeAnalysis::kVectorscopeSize>& values) {
  QJsonArray rows;
  for (const auto& row : values) {
    rows.append(encodeUInt32Array(row));
  }
  return rows;
}

[[nodiscard]] QJsonArray encodeWaveform(
    const std::array<std::array<std::uint32_t, render::ScopeAnalysis::kWaveformBins>,
                     render::ScopeAnalysis::kWaveformColumns>& values) {
  QJsonArray columns;
  for (const auto& column : values) {
    columns.append(encodeUInt32Array(column));
  }
  return columns;
}

} // namespace

AgentHost::AgentHost()
    : registry_(std::make_shared<playback::AssetRegistry>()),
      frame_provider_(std::make_shared<playback::FfmpegFrameProvider>(registry_)),
      renderer_(std::make_shared<render::CpuRenderer>(frame_provider_)) {}

AgentHost::~AgentHost() = default;

QJsonObject AgentHost::handleRequest(const QJsonObject& request) {
  const QString method = request.value(QStringLiteral("method")).toString();
  if (method == QStringLiteral("open_project")) {
    return methodOpenProject(request);
  }
  if (method == QStringLiteral("new_project")) {
    return methodNewProject(request);
  }
  if (method == QStringLiteral("save_project")) {
    return methodSaveProject(request);
  }
  if (method == QStringLiteral("read_session")) {
    return methodReadSession();
  }
  if (method == QStringLiteral("set_session")) {
    return methodSetSession(request);
  }
  if (method == QStringLiteral("apply_commands")) {
    return methodApplyCommands(request);
  }
  if (method == QStringLiteral("undo")) {
    return methodUndo(request);
  }
  if (method == QStringLiteral("redo")) {
    return methodRedo(request);
  }
  if (method == QStringLiteral("import_media")) {
    return methodImportMedia(request);
  }
  if (method == QStringLiteral("insert_media")) {
    return methodInsertMedia(request);
  }
  if (method == QStringLiteral("get_preview_frame")) {
    return methodGetPreviewFrame(request);
  }
  if (method == QStringLiteral("get_scopes")) {
    return methodGetScopes(request);
  }
  return makeError(QStringLiteral("unknown_method"),
                 QStringLiteral("Unknown method: %1").arg(method));
}

std::filesystem::path AgentHost::agentWorkingPathForProject(
    const std::filesystem::path& directory, const std::string& project_id) const {
  return directory / (project_id + kAgentWorkingSuffix);
}

void AgentHost::installProject(edit::Project project, std::filesystem::path working_path,
                               std::unique_ptr<store::ProjectStore> project_store,
                               std::optional<std::filesystem::path> checkpoint) {
  if (store_) {
    try {
      store_->mark_clean_close(store_->metadata().head_revision);
    } catch (...) {
    }
  }
  editor_ = std::make_unique<edit::TimelineEditor>(std::move(project));
  store_ = std::move(project_store);
  working_path_ = std::move(working_path);
  checkpoint_path_ = std::move(checkpoint);
  session_ = AgentSession{};
  session_.playhead = edit::Time{0, kAgentUiTimescale};
  const auto installed = editor_->projectAt(editor_->revision());
  if (!installed->sequences.empty()) {
    session_.active_sequence_id = installed->sequences.front().id;
  }
  registerAllAssets();
  dirty_ = false;
  ++preview_epoch_;
}

void AgentHost::registerAllAssets() {
  registry_ = std::make_shared<playback::AssetRegistry>();
  frame_provider_ = std::make_shared<playback::FfmpegFrameProvider>(registry_);
  renderer_ = std::make_shared<render::CpuRenderer>(frame_provider_);
  if (!editor_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  for (const edit::Asset& asset : project->assets) {
    if (asset.source_uri.empty()) {
      continue;
    }
    playback::AssetPlaybackSources sources{
        .original = {.path = pathFromUtf8(asset.source_uri), .video_stream_index = -1},
        .proxy = std::nullopt,
        .pts_map_path = std::nullopt};
    (void)registry_->register_asset(asset.id, std::move(sources));
  }
}

void AgentHost::persistSnapshot(const std::string_view reason) {
  if (!editor_ || !store_) {
    return;
  }
  const auto project = editor_->projectAt(editor_->revision());
  const project_codec::ProjectBytes bytes = project_codec::serialize_project(*project);
  const auto metadata = store_->metadata();
  store_->append_command("project.snapshot.v6", std::span<const std::byte>(bytes),
                         metadata.head_revision, project_codec::kCurrentSchemaVersion);
  store_->update_heartbeat();
  (void)reason;
}

void AgentHost::persistAndCheckpoint(const std::string_view reason) {
  persistSnapshot(reason);
  if (checkpoint_path_.has_value()) {
    const auto revision = store_->metadata().head_revision;
    store_->checkpoint_to(*checkpoint_path_, revision);
    dirty_ = false;
  } else {
    dirty_ = true;
  }
}

edit::Revision AgentHost::resolveExpectedRevision(const QJsonObject& request) const {
  if (request.contains(QStringLiteral("expected_revision"))) {
    return edit::Revision{
        static_cast<std::uint64_t>(request.value(QStringLiteral("expected_revision")).toInteger())};
  }
  return editor_ ? editor_->revision() : edit::Revision{};
}

QJsonObject AgentHost::sessionPayload() const {
  QJsonObject response = makeSuccess();
  response.insert(QStringLiteral("revision"), static_cast<qint64>(editor_->revision().value));
  response.insert(QStringLiteral("playhead"), encodeTime(session_.playhead));
  response.insert(QStringLiteral("active_sequence_id"),
                  stdToQstr(session_.active_sequence_id.toString()));
  QJsonArray selected;
  for (const edit::EntityId& clip_id : session_.selected_clip_ids) {
    selected.append(stdToQstr(clip_id.toString()));
  }
  response.insert(QStringLiteral("selected_clip_ids"), selected);
  const auto project = editor_->projectAt(editor_->revision());
  response.insert(QStringLiteral("project"), encodeProjectSummary(*project));
  return response;
}

QJsonObject AgentHost::compactSessionPayload() const {
  QJsonObject response = makeSuccess();
  response.insert(QStringLiteral("revision"), static_cast<qint64>(editor_->revision().value));
  response.insert(QStringLiteral("playhead"), encodeTime(session_.playhead));
  response.insert(QStringLiteral("active_sequence_id"),
                  stdToQstr(session_.active_sequence_id.toString()));
  QJsonArray selected;
  for (const edit::EntityId& clip_id : session_.selected_clip_ids) {
    selected.append(stdToQstr(clip_id.toString()));
  }
  response.insert(QStringLiteral("selected_clip_ids"), selected);
  return response;
}

const edit::Sequence* AgentHost::activeSequence() const {
  if (!editor_ || session_.active_sequence_id.isNil()) {
    return nullptr;
  }
  const auto project = editor_->projectAt(editor_->revision());
  return edit::findSequence(*project, session_.active_sequence_id);
}

QJsonObject AgentHost::methodOpenProject(const QJsonObject& request) {
  if (!request.contains(QStringLiteral("path"))) {
    return makeError(QStringLiteral("invalid_argument"), QStringLiteral("path is required"));
  }
  try {
    const std::filesystem::path checkpoint = pathFromUtf8(qstrToStd(request.value(
        QStringLiteral("path")).toString()));
    if (!std::filesystem::is_regular_file(checkpoint)) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("project file does not exist"));
    }

    const auto parent = checkpoint.parent_path();
    const auto staging = parent / (checkpoint.stem().string() + ".agent.staging.sqlite");
    removeSqliteSidecars(staging);
    std::filesystem::copy_file(checkpoint, staging,
                               std::filesystem::copy_options::overwrite_existing);

    store::ProjectStore probe(staging, store::OpenOptions{.create_if_missing = false,
                                                          .run_integrity_check = true,
                                                          .project_uuid = std::nullopt});
    const std::string project_uuid = probe.metadata().project_uuid;
    const auto working = agentWorkingPathForProject(parent, project_uuid);
    if (working != staging) {
      removeSqliteSidecars(working);
      std::filesystem::copy_file(staging, working,
                                 std::filesystem::copy_options::overwrite_existing);
      removeSqliteSidecars(staging);
    }

    auto opened_store = std::make_unique<store::ProjectStore>(
        working, store::OpenOptions{.create_if_missing = false,
                                    .run_integrity_check = true,
                                    .project_uuid = std::nullopt});
    const auto commands = opened_store->read_commands();
    const store::JournalEntry* snapshot_entry = latestProjectSnapshot(commands);
    if (snapshot_entry == nullptr) {
      return makeError(QStringLiteral("invalid_project"),
                       QStringLiteral("Project contains no readable model snapshot"));
    }
    const auto bytes = binaryPayload(*snapshot_entry);
    auto decoded = project_codec::deserialize_project(std::span<const std::byte>(bytes));
    if (!decoded) {
      return makeError(QStringLiteral("invalid_project"),
                       QString::fromStdString(decoded.error().message));
    }
    if (decoded.value().id.toString() != opened_store->metadata().project_uuid) {
      return makeError(QStringLiteral("invalid_project"),
                       QStringLiteral("Project identity does not match its database"));
    }

    installProject(std::move(decoded).value(), working, std::move(opened_store), checkpoint);
    dirty_ = true;
    return compactSessionPayload();
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("open_failed"), QString::fromUtf8(exception.what()));
  }
}

QJsonObject AgentHost::methodNewProject(const QJsonObject& request) {
  try {
    const std::string name = qstrToStd(request.value(QStringLiteral("name")).toString());
    edit::Project project = makeDefaultAgentProject(name);
    installProject(std::move(project), {}, nullptr, std::nullopt);
    dirty_ = false;
    return compactSessionPayload();
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("new_project_failed"), QString::fromUtf8(exception.what()));
  }
}

QJsonObject AgentHost::methodSaveProject(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  try {
    std::filesystem::path destination;
    if (request.contains(QStringLiteral("path"))) {
      destination = pathFromUtf8(qstrToStd(request.value(QStringLiteral("path")).toString()));
    } else if (checkpoint_path_.has_value()) {
      destination = *checkpoint_path_;
    } else {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("path is required when no checkpoint is open"));
    }

    if (destination.extension() != ".veproj") {
      destination += ".veproj";
    }

    if (!store_) {
      const auto project = editor_->projectAt(editor_->revision());
      working_path_ = agentWorkingPathForProject(destination.parent_path(), project->id.toString());
      removeSqliteSidecars(working_path_);
      store_ = std::make_unique<store::ProjectStore>(
          working_path_, store::OpenOptions{.project_uuid = project->id.toString()});
      const project_codec::ProjectBytes bytes = project_codec::serialize_project(*project);
      store_->append_command("project.snapshot.v6", std::span<const std::byte>(bytes), 0,
                             project_codec::kCurrentSchemaVersion);
      store_->update_heartbeat();
    } else {
      persistSnapshot("agent.save");
    }

    checkpoint_path_ = destination;
    const auto revision = store_->metadata().head_revision;
    store_->checkpoint_to(destination, revision);
    store_->mark_saved(revision);
    dirty_ = false;

    QJsonObject response = compactSessionPayload();
    response.insert(QStringLiteral("path"), stdToQstr(utf8FromPath(destination)));
    return response;
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("save_failed"), QString::fromUtf8(exception.what()));
  }
}

QJsonObject AgentHost::methodReadSession() {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  return sessionPayload();
}

QJsonObject AgentHost::methodSetSession(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  if (request.contains(QStringLiteral("playhead"))) {
    const auto playhead = decodeTime(request.value(QStringLiteral("playhead")), "playhead");
    if (!playhead) {
      return makeError(QStringLiteral("invalid_argument"),
                       QString::fromStdString(playhead.error()));
    }
    session_.playhead = playhead.value();
  }
  if (request.contains(QStringLiteral("active_sequence_id"))) {
    const auto sequence_id =
        edit::EntityId::parse(qstrToStd(request.value(QStringLiteral("active_sequence_id"))
                                            .toString()));
    if (!sequence_id.has_value()) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("active_sequence_id must be a valid UUID"));
    }
    const auto project = editor_->projectAt(editor_->revision());
    if (edit::findSequence(*project, *sequence_id) == nullptr) {
      return makeError(QStringLiteral("entity_not_found"),
                       QStringLiteral("active_sequence_id was not found"));
    }
    session_.active_sequence_id = *sequence_id;
  }
  if (request.contains(QStringLiteral("selected_clip_ids"))) {
    if (!request.value(QStringLiteral("selected_clip_ids")).isArray()) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("selected_clip_ids must be an array"));
    }
    session_.selected_clip_ids.clear();
    const QJsonArray array = request.value(QStringLiteral("selected_clip_ids")).toArray();
    for (const QJsonValue& value : array) {
      const auto clip_id = edit::EntityId::parse(qstrToStd(value.toString()));
      if (!clip_id.has_value()) {
        return makeError(QStringLiteral("invalid_argument"),
                         QStringLiteral("selected_clip_ids must contain valid UUIDs"));
      }
      session_.selected_clip_ids.push_back(*clip_id);
    }
  }
  return compactSessionPayload();
}

QJsonObject AgentHost::methodApplyCommands(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  if (!request.contains(QStringLiteral("commands")) ||
      !request.value(QStringLiteral("commands")).isArray()) {
    return makeError(QStringLiteral("invalid_argument"), QStringLiteral("commands array is required"));
  }
  const auto decoded =
      agent_protocol::decode_commands(request.value(QStringLiteral("commands")).toArray());
  if (!decoded) {
    return makeError(QStringLiteral("codec_error"), QString::fromStdString(decoded.error().message));
  }
  const QString batch_name = request.contains(QStringLiteral("batch_name"))
                                 ? request.value(QStringLiteral("batch_name")).toString()
                                 : QStringLiteral("agent.edit");
  const edit::Revision expected_revision = resolveExpectedRevision(request);
  const auto result = editor_->applyBatch(std::move(decoded).value(), expected_revision,
                                          qstrToStd(batch_name));
  if (!result) {
    return makeError(editErrorCodeName(result.error().code),
                     QString::fromStdString(result.error().message), result.error());
  }
  try {
    if (store_) {
      persistAndCheckpoint("agent.apply_commands");
    } else {
      dirty_ = true;
    }
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    (void)rollback;
    return makeError(QStringLiteral("persist_failed"), QString::fromUtf8(exception.what()));
  }
  return compactSessionPayload();
}

QJsonObject AgentHost::methodUndo(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  const edit::Revision expected_revision = resolveExpectedRevision(request);
  const auto result = editor_->undo(expected_revision);
  if (!result) {
    return makeError(editErrorCodeName(result.error().code),
                     QString::fromStdString(result.error().message), result.error());
  }
  try {
    if (store_) {
      persistAndCheckpoint("agent.undo");
    } else {
      dirty_ = true;
    }
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("persist_failed"), QString::fromUtf8(exception.what()));
  }
  return compactSessionPayload();
}

QJsonObject AgentHost::methodRedo(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  const edit::Revision expected_revision = resolveExpectedRevision(request);
  const auto result = editor_->redo(expected_revision);
  if (!result) {
    return makeError(editErrorCodeName(result.error().code),
                     QString::fromStdString(result.error().message), result.error());
  }
  try {
    if (store_) {
      persistAndCheckpoint("agent.redo");
    } else {
      dirty_ = true;
    }
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("persist_failed"), QString::fromUtf8(exception.what()));
  }
  return compactSessionPayload();
}

QJsonObject AgentHost::methodImportMedia(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  if (!request.contains(QStringLiteral("path"))) {
    return makeError(QStringLiteral("invalid_argument"), QStringLiteral("path is required"));
  }
  try {
    const std::filesystem::path path =
        pathFromUtf8(qstrToStd(request.value(QStringLiteral("path")).toString()));
    assets::AssetService service;
    auto imported = service.import(path);
    if (!imported) {
      return makeError(QStringLiteral("import_failed"),
                       QString::fromStdString(imported.error().message));
    }
    edit::Asset model_asset = assets::asset_from_record(imported.value());
    const edit::Revision expected_revision = resolveExpectedRevision(request);
    const auto apply_result = editor_->apply(
        edit::EditCommand{.operation = edit::AddAssetCommand{.asset = model_asset},
                          .coalescing_key = {}},
        expected_revision);
    if (!apply_result) {
      return makeError(editErrorCodeName(apply_result.error().code),
                       QString::fromStdString(apply_result.error().message),
                       apply_result.error());
    }
    playback::AssetPlaybackSources sources{
        .original = {.path = imported.value().uri, .video_stream_index = -1},
        .proxy = std::nullopt,
        .pts_map_path = std::nullopt};
    (void)registry_->register_asset(model_asset.id, std::move(sources));
    if (store_) {
      persistAndCheckpoint("agent.import_media");
    } else {
      dirty_ = true;
    }
    QJsonObject response = compactSessionPayload();
    response.insert(QStringLiteral("asset_id"), stdToQstr(model_asset.id.toString()));
    return response;
  } catch (const std::exception& exception) {
    return makeError(QStringLiteral("import_failed"), QString::fromUtf8(exception.what()));
  }
}

QJsonObject AgentHost::methodInsertMedia(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  if (!request.contains(QStringLiteral("mode"))) {
    return makeError(QStringLiteral("invalid_argument"), QStringLiteral("mode is required"));
  }
  const auto parsed_mode = parseInsertMode(request.value(QStringLiteral("mode")).toString());
  if (!parsed_mode) {
    return makeError(QStringLiteral("invalid_argument"), QString::fromStdString(parsed_mode.error()));
  }
  const edit::InsertMode insert_mode = parsed_mode.value();

  edit::EntityId asset_id{};
  if (request.contains(QStringLiteral("asset_id"))) {
    const auto parsed =
        edit::EntityId::parse(qstrToStd(request.value(QStringLiteral("asset_id")).toString()));
    if (!parsed.has_value()) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("asset_id must be a valid UUID"));
    }
    asset_id = *parsed;
  } else if (request.contains(QStringLiteral("path"))) {
    QJsonObject import_request;
    import_request.insert(QStringLiteral("method"), QStringLiteral("import_media"));
    import_request.insert(QStringLiteral("path"), request.value(QStringLiteral("path")));
    const QJsonObject import_response = methodImportMedia(import_request);
    if (!import_response.value(QStringLiteral("ok")).toBool()) {
      return import_response;
    }
    const auto parsed =
        edit::EntityId::parse(qstrToStd(import_response.value(QStringLiteral("asset_id"))
                                            .toString()));
    if (!parsed.has_value()) {
      return makeError(QStringLiteral("internal_error"),
                       QStringLiteral("import_media returned an invalid asset id"));
    }
    asset_id = *parsed;
  } else {
    return makeError(QStringLiteral("invalid_argument"),
                     QStringLiteral("path or asset_id is required"));
  }

  const edit::Sequence* sequence = activeSequence();
  if (sequence == nullptr) {
    return makeError(QStringLiteral("invalid_session"),
                     QStringLiteral("active_sequence_id is not set"));
  }

  const auto project = editor_->projectAt(editor_->revision());
  const edit::Asset* asset = edit::findAsset(*project, asset_id);
  if (asset == nullptr) {
    return makeError(QStringLiteral("entity_not_found"), QStringLiteral("asset was not found"));
  }

  const edit::Asset asset_copy = *asset;
  const edit::EntityId sequence_id = sequence->id;
  const edit::TimeRange source_range{edit::Time{0, asset_copy.duration.timescale()},
                                     asset_copy.duration};
  const edit::Time start = session_.playhead;
  const edit::Time duration = source_range.duration;
  const edit::TimeRange dest_range{start, duration};
  const bool still_overlay =
      insert_mode == edit::InsertMode::Ripple && isStillOverlayAsset(asset_copy);
  const edit::InsertMode clip_insert_mode =
      still_overlay ? edit::InsertMode::RejectOverlap : insert_mode;

  std::vector<edit::EditCommand> commands;
  std::optional<edit::EntityId> ensured_video_track_id;
  std::optional<edit::EntityId> ensured_audio_track_id;
  const auto ensure_track = [&](const edit::TrackKind kind, const std::string& default_name) {
    const bool exists = std::any_of(sequence->tracks.begin(), sequence->tracks.end(),
                                    [&](const edit::Track& track) { return track.kind == kind; });
    if (exists) {
      return;
    }
    edit::Track track;
    track.kind = kind;
    track.name = default_name;
    track.targeted = true;
    if (kind == edit::TrackKind::Video) {
      ensured_video_track_id = track.id;
    } else if (kind == edit::TrackKind::Audio) {
      ensured_audio_track_id = track.id;
    }
    commands.push_back({.operation = edit::AddTrackCommand{.sequence_id = sequence_id,
                                                            .track = track,
                                                            .index = std::nullopt},
                        .coalescing_key = {}});
  };
  ensure_track(edit::TrackKind::Video, "V1");
  ensure_track(edit::TrackKind::Audio, "A1");

  std::optional<edit::EntityId> video_track_id = ensured_video_track_id;
  std::optional<edit::EntityId> audio_track_id = ensured_audio_track_id;
  if (still_overlay) {
    bool has_targeted_video_track = false;
    for (const edit::Track& track : sequence->tracks) {
      if (!track.locked && track.targeted && track.kind == edit::TrackKind::Video) {
        has_targeted_video_track = true;
        if (!video_track_id.has_value() && !trackOverlapsRange(track, dest_range)) {
          video_track_id = track.id;
        }
      }
    }
    if (!video_track_id.has_value() && has_targeted_video_track) {
      edit::Track track;
      track.kind = edit::TrackKind::Video;
      track.targeted = true;
      const auto ordinal =
          1 + std::count_if(sequence->tracks.begin(), sequence->tracks.end(),
                            [](const edit::Track& item) {
                              return item.kind == edit::TrackKind::Video;
                            });
      track.name = std::string("Video ") + std::to_string(ordinal);
      video_track_id = track.id;
      std::size_t insert_index = 0;
      for (std::size_t index = 0; index < sequence->tracks.size(); ++index) {
        if (sequence->tracks[index].kind == edit::TrackKind::Video) {
          insert_index = index + 1;
        }
      }
      commands.push_back({.operation = edit::AddTrackCommand{.sequence_id = sequence_id,
                                                              .track = track,
                                                              .index = insert_index},
                          .coalescing_key = {}});
    }
  } else {
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
    if (asset_copy.has_video && !video_track_id.has_value()) {
      edit::Track track;
      track.kind = edit::TrackKind::Video;
      track.targeted = true;
      track.name = "V1";
      commands.push_back({.operation = edit::AddTrackCommand{.sequence_id = sequence_id,
                                                              .track = track,
                                                              .index = std::nullopt},
                          .coalescing_key = {}});
      video_track_id = track.id;
    }
    if (asset_copy.has_audio && !audio_track_id.has_value()) {
      edit::Track track;
      track.kind = edit::TrackKind::Audio;
      track.targeted = true;
      track.name = "A1";
      commands.push_back({.operation = edit::AddTrackCommand{.sequence_id = sequence_id,
                                                              .track = track,
                                                              .index = std::nullopt},
                          .coalescing_key = {}});
      audio_track_id = track.id;
    }
  }

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

  const edit::EntityId linked = edit::EntityId::generate();
  std::vector<edit::EntityId> inserted_clip_ids;
  const auto prepare_insert = [&](const std::optional<edit::EntityId> track_id,
                                  const edit::ClipKind clip_kind) {
    if (!track_id.has_value()) {
      return false;
    }
    edit::Clip clip;
    clip.asset_id = asset_copy.id;
    clip.kind = clip_kind;
    clip.name = asset_copy.name;
    clip.timeline_range = {start, duration};
    clip.source_range = source_range;
    if (asset_copy.has_video && asset_copy.has_audio) {
      clip.linked_group = linked;
    }
    inserted_clip_ids.push_back(clip.id);
    commands.push_back(
        {.operation = edit::InsertClipCommand{.sequence_id = sequence_id,
                                              .track_id = *track_id,
                                              .clip = std::move(clip),
                                              .mode = clip_insert_mode},
         .coalescing_key = {}});
    return true;
  };

  if (asset_copy.has_video && !prepare_insert(video_track_id, edit::ClipKind::Video)) {
    return makeError(QStringLiteral("invalid_track"),
                     QStringLiteral("No unlocked targeted video track is available"));
  }
  if (asset_copy.has_audio && !prepare_insert(audio_track_id, edit::ClipKind::Audio)) {
    return makeError(QStringLiteral("invalid_track"),
                     QStringLiteral("No unlocked targeted audio track is available"));
  }

  const edit::Revision expected_revision = resolveExpectedRevision(request);
  const auto result = editor_->applyBatch(std::move(commands), expected_revision, "agent.insert");
  if (!result) {
    return makeError(editErrorCodeName(result.error().code),
                     QString::fromStdString(result.error().message), result.error());
  }

  session_.selected_clip_ids = inserted_clip_ids;
  try {
    if (store_) {
      persistAndCheckpoint("agent.insert_media");
    } else {
      dirty_ = true;
    }
  } catch (const std::exception& exception) {
    const auto rollback = editor_->undo(editor_->revision());
    (void)rollback;
    return makeError(QStringLiteral("persist_failed"), QString::fromUtf8(exception.what()));
  }

  QJsonObject response = compactSessionPayload();
  QJsonArray clip_ids;
  for (const edit::EntityId& clip_id : inserted_clip_ids) {
    clip_ids.append(stdToQstr(clip_id.toString()));
  }
  response.insert(QStringLiteral("clip_ids"), clip_ids);
  return response;
}

QJsonObject AgentHost::methodGetPreviewFrame(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  edit::EntityId sequence_id = session_.active_sequence_id;
  if (request.contains(QStringLiteral("sequence_id"))) {
    const auto parsed =
        edit::EntityId::parse(qstrToStd(request.value(QStringLiteral("sequence_id")).toString()));
    if (!parsed.has_value()) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("sequence_id must be a valid UUID"));
    }
    sequence_id = *parsed;
  }
  edit::Time time = session_.playhead;
  if (request.contains(QStringLiteral("time"))) {
    const auto decoded = decodeTime(request.value(QStringLiteral("time")), "time");
    if (!decoded) {
      return makeError(QStringLiteral("invalid_argument"), QString::fromStdString(decoded.error()));
    }
    time = decoded.value();
  }

  auto snapshot_result = editor_->snapshot(sequence_id, editor_->revision());
  if (!snapshot_result) {
    return makeError(editErrorCodeName(snapshot_result.error().code),
                     QString::fromStdString(snapshot_result.error().message),
                     snapshot_result.error());
  }

  const std::uint64_t epoch = ++preview_epoch_;
  renderer_->begin_epoch(epoch);
  frame_provider_->begin_epoch(epoch);
  const render::PreviewProfile profile{
      .scale = render::PreviewScale::Full,
      .bypass_expensive_effects = true,
      .use_proxies = false,
  };
  const auto frame_result =
      renderer_->request_frame(snapshot_result.value(), time, profile, epoch);
  if (!frame_result) {
    return makeError(QStringLiteral("render_failed"),
                     QString::fromStdString(frame_result.error->message));
  }
  const auto* cpu_frame =
      std::get_if<std::shared_ptr<const render::CpuFrame>>(&frame_result.value->storage);
  if (cpu_frame == nullptr || *cpu_frame == nullptr) {
    return makeError(QStringLiteral("render_failed"),
                     QStringLiteral("Preview frame is not available as a CPU frame"));
  }

  const render::Rgba8Image rgba = render::cpu_frame_to_rgba8(**cpu_frame);
  const auto png = media::encode_png_rgba8(rgba.pixels, rgba.width, rgba.height);
  if (!png) {
    return makeError(QStringLiteral("encode_failed"), QString::fromStdString(png.error().message));
  }

  std::filesystem::path output_path;
  if (request.contains(QStringLiteral("output_path"))) {
    output_path = pathFromUtf8(qstrToStd(request.value(QStringLiteral("output_path")).toString()));
  } else {
    output_path = std::filesystem::temp_directory_path() /
                  ("video_editor_agent_preview_" + edit::EntityId::generate().toString() + ".png");
  }
  {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      return makeError(QStringLiteral("io_error"), QStringLiteral("Could not write preview PNG"));
    }
    output.write(reinterpret_cast<const char*>(png.value().data()),
                   static_cast<std::streamsize>(png.value().size()));
    if (!output) {
      return makeError(QStringLiteral("io_error"), QStringLiteral("Could not write preview PNG"));
    }
  }

  QJsonObject response = compactSessionPayload();
  response.insert(QStringLiteral("path"), stdToQstr(utf8FromPath(output_path)));
  response.insert(QStringLiteral("width"), rgba.width);
  response.insert(QStringLiteral("height"), rgba.height);
  if (request.value(QStringLiteral("include_base64")).toBool(false)) {
    const QByteArray bytes(reinterpret_cast<const char*>(png.value().data()),
                           static_cast<int>(png.value().size()));
    response.insert(QStringLiteral("base64"), QString::fromLatin1(bytes.toBase64()));
  }
  return response;
}

QJsonObject AgentHost::methodGetScopes(const QJsonObject& request) {
  if (!editor_) {
    return makeError(QStringLiteral("no_project"), QStringLiteral("No project is open"));
  }
  const QString detail = request.contains(QStringLiteral("detail"))
                             ? request.value(QStringLiteral("detail")).toString()
                             : QStringLiteral("summary");
  if (detail != QStringLiteral("summary") && detail != QStringLiteral("full")) {
    return makeError(QStringLiteral("invalid_argument"),
                     QStringLiteral("detail must be summary or full"));
  }

  edit::EntityId sequence_id = session_.active_sequence_id;
  if (request.contains(QStringLiteral("sequence_id"))) {
    const auto parsed =
        edit::EntityId::parse(qstrToStd(request.value(QStringLiteral("sequence_id")).toString()));
    if (!parsed.has_value()) {
      return makeError(QStringLiteral("invalid_argument"),
                       QStringLiteral("sequence_id must be a valid UUID"));
    }
    sequence_id = *parsed;
  }
  edit::Time time = session_.playhead;
  if (request.contains(QStringLiteral("time"))) {
    const auto decoded = decodeTime(request.value(QStringLiteral("time")), "time");
    if (!decoded) {
      return makeError(QStringLiteral("invalid_argument"), QString::fromStdString(decoded.error()));
    }
    time = decoded.value();
  }

  auto snapshot_result = editor_->snapshot(sequence_id, editor_->revision());
  if (!snapshot_result) {
    return makeError(editErrorCodeName(snapshot_result.error().code),
                     QString::fromStdString(snapshot_result.error().message),
                     snapshot_result.error());
  }

  const std::uint64_t epoch = ++preview_epoch_;
  renderer_->begin_epoch(epoch);
  frame_provider_->begin_epoch(epoch);
  const render::PreviewProfile profile{
      .scale = render::PreviewScale::Full,
      .bypass_expensive_effects = true,
      .use_proxies = false,
  };
  const auto frame_result =
      renderer_->request_frame(snapshot_result.value(), time, profile, epoch);
  if (!frame_result) {
    return makeError(QStringLiteral("render_failed"),
                     QString::fromStdString(frame_result.error->message));
  }
  const auto* cpu_frame =
      std::get_if<std::shared_ptr<const render::CpuFrame>>(&frame_result.value->storage);
  if (cpu_frame == nullptr || *cpu_frame == nullptr) {
    return makeError(QStringLiteral("render_failed"),
                     QStringLiteral("Preview frame is not available as a CPU frame"));
  }

  const render::ScopeAnalysis analysis = render::ScopeAnalyzer::analyze(**cpu_frame);
  QJsonObject response = compactSessionPayload();
  response.insert(QStringLiteral("source_width"), analysis.source_width);
  response.insert(QStringLiteral("source_height"), analysis.source_height);
  response.insert(QStringLiteral("waveform_column_count"), analysis.waveform_column_count);
  response.insert(QStringLiteral("histogram_r"), encodeUInt32Array(analysis.histogram_r));
  response.insert(QStringLiteral("histogram_g"), encodeUInt32Array(analysis.histogram_g));
  response.insert(QStringLiteral("histogram_b"), encodeUInt32Array(analysis.histogram_b));
  response.insert(QStringLiteral("histogram_luma"), encodeUInt32Array(analysis.histogram_luma));
  response.insert(QStringLiteral("waveform_min"), encodeFloatArray(analysis.waveform_min));
  response.insert(QStringLiteral("waveform_max"), encodeFloatArray(analysis.waveform_max));
  if (detail == QStringLiteral("full")) {
    response.insert(QStringLiteral("waveform"), encodeWaveform(analysis.waveform));
    response.insert(QStringLiteral("vectorscope"), encodeVectorscope(analysis.vectorscope));
  }
  return response;
}

} // namespace video_editor::agent_host
