// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/asset_service/asset_service.h"
#include "video_editor/edit_model/model.h"

namespace video_editor::assets {

[[nodiscard]] edit::Asset asset_from_record(const AssetRecord& record);

} // namespace video_editor::assets
