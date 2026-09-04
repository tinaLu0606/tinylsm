#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "db/db_test_peer.h"
#include "manifest/manifest_state.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"

namespace {

using tinylsm::internal::ManifestPublishState;
using tinylsm::test::FaultOperation;
using tinylsm::test::FaultPlan;
using tinylsm::test::FaultTiming;
using tinylsm::test::TempDir;

tinylsm::Options FlushOptions() {
  tinylsm::Options options;
  options.memtable_bytes = 128;
  options.sstable_block_bytes = 40;
  return options;
}

tinylsm::internal::ManifestSnapshot OldSnapshot() {
  tinylsm::internal::ManifestSnapshot snapshot;
  snapshot.active_wal_number = 1;
  snapshot.next_file_number = 2;
  snapshot.last_sequence = 4;
  return snapshot;
}

tinylsm::internal::ManifestSnapshot NextSnapshot() {
  tinylsm::internal::ManifestSnapshot snapshot;
  snapshot.active_wal_number = 3;
  snapshot.next_file_number = 4;
  snapshot.last_sequence = 7;
  snapshot.live_tables.push_back(tinylsm::internal::TableMeta{2, 100, "a", "z", 1, 7});
  return snapshot;
}

struct PublishFaultCase {
  FaultOperation operation;
  const char* suffix;
  FaultTiming timing = FaultTiming::kBefore;
};

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(ManifestPublishTest, FailuresBeforeRenameAreNotPublished) {
  const std::vector<PublishFaultCase> cases{
      {FaultOperation::kOpenWritable, "MANIFEST.tmp"},
      {FaultOperation::kAppend, "MANIFEST.tmp"},
      {FaultOperation::kSync, "MANIFEST.tmp"},
      {FaultOperation::kClose, "MANIFEST.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, "MANIFEST"},
  };

  for (const auto& fault : cases) {
    SCOPED_TRACE(fault.suffix);
    TempDir dir;
    auto plan = std::make_shared<FaultPlan>();
    auto fs = tinylsm::test::NewFaultInjectionFileSystem(plan);
    ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
    plan->Fail(fault.operation, fault.suffix, 1, fault.timing);

    const auto old_snapshot = OldSnapshot();
    tinylsm::internal::ManifestState manifest(*fs, dir.path(), old_snapshot);
    auto outcome = manifest.Publish(NextSnapshot());

    EXPECT_EQ(outcome.state(), ManifestPublishState::kNotPublished);
    EXPECT_EQ(outcome.status().code(), tinylsm::StatusCode::kIOError);
    EXPECT_EQ(manifest.current().active_wal_number, old_snapshot.active_wal_number);
  }
}

TEST(ManifestPublishTest, RenameFollowedBySyncDirFailureIsVisibleNotDurable) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto fs = tinylsm::test::NewFaultInjectionFileSystem(plan);
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  plan->Fail(FaultOperation::kSyncDir);

  const auto old_snapshot = OldSnapshot();
  const auto next_snapshot = NextSnapshot();
  tinylsm::internal::ManifestState manifest(*fs, dir.path(), old_snapshot);
  auto outcome = manifest.Publish(next_snapshot);

  EXPECT_EQ(outcome.state(), ManifestPublishState::kVisibleNotDurable);
  EXPECT_EQ(outcome.status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(manifest.current().active_wal_number, old_snapshot.active_wal_number);
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "MANIFEST"));

  auto real_fs = tinylsm::internal::NewPosixFileSystem();
  auto loaded = tinylsm::internal::ManifestState::Load(*real_fs, dir.path());
  ASSERT_TRUE(loaded.ok()) << loaded.status().ToString();
  EXPECT_EQ(loaded.value().active_wal_number, next_snapshot.active_wal_number);
}

TEST(ManifestPublishTest, DurablePublicationUpdatesCurrentSnapshot) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());

  tinylsm::internal::ManifestState manifest(*fs, dir.path(), OldSnapshot());
  auto outcome = manifest.Publish(NextSnapshot());

  EXPECT_TRUE(outcome.durable());
  EXPECT_TRUE(outcome.status().ok());
  EXPECT_EQ(manifest.current().active_wal_number, 3U);
}

