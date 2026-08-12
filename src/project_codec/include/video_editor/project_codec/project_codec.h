// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "video_editor/edit_model/model.h"
#include "video_editor/edit_model/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace video_editor::project_codec {

inline constexpr std::uint32_t kCurrentSchemaVersion = 2;
inline constexpr std::uint32_t kMinimumReaderVersion = 1;
inline constexpr std::size_t kMaximumSnapshotBytes = 256U * 1024U * 1024U;

using ProjectBytes = std::vector<std::byte>;

enum class CodecErrorCode {
  InvalidProject,
  SerializationFailed,
  MalformedProtobuf,
  UnsupportedSchemaVersion,
  UnsupportedMinimumReaderVersion,
  MissingField,
  InvalidField,
  DuplicateId,
};

struct CodecError final {
  CodecErrorCode code{CodecErrorCode::InvalidField};
  std::string message;
  std::string field_path;
};

class CodecException final : public std::runtime_error {
public:
  explicit CodecException(CodecError error);

  [[nodiscard]] const CodecError& error() const noexcept {
    return error_;
  }

private:
  CodecError error_;
};

// Produces canonical protobuf bytes. Invalid model state is rejected with a
// CodecException rather than being written into a project checkpoint.
[[nodiscard]] ProjectBytes serialize_project(const edit::Project& project);

// Parses and validates an entire snapshot. No partially decoded project is
// returned on failure.
[[nodiscard]] edit::Result<edit::Project, CodecError>
deserialize_project(std::span<const std::byte> bytes);

} // namespace video_editor::project_codec
