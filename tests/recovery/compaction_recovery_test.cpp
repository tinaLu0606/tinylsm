#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "db/db_test_peer.h"
#include "manifest/manifest_state.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"
#include "tinylsm/options.h"

namespace {

using tinylsm::internal::ManifestSnapshot;
using tinylsm::test::FaultOperation;
using tinylsm::test::FaultPlan;
using tinylsm::test::FaultTiming;
using tinylsm::test::TempDir;

tinylsm::Options FlushEveryWriteOptions() {
  tinylsm::Options options;
  options.memtable_bytes = 1;
  options.sstable_block_bytes = 40;
  options.compaction_table_trigger = 0;
  return options;
}

tinylsm::Result<ManifestSnapshot> LoadManifest(const std::filesystem::path& path) {
  auto fs = tinylsm::internal::NewPosixFileSystem();
  return tinylsm::internal::ManifestState::Load(*fs, path);
}

std::size_t CountSuffix(const std::filesystem::path& path, std::string_view suffix) {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(path))
    if (entry.path().filename().string().ends_with(suffix))
      ++count;
  return count;
}

void CreateVersionedTables(const std::filesystem::path& path) {
  auto opened = tinylsm::DB::Open(path, FlushEveryWriteOptions());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  ASSERT_TRUE(opened.value()->Put("a", "old-a").ok());
  ASSERT_TRUE(opened.value()->Put("b", "live-b").ok());
  ASSERT_TRUE(opened.value()->Put("a", "new-a").ok());
  ASSERT_TRUE(opened.value()->Close().ok());
}

struct CompactionFaultCase {
  FaultOperation operation;
  const char* suffix;
  FaultTiming timing = FaultTiming::kBefore;
};

} // namespace

TEST(DBCompactionTest, ZeroTablesIsANoOpAndClosedHandlesAreRejected) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  const auto before = LoadManifest(dir.path());
  ASSERT_TRUE(before.ok());

  EXPECT_TRUE(opened.value()->Compact().ok());
  auto after = LoadManifest(dir.path());
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value(), before.value());

  ASSERT_TRUE(opened.value()->Close().ok());
  EXPECT_EQ(opened.value()->Compact().code(), tinylsm::StatusCode::kAlreadyClosed);
}

TEST(DBCompactionTest, RewritesOneTableWithoutChangingTheActiveWal) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  ASSERT_TRUE(opened.value()->Put("key", "value").ok());
  ASSERT_TRUE(
      tinylsm::internal::DBTestPeer::WaitForBackgroundFlush(*opened.value()).ok());
  auto before = LoadManifest(dir.path());
  ASSERT_TRUE(before.ok());
  ASSERT_EQ(before.value().live_tables.size(), 1U);

  ASSERT_TRUE(opened.value()->Compact().ok());
  auto after = LoadManifest(dir.path());
  ASSERT_TRUE(after.ok());
  ASSERT_EQ(after.value().live_tables.size(), 1U);
  EXPECT_EQ(after.value().active_wal_number, before.value().active_wal_number);
  EXPECT_EQ(after.value().last_sequence, before.value().last_sequence);
  EXPECT_EQ(after.value().live_tables.front().file_number,
            before.value().next_file_number);
  EXPECT_EQ(after.value().next_file_number, before.value().next_file_number + 1);
  EXPECT_EQ(CountSuffix(dir.path(), ".wal"), 1U);
  EXPECT_EQ(CountSuffix(dir.path(), ".sst"), 1U);
  EXPECT_EQ(opened.value()->Get("key").value(), "value");
}

TEST(DBCompactionTest, DropsAllTombstonesWithoutConsumingAFileNumber) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  ASSERT_TRUE(opened.value()->Delete("gone").ok());
  ASSERT_TRUE(
      tinylsm::internal::DBTestPeer::WaitForBackgroundFlush(*opened.value()).ok());
  auto before = LoadManifest(dir.path());
  ASSERT_TRUE(before.ok());
  ASSERT_EQ(before.value().live_tables.size(), 1U);

  ASSERT_TRUE(opened.value()->Compact().ok());
  auto after = LoadManifest(dir.path());
  ASSERT_TRUE(after.ok());
  EXPECT_TRUE(after.value().live_tables.empty());
  EXPECT_EQ(after.value().active_wal_number, before.value().active_wal_number);
  EXPECT_EQ(after.value().last_sequence, before.value().last_sequence);
  EXPECT_EQ(after.value().next_file_number, before.value().next_file_number);
  EXPECT_EQ(CountSuffix(dir.path(), ".sst"), 0U);
  EXPECT_EQ(opened.value()->Get("gone").status().code(),
            tinylsm::StatusCode::kNotFound);

  ASSERT_TRUE(opened.value()->Close().ok());
  auto reopened = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("gone").status().code(),
            tinylsm::StatusCode::kNotFound);
}

