// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/framing.h"

#include <array>
#include <istream>
#include <ostream>
#include <string>

namespace video_editor::jobs {

ProtocolResult write_frame(std::ostream& output,
                           const google::protobuf::MessageLite& message) {
  const std::size_t size = message.ByteSizeLong();
  if (size > static_cast<std::size_t>(UINT32_MAX)) {
    return {.ok = false, .status = ReadStatus::InvalidFrame, .message = "message is too large"};
  }
  const auto size32 = static_cast<std::uint32_t>(size);
  const std::array<char, 4> header{
      static_cast<char>(size32 & 0xFFU),
      static_cast<char>((size32 >> 8U) & 0xFFU),
      static_cast<char>((size32 >> 16U) & 0xFFU),
      static_cast<char>((size32 >> 24U) & 0xFFU),
  };
  output.write(header.data(), static_cast<std::streamsize>(header.size()));
  if (!message.SerializeToOstream(&output)) {
    return {.ok = false, .status = ReadStatus::IoError, .message = "cannot serialize message"};
  }
  output.flush();
  if (!output) {
    return {.ok = false, .status = ReadStatus::IoError, .message = "cannot write protocol frame"};
  }
  return {.ok = true, .status = ReadStatus::Ok, .message = {}};
}

ProtocolResult read_frame(std::istream& input, google::protobuf::MessageLite& message,
                          const std::uint32_t maximum_bytes) {
  std::array<unsigned char, 4> header{};
  input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  if (input.gcount() == 0 && input.eof()) {
    return {.ok = false, .status = ReadStatus::EndOfStream, .message = "end of stream"};
  }
  if (input.gcount() != static_cast<std::streamsize>(header.size())) {
    return {.ok = false, .status = ReadStatus::IoError, .message = "truncated frame header"};
  }
  const std::uint32_t size = static_cast<std::uint32_t>(header[0]) |
                             (static_cast<std::uint32_t>(header[1]) << 8U) |
                             (static_cast<std::uint32_t>(header[2]) << 16U) |
                             (static_cast<std::uint32_t>(header[3]) << 24U);
  if (size > maximum_bytes) {
    return {.ok = false, .status = ReadStatus::InvalidFrame,
            .message = "frame exceeds configured size limit"};
  }
  std::string payload(size, '\0');
  input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
  if (input.gcount() != static_cast<std::streamsize>(payload.size())) {
    return {.ok = false, .status = ReadStatus::IoError, .message = "truncated frame payload"};
  }
  if (!message.ParseFromString(payload)) {
    return {.ok = false, .status = ReadStatus::InvalidFrame,
            .message = "payload is not a valid protocol message"};
  }
  return {.ok = true, .status = ReadStatus::Ok, .message = {}};
}

} // namespace video_editor::jobs

