// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <google/protobuf/message_lite.h>

#include <cstdint>
#include <iosfwd>
#include <string>

namespace video_editor::jobs {

enum class ReadStatus : std::uint8_t { Ok, EndOfStream, InvalidFrame, IoError };

struct ProtocolResult {
  bool ok{false};
  ReadStatus status{ReadStatus::IoError};
  std::string message;
};

[[nodiscard]] ProtocolResult write_frame(std::ostream& output,
                                         const google::protobuf::MessageLite& message);
[[nodiscard]] ProtocolResult read_frame(std::istream& input,
                                        google::protobuf::MessageLite& message,
                                        std::uint32_t maximum_bytes = 64U * 1024U * 1024U);

} // namespace video_editor::jobs