TEST(DBCompactionTest, PreservesLogicalStateAndNewerMemtableAcrossReopen) {
  TempDir dir;
  auto flushing = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(flushing.ok()) << flushing.status().ToString();
  ASSERT_TRUE(flushing.value()->Put("a", "old-a").ok());
  ASSERT_TRUE(flushing.value()->Put("b", "old-b").ok());
  ASSERT_TRUE(flushing.value()->Put("a", "new-a").ok());
  ASSERT_TRUE(flushing.value()->Delete("b").ok());
  ASSERT_TRUE(flushing.value()->Put("c", "live-c").ok());
  ASSERT_TRUE(flushing.value()->Close().ok());

  tinylsm::Options large_memtable;
  large_memtable.sstable_block_bytes = 40;
  large_memtable.compaction_table_trigger = 0;
  auto opened = tinylsm::DB::Open(dir.path(), large_memtable);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  auto before = LoadManifest(dir.path());
  ASSERT_TRUE(before.ok());
  ASSERT_EQ(before.value().live_tables.size(), 5U);
  ASSERT_TRUE(opened.value()->Put("memory", "wal-only").ok());
  auto before_scan = opened.value()->Scan("", "");
  ASSERT_TRUE(before_scan.ok()) << before_scan.status().ToString();

  ASSERT_TRUE(opened.value()->Compact().ok());
  auto after = LoadManifest(dir.path());
  ASSERT_TRUE(after.ok());
  ASSERT_EQ(after.value().live_tables.size(), 1U);
  EXPECT_EQ(after.value().active_wal_number, before.value().active_wal_number);
  EXPECT_EQ(after.value().last_sequence, before.value().last_sequence);
  EXPECT_EQ(after.value().next_file_number, before.value().next_file_number + 1);
  EXPECT_EQ(CountSuffix(dir.path(), ".wal"), 1U);
  EXPECT_EQ(CountSuffix(dir.path(), ".sst"), 1U);
  auto after_scan = opened.value()->Scan("", "");
  ASSERT_TRUE(after_scan.ok()) << after_scan.status().ToString();
  EXPECT_EQ(after_scan.value(), before_scan.value());

  ASSERT_TRUE(opened.value()->Close().ok());
  auto reopened = tinylsm::DB::Open(dir.path(), large_memtable);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  auto reopened_scan = reopened.value()->Scan("", "");
  ASSERT_TRUE(reopened_scan.ok()) << reopened_scan.status().ToString();
  EXPECT_EQ(reopened_scan.value(), before_scan.value());
}

