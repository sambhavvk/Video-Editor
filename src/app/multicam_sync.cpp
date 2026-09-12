// SPDX-License-Identifier: MPL-2.0
#include "multicam_sync.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>

namespace video_editor::app {
namespace {

[[nodiscard]] std::optional<std::int64_t> parseMicrosecondsMetadata(
    const std::map<std::string, std::string, std::less<>>& metadata, const char* key) {
  const auto found = metadata.find(key);
  if (found == metadata.end()) {
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(found->second, &consumed);
    if (consumed != found->second.size()) {
      return std::nullopt;
    }
    return parsed;
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
  struct AngleClock final {
    std::size_t index{0};
    edit::Time clock{};
  };
  std::vector<AngleClock> clocks;
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
    clocks.push_back(AngleClock{
        .index = result.angles.size(),
        .clock = edit::Time(*timecode, 1'000'000) + clip->source_range.start,
    });
    result.angles.push_back(proposal);
  }

  if (clocks.empty()) {
    result.requires_manual_choice = true;
    return result;
  }

  edit::Time earliest = clocks.front().clock;
  for (const AngleClock& entry : clocks) {
    if (entry.clock < earliest) {
      earliest = entry.clock;
    }
  }
  for (const AngleClock& entry : clocks) {
    MulticamSyncProposal& proposal = result.angles[entry.index];
    proposal.sync_offset = entry.clock - earliest;
    proposal.note = "timecode aligned to earliest source clock";
  }
  return result;
}

} // namespace video_editor::app
