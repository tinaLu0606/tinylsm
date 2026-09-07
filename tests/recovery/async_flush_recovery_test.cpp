#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "db/db_test_peer.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"

namespace {

using tinylsm::test::FaultOperation;
using tinylsm::test::TempDir;

tinylsm::Options AsyncFlushOptions() {
  tinylsm::Options options;
  options.memtable_bytes = 128;
  options.sstable_block_bytes = 40;
  return options;
}

TEST(AsyncFlushRecoveryTest, BackgroundFailureIsStickyAndImmutableWalRecovers) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  plan->Fail(FaultOperation::kOpenWritable, ".sst.tmp");

  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), AsyncFlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  // Rotation durably publishes WAL 1 as immutable and WAL 2 as active before
  // the worker starts its SSTable I/O, so this acknowledged write is recoverable.
  ASSERT_TRUE(opened.value()->Put("durable", std::string(96, 'v')).ok());
  const auto flush =
      tinylsm::internal::DBTestPeer::WaitForBackgroundFlush(*opened.value());
  EXPECT_EQ(flush.code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Get("durable").status().code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Close().code(), tinylsm::StatusCode::kIOError);

  const auto metrics = opened.value()->GetWriteMetrics();
  EXPECT_EQ(metrics.memtable_rotations, 1U);
  EXPECT_EQ(metrics.background_flush_failures, 1U);
  EXPECT_EQ(metrics.max_background_queue_depth, 1U);
  opened.value().reset();

  // A fresh process reads immutable WAL 1 before active WAL 2 and can finish
  // its delayed flush without exposing a partial SSTable.
  auto recovered = tinylsm::DB::Open(dir.path(), AsyncFlushOptions());
  ASSERT_TRUE(recovered.ok()) << recovered.status().ToString();
  ASSERT_TRUE(recovered.value()->Get("durable").ok());
  EXPECT_TRUE(recovered.value()->Close().ok());

  auto reopened = tinylsm::DB::Open(dir.path(), AsyncFlushOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  auto value = reopened.value()->Get("durable");
  ASSERT_TRUE(value.ok()) << value.status().ToString();
  EXPECT_EQ(value.value(), std::string(96, 'v'));
}

} // namespace