TEST(CompactionRecoveryTest, FailuresBeforeManifestRenameKeepOldState) {
  const std::vector<CompactionFaultCase> cases{
      {FaultOperation::kReadAt, ".sst"},
      {FaultOperation::kOpenWritable, ".sst.tmp"},
      {FaultOperation::kAppend, ".sst.tmp"},
      {FaultOperation::kSync, ".sst.tmp"},
      {FaultOperation::kClose, ".sst.tmp", FaultTiming::kAfter},
      {FaultOperation::kOpenRandomAccess, ".sst.tmp"},
      {FaultOperation::kReadAt, ".sst.tmp"},
      {FaultOperation::kRename, ".sst"},
      {FaultOperation::kSyncDir, ""},
      {FaultOperation::kOpenWritable, "MANIFEST.tmp"},
      {FaultOperation::kAppend, "MANIFEST.tmp"},
      {FaultOperation::kSync, "MANIFEST.tmp"},
      {FaultOperation::kClose, "MANIFEST.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, "MANIFEST"},
  };

  for (const auto& fault : cases) {
    SCOPED_TRACE(fault.suffix);
    TempDir dir;
    CreateVersionedTables(dir.path());
    auto before = LoadManifest(dir.path());
    ASSERT_TRUE(before.ok());
    auto plan = std::make_shared<FaultPlan>();
    auto opened = tinylsm::internal::DBTestPeer::Open(
        dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    plan->Fail(fault.operation, fault.suffix, 1, fault.timing);

    auto status = opened.value()->Compact();
    EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
    auto after = LoadManifest(dir.path());
    ASSERT_TRUE(after.ok()) << after.status().ToString();
    EXPECT_EQ(after.value(), before.value());
    EXPECT_EQ(opened.value()->Get("a").value(), "new-a");
    EXPECT_EQ(opened.value()->Get("b").value(), "live-b");
    EXPECT_TRUE(opened.value()->Close().ok());
    opened.value().reset();

    auto reopened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    EXPECT_EQ(reopened.value()->Get("a").value(), "new-a");
    EXPECT_EQ(reopened.value()->Get("b").value(), "live-b");
  }
}

TEST(CompactionRecoveryTest, ManifestSyncDirFailureFreezesUntilReopen) {
  TempDir dir;
  CreateVersionedTables(dir.path());
  auto before = LoadManifest(dir.path());
  ASSERT_TRUE(before.ok());
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Fail(FaultOperation::kSyncDir, "", 2);

  auto status = opened.value()->Compact();
  EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(status.message().find("close and reopen"), std::string::npos);
  EXPECT_EQ(opened.value()->Get("a").status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Scan("", "").status().code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Put("later", "value").code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Delete("a").code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Compact().code(), tinylsm::StatusCode::kIOError);

  auto published = LoadManifest(dir.path());
  ASSERT_TRUE(published.ok()) << published.status().ToString();
  ASSERT_EQ(published.value().live_tables.size(), 1U);
  EXPECT_EQ(published.value().active_wal_number, before.value().active_wal_number);
  EXPECT_EQ(published.value().last_sequence, before.value().last_sequence);
  EXPECT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").value(), "new-a");
  EXPECT_EQ(reopened.value()->Get("b").value(), "live-b");
}

TEST(CompactionRecoveryTest, CleanupExceptionDoesNotChangeCommittedSuccess) {
  TempDir dir;
  CreateVersionedTables(dir.path());
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Throw(FaultOperation::kRemove, ".sst");

  EXPECT_TRUE(opened.value()->Compact().ok());
  auto published = LoadManifest(dir.path());
  ASSERT_TRUE(published.ok()) << published.status().ToString();
  ASSERT_EQ(published.value().live_tables.size(), 1U);
  EXPECT_EQ(opened.value()->Get("a").value(), "new-a");
  EXPECT_EQ(opened.value()->Get("b").value(), "live-b");
}

TEST(BackgroundCompactionTest, DropsOnlySafeOldestPrefixTombstoneAndReportsRawMetrics) {
  TempDir dir;
  tinylsm::Options options = FlushEveryWriteOptions();
  options.sync_on_write = false;
  options.compaction_table_trigger = 2;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  ASSERT_TRUE(opened.value()->Put("gone", "old").ok());
  ASSERT_TRUE(
      tinylsm::internal::DBTestPeer::WaitForBackgroundFlush(*opened.value()).ok());
  ASSERT_TRUE(opened.value()->Delete("gone").ok());
  ASSERT_TRUE(
      tinylsm::internal::DBTestPeer::WaitForBackgroundWork(*opened.value()).ok());

  const auto compaction = opened.value()->GetCompactionMetrics();
  EXPECT_EQ(compaction.compactions, 1U);
  EXPECT_EQ(compaction.background_compactions, 1U);
  EXPECT_EQ(compaction.compaction_input_tables, 2U);
  EXPECT_EQ(compaction.compaction_output_tables, 0U);
  EXPECT_GT(compaction.compaction_input_bytes, 0U);
  EXPECT_EQ(compaction.compaction_output_bytes, 0U);
  EXPECT_EQ(compaction.table_count, 0U);
  EXPECT_EQ(compaction.compaction_debt_tables, 0U);
  EXPECT_GT(compaction.flush_output_bytes, 0U);

  const auto writes = opened.value()->GetWriteMetrics();
  EXPECT_EQ(writes.logical_write_bytes, 11U);
  EXPECT_EQ(opened.value()->Get("gone").status().code(),
            tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->GetReadMetrics().point_lookups, 1U);
  ASSERT_TRUE(opened.value()->Close().ok());

  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("gone").status().code(),
            tinylsm::StatusCode::kNotFound);
}

TEST(BackgroundCompactionRecoveryTest, FailedCommitKeepsTheOldestPrefixRecoverable) {
  TempDir dir;
  {
    auto source = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
    ASSERT_TRUE(source.ok()) << source.status().ToString();
    ASSERT_TRUE(source.value()->Put("key-0", "value-0").ok());
    ASSERT_TRUE(source.value()->Put("key-1", "value-1").ok());
    ASSERT_TRUE(source.value()->Close().ok());
  }
  auto plan = std::make_shared<FaultPlan>();
  plan->Fail(FaultOperation::kRename, "MANIFEST", 2);
  tinylsm::Options options = FlushEveryWriteOptions();
  options.compaction_table_trigger = 2;
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), options, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  const auto waited =
      tinylsm::internal::DBTestPeer::WaitForBackgroundWork(*opened.value());
  EXPECT_EQ(waited.code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Get("key-0").status().code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Close().code(), tinylsm::StatusCode::kIOError);
  opened.value().reset();

  options.compaction_table_trigger = 0;
  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_EQ(reopened.value()->Get("key-1").value(), "value-1");
}

