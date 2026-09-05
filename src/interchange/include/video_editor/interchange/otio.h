// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace video_editor::interchange {

struct OtioReport final {
  std::vector<std::string> warnings;
  std::vector<std::string> skipped;
};

// Export one sequence from `project` as OTIO Timeline JSON (UTF-8).
// Nested sequences referenced by NestedSequence clips are embedded inline as
// Stack.1 children. Effects, LUTs, mixer DSP, and captions are written under
// metadata.video_editor and are not required for round-trip media identity.
[[nodiscard]] edit::Result<std::string, std::string> export_otio_json(
    const edit::Project& project, edit::EntityId sequence_id, OtioReport* report = nullptr);

// Import OTIO Timeline JSON into a fresh Project containing the timeline's
// sequences, assets, and markers. Unknown vendor effect plugins are skipped
// with a report entry. Import fails closed when a composable would drop media
// identity (for example a Clip.1 without ExternalReference and without a
// nested Stack child).
[[nodiscard]] edit::Result<edit::Project, std::string> import_otio_json(
    std::string_view json, OtioReport* report = nullptr);

}  // namespace video_editor::interchange
