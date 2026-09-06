// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/commands.h"
#include "video_editor/edit_model/result.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

#include <string>
#include <vector>

namespace video_editor::agent_protocol {

struct CodecError {
  std::string message;
};

[[nodiscard]] QJsonObject encode_command(const edit::EditCommand& command);
[[nodiscard]] edit::Result<edit::EditCommand, CodecError> decode_command(
    const QJsonObject& object);

[[nodiscard]] QByteArray encode_command_bytes(const edit::EditCommand& command);
[[nodiscard]] edit::Result<edit::EditCommand, CodecError> decode_command_bytes(
    const QByteArray& bytes);

[[nodiscard]] QJsonArray encode_commands(const std::vector<edit::EditCommand>& commands);
[[nodiscard]] edit::Result<std::vector<edit::EditCommand>, CodecError> decode_commands(
    const QJsonArray& array);

}  // namespace video_editor::agent_protocol
