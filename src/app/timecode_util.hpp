// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/time.h"

#include <QString>
#include <optional>

namespace video_editor::app {

[[nodiscard]] QString formatTimecode(edit::Time time, const edit::Rate& rate);
[[nodiscard]] std::optional<edit::Time> parseTimecodeInput(const QString& text,
                                                            const edit::Rate& rate,
                                                            std::uint32_t ui_timescale);

} // namespace video_editor::app