TEST(FlushRecoveryTest, PreManifestRenameFailuresKeepOldStateRecoverable) {
  const std::vector<PublishFaultCase> cases{
      {FaultOperation::kOpenWritable, ".sst.tmp"},
      {FaultOperation::kAppend, ".sst.tmp"},
      {FaultOperation::kSync, ".sst.tmp"},
      {FaultOperation::kClose, ".sst.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, ".sst"},
      {FaultOperation::kSyncDir, ""},
      {FaultOperation::kOpenWritable, "000003.wal"},
      {FaultOperation::kSync, "000003.wal"},
      {FaultOperation::kOpenWritable, "MANIFEST.tmp"},
      {FaultOperation::kAppend, "MANIFEST.tmp"},
      {FaultOperation::kSync, "MANIFEST.tmp"},
      {FaultOperation::kClose, "MANIFEST.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, "MANIFEST"},
  };

  for (const auto& fault : cases) {
    SCOPED_TRACE(fault.suffix);
    TempDir dir;
    auto plan = std::make_shared<FaultPlan>();
    auto opened = tinylsm::internal::DBTestPeer::Open(
        dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    plan->Fail(fault.operation, fault.suffix, 1, fault.timing);

    auto status = opened.value()->Put("key", std::string(64, 'v'));
    EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
    auto current = opened.value()->Get("key");
    ASSERT_TRUE(current.ok()) << current.status().ToString();
    EXPECT_EQ(current.value(), std::string(64, 'v'));
    EXPECT_TRUE(opened.value()->Close().ok());
    opened.value().reset();

    auto reopened = tinylsm::DB::Open(dir.path(), FlushOptions());
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    auto recovered = reopened.value()->Get("key");
    ASSERT_TRUE(recovered.ok()) << recovered.status().ToString();
    EXPECT_EQ(recovered.value(), std::string(64, 'v'));
  }
}

TEST(FlushRecoveryTest, SecondFlushFailuresKeepPublishedTableAndWalRecoverable) {
  const std::vector<PublishFaultCase> cases{
      {FaultOperation::kOpenWritable, ".sst.tmp"},
      {FaultOperation::kAppend, ".sst.tmp"},
      {FaultOperation::kSync, ".sst.tmp"},
      {FaultOperation::kClose, ".sst.tmp", FaultTiming::kAfter},
      {FaultOperation::kOpenRandomAccess, ".sst.tmp"},
      {FaultOperation::kReadAt, ".sst.tmp"},
      {FaultOperation::kRename, ".sst"},
      {FaultOperation::kSyncDir, ""},
      {FaultOperation::kOpenWritable, "000005.wal"},
      {FaultOperation::kSync, "000005.wal"},
      {FaultOperation::kOpenWritable, "MANIFEST.tmp"},
      {FaultOperation::kAppend, "MANIFEST.tmp"},
      {FaultOperation::kSync, "MANIFEST.tmp"},
      {FaultOperation::kClose, "MANIFEST.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, "MANIFEST"},
  };

  for (const auto& fault : cases) {
    SCOPED_TRACE(fault.suffix);
    TempDir dir;
    auto plan = std::make_shared<FaultPlan>();
    auto opened = tinylsm::internal::DBTestPeer::Open(
        dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    ASSERT_TRUE(opened.value()->Put("old", std::string(64, 'o')).ok());
    plan->Fail(fault.operation, fault.suffix, 1, fault.timing);

    auto status = opened.value()->Put("new", std::string(64, 'n'));
    EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
    EXPECT_EQ(opened.value()->Get("old").value(), std::string(64, 'o'));
    EXPECT_EQ(opened.value()->Get("new").value(), std::string(64, 'n'));

    auto real_fs = tinylsm::internal::NewPosixFileSystem();
    auto manifest = tinylsm::internal::ManifestState::Load(*real_fs, dir.path());
    ASSERT_TRUE(manifest.ok()) << manifest.status().ToString();
    EXPECT_EQ(manifest.value().live_tables.size(), 1U);

    EXPECT_TRUE(opened.value()->Close().ok());
    opened.value().reset();
    auto reopened = tinylsm::DB::Open(dir.path(), FlushOptions());
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    EXPECT_EQ(reopened.value()->Get("old").value(), std::string(64, 'o'));
    EXPECT_EQ(reopened.value()->Get("new").value(), std::string(64, 'n'));
  }
}

TEST(FlushRecoveryTest, ManifestSyncDirFailureFreezesDataOperationsUntilReopen) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  ASSERT_TRUE(opened.value()->Put("old", std::string(64, 'o')).ok());
  plan->Fail(FaultOperation::kSyncDir, "", 2);

  const auto status = opened.value()->Put("key", std::string(64, 'v'));
  EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(status.message().find("close and reopen"), std::string::npos);
  EXPECT_EQ(opened.value()->Get("key").status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Put("later", "value").code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Delete("key").code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Scan("", "").status().code(),
            tinylsm::StatusCode::kIOError);

  EXPECT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path(), FlushOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("old").value(), std::string(64, 'o'));
  EXPECT_EQ(reopened.value()->Get("key").value(), std::string(64, 'v'));
  EXPECT_EQ(reopened.value()->Get("later").status().code(),
            tinylsm::StatusCode::kNotFound);
}

TEST(FlushRecoveryTest, TerminalCloseReportsOnlyItsOwnFailureAndCanRetry) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Fail(FaultOperation::kSyncDir, "", 2);
  ASSERT_EQ(opened.value()->Put("key", std::string(64, 'v')).code(),
            tinylsm::StatusCode::kIOError);

  plan->Fail(FaultOperation::kSync, "000001.wal");
  auto first_close = opened.value()->Close();
  EXPECT_EQ(first_close.code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(first_close.message().find("sync WAL"), std::string::npos);
  EXPECT_EQ(opened.value()->Get("key").status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_TRUE(opened.value()->Close().ok());
}

TEST(FlushRecoveryTest, InitialManifestSyncDirFailureCanBeRecoveredByOpen) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  plan->Fail(FaultOperation::kSyncDir);
  auto first = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_FALSE(first.ok());
  EXPECT_EQ(first.status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(first.status().message().find("retry DB::Open"), std::string::npos);

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_TRUE(reopened.value()->Put("key", "value").ok());
}

TEST(FlushRecoveryTest, InitialCreationFailuresCanBeRetriedSafely) {
  const std::vector<PublishFaultCase> cases{
      {FaultOperation::kOpenWritable, "000001.wal"},
      {FaultOperation::kSync, "000001.wal"},
      {FaultOperation::kClose, "000001.wal", FaultTiming::kAfter},
      {FaultOperation::kOpenWritable, "MANIFEST.tmp"},
      {FaultOperation::kAppend, "MANIFEST.tmp"},
      {FaultOperation::kSync, "MANIFEST.tmp"},
      {FaultOperation::kClose, "MANIFEST.tmp", FaultTiming::kAfter},
      {FaultOperation::kRename, "MANIFEST"},
  };

  for (const auto& fault : cases) {
    SCOPED_TRACE(fault.suffix);
    TempDir dir;
    auto plan = std::make_shared<FaultPlan>();
    plan->Fail(fault.operation, fault.suffix, 1, fault.timing);
    auto first = tinylsm::internal::DBTestPeer::Open(
        dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
    ASSERT_FALSE(first.ok());
    EXPECT_EQ(first.status().code(), tinylsm::StatusCode::kIOError);

    auto reopened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    EXPECT_TRUE(reopened.value()->Put("key", "value").ok());
    EXPECT_TRUE(reopened.value()->Close().ok());
  }
}

TEST(FlushRecoveryTest, NonEmptyPreManifestWalIsRejectedWithoutModification) {
  TempDir dir;
  std::filesystem::create_directories(dir.path());
  const auto wal = dir.path() / "000001.wal";
  const std::string original("legacy\0wal", 10);
  std::ofstream(wal, std::ios::binary)
      .write(original.data(), static_cast<std::streamsize>(original.size()));

  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kNotSupported);
  EXPECT_EQ(ReadFile(wal), original);
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "MANIFEST"));
}

TEST(FlushRecoveryTest, ComplexPreManifestStatesAreRejectedAsCorruption) {
  for (const std::string name :
       {"000002.wal", "000001.sst", "000001.sst.tmp", "MANIFEST.tmp"}) {
    SCOPED_TRACE(name);
    TempDir dir;
    std::filesystem::create_directories(dir.path());
    std::ofstream file(dir.path() / name, std::ios::binary);
    ASSERT_TRUE(file.is_open());

    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kCorruption);
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "MANIFEST"));
  }
}

