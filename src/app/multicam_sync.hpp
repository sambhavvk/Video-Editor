// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace video_editor::app {

struct MulticamSyncProposal final {
  edit::EntityId angle_id;
  edit::Time sync_offset{};
  bool has_timecode{false};
  bool ambiguous{false};
  std::string note;
};

struct MulticamTimecodeSyncProposal final {
  edit::Time sync_reference{};
  std::vector<MulticamSyncProposal> angles;
  bool requires_manual_choice{false};
};

[[nodiscard]] std::optional<std::int64_t> assetTimecodeMicroseconds(const edit::Asset& asset);
[[nodiscard]] MulticamTimecodeSyncProposal
proposeMulticamTimecodeSync(const edit::Project& project, const edit::Sequence& sequence,
                            const edit::MulticamGroup& group, edit::Time sync_reference);

} // namespace video_editor::app
