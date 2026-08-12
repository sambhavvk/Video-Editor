// SPDX-License-Identifier: MPL-2.0
#include "video_editor/job_service/cancellation_registry.h"
#include "video_editor/job_service/framing.h"
#include "video_editor/job_service/job_id.h"
#include "video_editor/job_service/protocol.h"

#include <gtest/gtest.h>

#include <sstream>

namespace video_editor::jobs {
namespace {

TEST(JobId, GeneratesValidDistinctIdentifiers) {
  const std::string first = make_job_id();
  const std::string second = make_job_id();
  EXPECT_TRUE(valid_job_id(first));
  EXPECT_TRUE(valid_job_id(second));
  EXPECT_NE(first, second);
}

TEST(Framing, RoundTripsVersionedWorkerRequest) {
  v1::WorkerRequest request;
  request.set_protocol_major(kProtocolMajor);
  request.set_protocol_minor(kProtocolMinor);
  request.mutable_start()->mutable_spec()->set_job_id(make_job_id());
  request.mutable_start()->mutable_spec()->set_kind(v1::JOB_KIND_PROXY);
  request.mutable_start()->mutable_spec()->set_project_revision(42);

  std::stringstream bytes(std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(write_frame(bytes, request).ok);
  v1::WorkerRequest decoded;
  ASSERT_TRUE(read_frame(bytes, decoded).ok);
  EXPECT_TRUE(compatible(decoded));
  EXPECT_EQ(decoded.start().spec().project_revision(), 42U);
  EXPECT_EQ(decoded.start().spec().kind(), v1::JOB_KIND_PROXY);
}

TEST(Framing, RejectsOversizedFrameBeforeAllocation) {
  std::string bytes(4, '\0');
  bytes[0] = 32;
  std::stringstream input(bytes, std::ios::in | std::ios::binary);
  v1::WorkerRequest decoded;
  const ProtocolResult result = read_frame(input, decoded, 16);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.status, ReadStatus::InvalidFrame);
}

TEST(CancellationRegistry, SharesCancellationWithRunningJob) {
  CancellationRegistry registry;
  const std::string id = make_job_id();
  auto token = registry.begin(id);
  EXPECT_EQ(registry.active_count(), 1U);
  EXPECT_FALSE(token->load());
  EXPECT_TRUE(registry.cancel(id));
  EXPECT_TRUE(token->load());
  registry.finish(id);
  EXPECT_EQ(registry.active_count(), 0U);
}

} // namespace
} // namespace video_editor::jobs

