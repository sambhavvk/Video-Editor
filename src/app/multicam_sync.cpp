// SPDX-License-Identifier: MPL-2.0
#include "multicam_sync.hpp"

#include <algorithm>
#include <limits>

namespace video_editor::app {
namespace {

[[nodiscard]] std::optional<std::int64_t> parseMicrosecondsMetadata(
    const std::map<std::string, std::string, std::less<>>& metadata, const char* key) {
  const auto found = metadata.find(key);
  if (found == metadata.end()) {
    return std::nullopt;
  }
  try {
    return std::stoll(found->second);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

} // namespace

std::optional<std::int64_t> assetTimecodeMicroseconds(const edit::Asset& asset) {
  if (const auto direct = parseMicrosecondsMetadata(asset.metadata, "timecode_start_us")) {
    return direct;
  }
  return parseMicrosecondsMetadata(asset.metadata, "stream_timecode_start_us");
}

MulticamTimecodeSyncProposal proposeMulticamTimecodeSync(const edit::Project& project,
                                                         const edit::Sequence& sequence,
                                                         const edit::MulticamGroup& group,
                                                         const edit::Time sync_reference) {
  MulticamTimecodeSyncProposal result;
  result.sync_reference = sync_reference;
  std::optional<std::int64_t> reference_microseconds;
  for (const edit::MulticamAngle& angle : group.angles) {
    MulticamSyncProposal proposal;
    proposal.angle_id = angle.id;
    proposal.sync_offset = angle.sync_offset;
    const edit::Clip* clip = edit::findClip(sequence, angle.clip_id);
    if (clip == nullptr) {
      proposal.ambiguous = true;
      proposal.note = "missing clip";
      result.angles.push_back(proposal);
      continue;
    }
    const edit::Asset* asset = edit::findAsset(project, clip->asset_id);
    if (asset == nullptr) {
      proposal.ambiguous = true;
      proposal.note = "missing asset";
      result.angles.push_back(proposal);
      continue;
    }
    const auto timecode = assetTimecodeMicroseconds(*asset);
    if (!timecode.has_value()) {
      proposal.note = "no timecode";
      result.requires_manual_choice = true;
      result.angles.push_back(proposal);
      continue;
    }
    proposal.has_timecode = true;
    if (!reference_microseconds.has_value() ||
        *timecode < *reference_microseconds) {
      reference_microseconds = timecode;
    }
    result.angles.push_back(proposal);
  }

  if (!reference_microseconds.has_value()) {
    result.requires_manual_choice = true;
    return result;
  }

  for (MulticamSyncProposal& proposal : result.angles) {
    if (!proposal.has_timecode) {
      continue;
    }
    const edit::MulticamAngle* angle = edit::findMulticamAngle(group, proposal.angle_id);
    const edit::Clip* clip =
        angle == nullptr ? nullptr : edit::findClip(sequence, angle->clip_id);
    if (clip == nullptr) {
      continue;
    }
    const edit::Asset* asset = edit::findAsset(project, clip->asset_id);
    const auto timecode = asset == nullptr ? std::nullopt : assetTimecodeMicroseconds(*asset);
    if (!timecode.has_value()) {
      continue;
    }
    proposal.sync_offset =
        clip->timeline_range.start - sync_reference -
        edit::Time(*timecode - *reference_microseconds, 1'000'000);
    proposal.note = "timecode aligned to earliest source clock";
  }
  return result;
}

} // namespace video_editor::app
