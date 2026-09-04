#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "db/db_test_peer.h"
#include "db/filename.h"
#include "manifest/manifest_state.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"
#include "tinylsm/options.h"

namespace {

using tinylsm::test::FaultOperation;
using tinylsm::test::FaultPlan;
using tinylsm::test::TempDir;

tinylsm::Options FlushEveryWriteOptions() {
  tinylsm::Options options;
  options.memtable_bytes = 1;
  options.sstable_block_bytes = 40;
  return options;
}

void CreateFlushedDatabase(const std::filesystem::path& path,
                           std::size_t table_count = 2) {
  auto opened = tinylsm::DB::Open(path, FlushEveryWriteOptions());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  for (std::size_t i = 0; i < table_count; ++i) {
    ASSERT_TRUE(opened.value()
                    ->Put("key-" + std::to_string(i), "value-" + std::to_string(i))
                    .ok());
  }
  ASSERT_TRUE(opened.value()->Close().ok());
}

void WriteJunk(const std::filesystem::path& path) {
  std::ofstream(path, std::ios::binary).write("junk", 4);
}

} // namespace

TEST(CleanupRecoveryTest, OpenRemovesOnlyCanonicalUnreferencedFiles) {
  TempDir dir;
  CreateFlushedDatabase(dir.path());

  const std::vector<std::string> obsolete{"000001.wal", "000003.wal", "000006.sst",
                                          "000007.sst.tmp", "MANIFEST.tmp"};
  const std::vector<std::string> preserved{"1.wal",       "000000.wal",
                                           "0000001.sst", "000006.sst.bak",
                                           "notes.txt",   "18446744073709551616.sst"};
  for (const auto& name : obsolete)
    WriteJunk(dir.path() / name);
  for (const auto& name : preserved)
    WriteJunk(dir.path() / name);

  auto reopened = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_EQ(reopened.value()->Get("key-1").value(), "value-1");
  for (const auto& name : obsolete) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / name));
  }
  for (const auto& name : preserved) {
    SCOPED_TRACE(name);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / name));
  }
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000002.sst"));
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000004.sst"));
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000005.wal"));
}

TEST(CleanupRecoveryTest, ListDirFailureDoesNotBlockRecoveredData) {
  TempDir dir;
  CreateFlushedDatabase(dir.path(), 1);
  WriteJunk(dir.path() / "000001.wal");
  auto plan = std::make_shared<FaultPlan>();
  plan->Fail(FaultOperation::kListDir);

  auto reopened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushEveryWriteOptions(),
      tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000001.wal"));
}

TEST(CleanupRecoveryTest, RemoveFailureIsRetriedOnTheNextOpen) {
  TempDir dir;
  CreateFlushedDatabase(dir.path(), 1);
  const auto orphan = dir.path() / "000001.wal";
  WriteJunk(orphan);
  auto plan = std::make_shared<FaultPlan>();
  plan->Fail(FaultOperation::kRemove, "000001.wal");

  auto reopened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushEveryWriteOptions(),
      tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_TRUE(std::filesystem::exists(orphan));
  ASSERT_TRUE(reopened.value()->Close().ok());
  reopened.value().reset();

  auto retry = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(retry.ok()) << retry.status().ToString();
  EXPECT_FALSE(std::filesystem::exists(orphan));
  EXPECT_EQ(retry.value()->Get("key-0").value(), "value-0");
}

TEST(CleanupRecoveryTest, SyncDirFailureDoesNotBlockRecoveredData) {
  TempDir dir;
  CreateFlushedDatabase(dir.path(), 1);
  const auto orphan = dir.path() / "000001.wal";
  WriteJunk(orphan);
  auto plan = std::make_shared<FaultPlan>();
  plan->Fail(FaultOperation::kSyncDir);

  auto reopened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushEveryWriteOptions(),
      tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key-0").value(), "value-0");
  EXPECT_FALSE(std::filesystem::exists(orphan));
}

TEST(CleanupRecoveryTest, FlushRetriesPendingWalRemoval) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushEveryWriteOptions(),
      tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Fail(FaultOperation::kRemove, "000001.wal");

  ASSERT_TRUE(opened.value()->Put("first", "value").ok());
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000001.wal"));
  ASSERT_TRUE(opened.value()->Put("second", "value").ok());
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "000001.wal"));
  EXPECT_EQ(opened.value()->Get("first").value(), "value");
  EXPECT_EQ(opened.value()->Get("second").value(), "value");
}

TEST(CleanupRecoveryTest, CompactionRetriesPendingTableRemoval) {
  TempDir dir;
  CreateFlushedDatabase(dir.path(), 3);
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Fail(FaultOperation::kRemove, "000002.sst");

  ASSERT_TRUE(opened.value()->Compact().ok());
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000002.sst"));
  ASSERT_TRUE(opened.value()->Compact().ok());
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "000002.sst"));
  EXPECT_EQ(opened.value()->Get("key-0").value(), "value-0");
  EXPECT_EQ(opened.value()->Get("key-2").value(), "value-2");
}

TEST(CleanupRecoveryTest, RepeatedMaintenanceLeavesOnlyManifestLiveFiles) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path(), FlushEveryWriteOptions());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  WriteJunk(dir.path() / "notes.txt");
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(opened.value()
                    ->Put("key-" + std::to_string(i), "value-" + std::to_string(i))
                    .ok());
  }
  ASSERT_TRUE(opened.value()->Compact().ok());

  auto fs = tinylsm::internal::NewPosixFileSystem();
  auto manifest = tinylsm::internal::ManifestState::Load(*fs, dir.path());
  ASSERT_TRUE(manifest.ok()) << manifest.status().ToString();
  std::set<std::string> expected{
      tinylsm::internal::WalFileName(manifest.value().active_wal_number)};
  for (const auto& table : manifest.value().live_tables)
    expected.insert(tinylsm::internal::SstableFileName(table.file_number));

  for (const auto& path : std::filesystem::directory_iterator(dir.path())) {
    const auto name = path.path().filename().string();
    if (tinylsm::internal::ParseNumberedFileName(name))
      EXPECT_TRUE(expected.erase(name)) << name;
  }
  EXPECT_TRUE(expected.empty());
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "notes.txt"));
}