TEST(FlushRecoveryTest, UnrelatedFilesDoNotBlockInitialCreation) {
  TempDir dir;
  std::filesystem::create_directories(dir.path());
  std::ofstream(dir.path() / "notes.txt") << "unrelated";

  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  EXPECT_TRUE(opened.value()->Put("key", "value").ok());
  EXPECT_EQ(ReadFile(dir.path() / "notes.txt"), "unrelated");
}

TEST(FlushRecoveryTest, OldWalCleanupFailureDoesNotChangeCommittedSuccess) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Fail(FaultOperation::kClose, "000001.wal", 1, FaultTiming::kAfter);
  plan->Fail(FaultOperation::kRemove, "000001.wal");

  EXPECT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "000001.wal"));
  EXPECT_EQ(opened.value()->Get("key").value(), std::string(64, 'v'));
  EXPECT_TRUE(opened.value()->Put("next", std::string(64, 'n')).ok());
  EXPECT_EQ(opened.value()->Get("next").value(), std::string(64, 'n'));
  EXPECT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path(), FlushOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key").value(), std::string(64, 'v'));
  EXPECT_EQ(reopened.value()->Get("next").value(), std::string(64, 'n'));
}

TEST(FlushRecoveryTest, OldWalCloseExceptionDoesNotChangeCommittedSuccess) {
  TempDir dir;
  auto plan = std::make_shared<FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), FlushOptions(), tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  plan->Throw(FaultOperation::kClose, "000001.wal");

  EXPECT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
  EXPECT_EQ(opened.value()->Get("key").value(), std::string(64, 'v'));
  EXPECT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path(), FlushOptions());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key").value(), std::string(64, 'v'));
}