TEST(BackgroundCompactionRecoveryTest, ManifestSyncDirFailureFreezesUntilReopen) {
  TempDir dir;
  {
    auto source = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
    ASSERT_TRUE(source.ok()) << source.status().ToString();
    ASSERT_TRUE(source.value()->Put("key-0", "value-0").ok());
    ASSERT_TRUE(source.value()->Put("key-1", "value-1").ok());
    ASSERT_TRUE(source.value()->Close().ok());
  }
  auto plan = std::make_shared<FaultPlan>();
  // Reservation Manifest sync, output SST directory sync, then replacement
  // Manifest directory sync.
  plan->Fail(FaultOperation::kSyncDir, "", 3);
  tinylsm::Options options = FlushEveryWriteOptions();
  options.compaction_table_trigger = 2;
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), options, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  const auto waited =
      tinylsm::internal::DBTestPeer::WaitForBackgroundWork(*opened.value());
  EXPECT_EQ(waited.code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(waited.message().find("close and reopen"), std::string::npos);
  EXPECT_EQ(opened.value()->Scan("", "").status().code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  options.compaction_table_trigger = 0;
  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_EQ(reopened.value()->Get("key-1").value(), "value-1");
}

TEST(BackgroundCompactionTest, LongMixedWorkloadMatchesReferenceAcrossReopen) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 4U * 1024U;
  options.sstable_block_bytes = 512;
  options.sync_on_write = false;
  options.compaction_table_trigger = 4;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  std::map<std::string, std::string, std::less<>> reference;
  for (std::size_t i = 0; i < 5'000; ++i) {
    const auto key = "key-" + std::to_string((i * 37) % 257);
    if (i % 11 == 0) {
      ASSERT_TRUE(opened.value()->Delete(key).ok());
      reference.erase(key);
    } else {
      const auto value = "value-" + std::to_string(i);
      ASSERT_TRUE(opened.value()->Put(key, value).ok());
      reference[key] = value;
    }

    if (i % 17 == 0) {
      const auto expected = reference.find(key);
      const auto actual = opened.value()->Get(key);
      if (expected == reference.end()) {
        EXPECT_EQ(actual.status().code(), tinylsm::StatusCode::kNotFound);
      } else {
        ASSERT_TRUE(actual.ok()) << actual.status().ToString();
        EXPECT_EQ(actual.value(), expected->second);
      }
    }
    if (i % 251 == 0) {
      const auto actual = opened.value()->Scan("", "");
      ASSERT_TRUE(actual.ok()) << actual.status().ToString();
      ASSERT_EQ(actual.value().size(), reference.size());
      auto expected = reference.begin();
      for (const auto& entry : actual.value()) {
        EXPECT_EQ(entry.key, expected->first);
        EXPECT_EQ(entry.value, expected->second);
        ++expected;
      }
    }
  }

  ASSERT_TRUE(
      tinylsm::internal::DBTestPeer::WaitForBackgroundWork(*opened.value()).ok());
  ASSERT_TRUE(opened.value()->Close().ok());
  options.compaction_table_trigger = 0;
  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  const auto actual = reopened.value()->Scan("", "");
  ASSERT_TRUE(actual.ok()) << actual.status().ToString();
  ASSERT_EQ(actual.value().size(), reference.size());
  auto expected = reference.begin();
  for (const auto& entry : actual.value()) {
    EXPECT_EQ(entry.key, expected->first);
    EXPECT_EQ(entry.value, expected->second);
    ++expected;
  }
}
