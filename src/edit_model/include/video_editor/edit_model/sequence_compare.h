// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"

#include <string>
#include <vector>

namespace video_editor::edit {

enum class SequenceChangeKind { ClipAdded, ClipRemoved, ClipMoved, Unsupported };

struct SequenceChange final {
  SequenceChangeKind kind{SequenceChangeKind::Unsupported};
  std::string clip_name;
  std::string detail;
};

[[nodiscard]] std::vector<SequenceChange> compare_sequences(const Sequence& before,
                                                              const Sequence& after);

} // namespace video_editor::edit
