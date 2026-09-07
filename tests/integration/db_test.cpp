#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

#include "db/db_test_peer.h"
#include "io/file.h"
#include "manifest/manifest_state.h"
#include "sstable/sstable_builder.h"
#include "sstable/sstable_format.h"
#include "sstable/sstable_reader.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"
#include "util/coding.h"
#include "wal/wal_writer.h"

namespace {
using tinylsm::test::FaultOperation;
using tinylsm::test::FaultTiming;
using tinylsm::test::TempDir;

void CorruptByte(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.is_open());
  file.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  file.read(&byte, 1);
  ASSERT_TRUE(file.good());
  byte ^= 1;
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(&byte, 1);
  ASSERT_TRUE(file.good());
}

std::string NumberedName(std::uint64_t number, std::string_view suffix) {
  std::ostringstream output;
  output << std::setw(6) << std::setfill('0') << number << suffix;
  return output.str();
}

tinylsm::Result<tinylsm::internal::TableMeta>
BuildTable(tinylsm::internal::FileSystem& fs, const std::filesystem::path& directory,
           std::uint64_t number,
           const std::vector<tinylsm::internal::InternalEntry>& entries) {
  auto writable = fs.OpenWritable(directory / NumberedName(number, ".sst"), false);
  if (!writable.ok())
    return writable.status();

  tinylsm::internal::SSTableBuilder builder(std::move(writable.value()), 40);
  for (const auto& entry : entries) {
    auto status = builder.Add(entry);
    if (!status.ok())
      return status;
  }
  auto built = builder.Finish();
  if (!built.ok())
    return built.status();
  return tinylsm::internal::TableMeta{number,
                                      built.value().file_size,
                                      built.value().smallest_key,
                                      built.value().largest_key,
                                      built.value().min_sequence,
                                      built.value().max_sequence};
}

tinylsm::Status CreateEmptyWal(tinylsm::internal::FileSystem& fs,
                               const std::filesystem::path& directory,
                               std::uint64_t number) {
  auto writable = fs.OpenWritable(directory / NumberedName(number, ".wal"), false);
  if (!writable.ok())
    return writable.status();
  auto status = writable.value()->Sync();
  if (!status.ok())
    return status;
  return writable.value()->Close();
}

tinylsm::Status WriteWal(tinylsm::internal::FileSystem& fs,
                         const std::filesystem::path& directory, std::uint64_t number,
                         const std::vector<tinylsm::internal::InternalEntry>& entries) {
  auto writable = fs.OpenWritable(directory / NumberedName(number, ".wal"), false);
  if (!writable.ok())
    return writable.status();
  tinylsm::internal::WalWriter writer(std::move(writable.value()), {});
  for (const auto& entry : entries) {
    auto status = writer.Append(entry);
    if (!status.ok())
      return status;
  }
  auto status = writer.Sync();
  if (!status.ok())
    return status;
  return writer.Close();
}
} // namespace

TEST(SstableTest, BuildsMultipleBlocksAndReadsBoundaryKeys) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  auto writable = fs->OpenWritable(dir.path() / "table.sst", false);
  ASSERT_TRUE(writable.ok());
  tinylsm::internal::SSTableBuilder builder(std::move(writable.value()), 40);
  ASSERT_TRUE(
      builder.Add({"a", 1, tinylsm::internal::ValueType::kValue, std::string(20, 'a')})
          .ok());
  ASSERT_TRUE(
      builder.Add({"m", 2, tinylsm::internal::ValueType::kValue, std::string(20, 'm')})
          .ok());
  ASSERT_TRUE(
      builder.Add({"z", 3, tinylsm::internal::ValueType::kValue, std::string(20, 'z')})
          .ok());
  ASSERT_TRUE(builder.Finish().ok());
  auto random = fs->OpenRandomAccess(dir.path() / "table.sst");
  ASSERT_TRUE(random.ok());
  auto reader = tinylsm::internal::SSTableReader::Open(std::move(random.value()));
  ASSERT_TRUE(reader.ok());
  auto properties = reader.value()->ValidateAndGetProperties();
  ASSERT_TRUE(properties.ok()) << properties.status().ToString();
  EXPECT_EQ(properties.value().file_size,
            std::filesystem::file_size(dir.path() / "table.sst"));
  EXPECT_EQ(properties.value().smallest_key, "a");
  EXPECT_EQ(properties.value().largest_key, "z");
  EXPECT_EQ(properties.value().min_sequence, 1U);
  EXPECT_EQ(properties.value().max_sequence, 3U);
  EXPECT_EQ(reader.value()->Get("a").value().sequence, 1U);
  EXPECT_EQ(reader.value()->Get("m").value().sequence, 2U);
  EXPECT_EQ(reader.value()->Get("z").value().sequence, 3U);
  auto scan = reader.value()->Scan("m", "zz");
  ASSERT_TRUE(scan.ok());
  ASSERT_EQ(scan.value().size(), 2U);
  EXPECT_EQ(scan.value().front().user_key, "m");
  EXPECT_EQ(scan.value().back().user_key, "z");
}

TEST(SstableTest, PropertiesRejectIndexMismatchEmptyTableAndZeroSequence) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());

  auto data = tinylsm::internal::EncodeDataBlock(
      {{"a", 1, tinylsm::internal::ValueType::kValue, "one"},
       {"z", 2, tinylsm::internal::ValueType::kValue, "two"}});
  ASSERT_TRUE(data.ok());
  auto index = tinylsm::internal::EncodeIndex(
      {{"b", "z", 0, static_cast<std::uint64_t>(data.value().size())}});
  ASSERT_TRUE(index.ok());
  const auto footer =
      tinylsm::internal::EncodeFooter({data.value().size(), index.value().size()});
  {
    std::ofstream output(dir.path() / "mismatch.sst", std::ios::binary);
    output << data.value() << index.value() << footer;
  }
  auto mismatch_file = fs->OpenRandomAccess(dir.path() / "mismatch.sst");
  ASSERT_TRUE(mismatch_file.ok());
  auto mismatch =
      tinylsm::internal::SSTableReader::Open(std::move(mismatch_file.value()));
  ASSERT_TRUE(mismatch.ok());
  EXPECT_EQ(mismatch.value()->ValidateAndGetProperties().status().code(),
            tinylsm::StatusCode::kCorruption);

  auto empty_index = tinylsm::internal::EncodeIndex({});
  ASSERT_TRUE(empty_index.ok());
  const auto empty_footer =
      tinylsm::internal::EncodeFooter({0, empty_index.value().size()});
  {
    std::ofstream output(dir.path() / "empty.sst", std::ios::binary);
    output << empty_index.value() << empty_footer;
  }
  auto empty_file = fs->OpenRandomAccess(dir.path() / "empty.sst");
  ASSERT_TRUE(empty_file.ok());
  auto empty = tinylsm::internal::SSTableReader::Open(std::move(empty_file.value()));
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(empty.value()->ValidateAndGetProperties().status().code(),
            tinylsm::StatusCode::kCorruption);

  auto zero = BuildTable(*fs, dir.path(), 2,
                         {{"key", 0, tinylsm::internal::ValueType::kValue, "value"}});
  ASSERT_TRUE(zero.ok());
  auto zero_file = fs->OpenRandomAccess(dir.path() / "000002.sst");
  ASSERT_TRUE(zero_file.ok());
  auto zero_reader =
      tinylsm::internal::SSTableReader::Open(std::move(zero_file.value()));
  ASSERT_TRUE(zero_reader.ok());
  EXPECT_EQ(zero_reader.value()->ValidateAndGetProperties().status().code(),
            tinylsm::StatusCode::kCorruption);
}

TEST(FileSystemTest, WritableFileRetriesShortWritesAndPropagatesPathErrors) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystemForTesting(
      [](int fd, const void* data, std::size_t size) {
        return static_cast<std::ptrdiff_t>(
            ::write(fd, data, std::min<std::size_t>(size, 1)));
      });
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  const auto path = dir.path() / "short-write";
  auto missing = fs->FileExists(path);
  ASSERT_TRUE(missing.ok());
  EXPECT_FALSE(missing.value());
  auto writable = fs->OpenWritable(path, false);
  ASSERT_TRUE(writable.ok());
  const std::string expected = "complete despite short writes";
  ASSERT_TRUE(writable.value()->Append(tinylsm::internal::AsBytes(expected)).ok());
  ASSERT_TRUE(writable.value()->Sync().ok());
  ASSERT_TRUE(writable.value()->Close().ok());
  std::ifstream input(path, std::ios::binary);
  const std::string actual{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  EXPECT_EQ(actual, expected);
  auto exists = fs->FileExists(path);
  ASSERT_TRUE(exists.ok());
  EXPECT_TRUE(exists.value());
  EXPECT_EQ(fs->FileExists(dir.path() / std::string(5000, 'x')).status().code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(fs->Rename(dir.path() / "missing", dir.path() / "target").code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(fs->SyncDir(dir.path() / "missing").code(), tinylsm::StatusCode::kIOError);
}

TEST(DBErrorTest, FileExistsFailureIsPropagatedWithOpenContext) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  plan->Fail(FaultOperation::kFileExists);
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_NE(opened.status().message().find("open database"), std::string::npos);
}

TEST(DBErrorTest, SyncFailureLeavesDatabaseOpenAndCloseCanBeRetried) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  plan->Fail(FaultOperation::kSync, ".wal");
  EXPECT_EQ(opened.value()->Close().code(), tinylsm::StatusCode::kIOError);
  EXPECT_TRUE(opened.value()->Put("still-open", "value").ok());
  EXPECT_TRUE(opened.value()->Close().ok());
}

TEST(DBErrorTest, CloseFailureStillTransitionsDatabaseToClosed) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  plan->Fail(FaultOperation::kClose, ".wal", 1, FaultTiming::kAfter);
  EXPECT_EQ(opened.value()->Close().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Get("key").status().code(),
            tinylsm::StatusCode::kAlreadyClosed);
}

TEST(DBErrorTest, StandardExceptionsPropagateThroughThePublicApi) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  plan->Throw(FaultOperation::kAppend, ".wal");
  EXPECT_THROW(static_cast<void>(opened.value()->Put("key", "value")),
               std::runtime_error);
  EXPECT_TRUE(opened.value()->Close().ok());
}

TEST(DBErrorTest, AppendFailureChangesNeitherWalNorMemtable) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  const auto wal = dir.path() / "000001.wal";
  const auto size_before = std::filesystem::file_size(wal);

  plan->Fail(FaultOperation::kAppend, "000001.wal");
  EXPECT_EQ(opened.value()->Put("key", "value").code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(std::filesystem::file_size(wal), size_before);
  EXPECT_EQ(opened.value()->Get("key").status().code(), tinylsm::StatusCode::kNotFound);
}

TEST(DBErrorTest, SyncFailureLeavesWriteUnconfirmedButRecoverable) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  plan->Fail(FaultOperation::kSync, "000001.wal");
  EXPECT_EQ(opened.value()->Put("key", "value").code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Get("key").status().code(), tinylsm::StatusCode::kNotFound);
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("key").value(), "value");
}

TEST(DBErrorTest, DisabledSyncOnWriteSkipsPerWriteAndCloseSync) {
  TempDir dir;
  tinylsm::Options options;
  options.sync_on_write = false;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), options, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  plan->Fail(FaultOperation::kSync, "000001.wal");
  EXPECT_TRUE(opened.value()->Put("key", "value").ok());
  EXPECT_TRUE(opened.value()->Delete("key").ok());
  EXPECT_TRUE(opened.value()->Close().ok());
}

TEST(DBErrorTest, ExhaustedRecoveredSequenceUsesResourceStatus) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  auto wal = fs->OpenWritable(dir.path() / "000001.wal", false);
  ASSERT_TRUE(wal.ok());
  ASSERT_TRUE(wal.value()->Sync().ok());
  ASSERT_TRUE(wal.value()->Close().ok());

  tinylsm::internal::ManifestSnapshot snapshot;
  snapshot.active_wal_number = 1;
  snapshot.next_file_number = 2;
  snapshot.last_sequence = std::numeric_limits<std::uint64_t>::max();
  tinylsm::internal::ManifestState manifest(*fs, dir.path(), snapshot);
  ASSERT_TRUE(manifest.Publish(snapshot).durable());

  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_FALSE(opened.ok());
  EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kResourceExhausted);
}

TEST(DBErrorTest, RejectsActiveWalSequenceThatDoesNotFollowManifest) {
  const std::vector<std::vector<std::uint64_t>> invalid_sequences{
      {7}, {6}, {8, 8}, {9, 8}};
  for (const auto& sequences : invalid_sequences) {
    SCOPED_TRACE(sequences.front());
    TempDir dir;
    auto fs = tinylsm::internal::NewPosixFileSystem();
    ASSERT_TRUE(fs->CreateDir(dir.path()).ok());

    std::vector<tinylsm::internal::InternalEntry> entries;
    entries.reserve(sequences.size());
    for (const auto sequence : sequences) {
      entries.push_back({"key-" + std::to_string(sequence), sequence,
                         tinylsm::internal::ValueType::kValue, "value"});
    }
    ASSERT_TRUE(WriteWal(*fs, dir.path(), 1, entries).ok());

    const tinylsm::internal::ManifestSnapshot snapshot{1, 2, 7, {}};
    tinylsm::internal::ManifestState manifest(*fs, dir.path(), snapshot);
    ASSERT_TRUE(manifest.Publish(snapshot).durable());
    const auto original_size = std::filesystem::file_size(dir.path() / "000001.wal");

    auto opened = tinylsm::DB::Open(dir.path());
    EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kCorruption);
    EXPECT_EQ(std::filesystem::file_size(dir.path() / "000001.wal"), original_size);
  }
}

TEST(DBTest, AcceptsActiveWalSequenceGapsAndContinuesAfterTheMaximum) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  ASSERT_TRUE(WriteWal(*fs, dir.path(), 1,
                       {{"a", 9, tinylsm::internal::ValueType::kValue, "nine"},
                        {"b", 12, tinylsm::internal::ValueType::kValue, "twelve"}})
                  .ok());

  const tinylsm::internal::ManifestSnapshot snapshot{1, 2, 7, {}};
  tinylsm::internal::ManifestState manifest(*fs, dir.path(), snapshot);
  ASSERT_TRUE(manifest.Publish(snapshot).durable());

  tinylsm::Options options;
  options.memtable_bytes = 1;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  EXPECT_EQ(opened.value()->Get("a").value(), "nine");
  EXPECT_EQ(opened.value()->Get("b").value(), "twelve");
  ASSERT_TRUE(opened.value()->Put("c", "thirteen").ok());

  auto published = tinylsm::internal::ManifestState::Load(*fs, dir.path());
  ASSERT_TRUE(published.ok()) << published.status().ToString();
  ASSERT_EQ(published.value().live_tables.size(), 1U);
  EXPECT_EQ(published.value().last_sequence, 13U);
  EXPECT_EQ(published.value().live_tables.front().min_sequence, 9U);
  EXPECT_EQ(published.value().live_tables.front().max_sequence, 13U);
}

TEST(DBTest, DistinguishesMissingEmptyAndTombstone) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(opened.ok());
  auto& db = *opened.value();
  EXPECT_TRUE(db.Put("b", "two").ok());
  EXPECT_TRUE(db.Put("a", "").ok());
  ASSERT_TRUE(db.Get("a").ok());
  EXPECT_EQ(db.Get("a").value(), "");
  EXPECT_TRUE(db.Delete("a").ok());
  EXPECT_EQ(db.Get("a").status().code(), tinylsm::StatusCode::kNotFound);
  auto scan = db.Scan("", "z");
  ASSERT_TRUE(scan.ok());
  EXPECT_EQ(scan.value(), (std::vector<tinylsm::Entry>{{"b", "two"}}));
}

TEST(DBTest, DeleteOfMissingKeyIsIdempotent) {
  TempDir dir;
  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(opened.ok());
  EXPECT_TRUE(opened.value()->Delete("missing").ok());
  EXPECT_TRUE(opened.value()->Delete("missing").ok());
  EXPECT_EQ(opened.value()->Get("missing").status().code(),
            tinylsm::StatusCode::kNotFound);
}

TEST(DBTest, EnforcesKeyAndValueLimitsBeforeWalAndMemtableMutation) {
  TempDir dir;
  tinylsm::Options options;
  options.max_key_bytes = 3;
  options.max_value_bytes = 4;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  EXPECT_TRUE(opened.value()->Put("key", "data").ok());
  const auto wal = dir.path() / "000001.wal";
  const auto size_after_boundary = std::filesystem::file_size(wal);
  EXPECT_EQ(opened.value()->Put("long", "data").code(),
            tinylsm::StatusCode::kInvalidArgument);
  EXPECT_EQ(opened.value()->Put("new", "value").code(),
            tinylsm::StatusCode::kInvalidArgument);
  EXPECT_EQ(std::filesystem::file_size(wal), size_after_boundary);
  EXPECT_EQ(opened.value()->Get("long").status().code(),
            tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Get("new").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Get("key").value(), "data");
}

TEST(DBTest, WriteBatchPreservesOrderAndRecoversAsOneRecord) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();

    tinylsm::WriteBatch empty;
    EXPECT_TRUE(opened.value()->Write(empty).ok());

    tinylsm::WriteBatch batch;
    batch.Put("a", "old");
    batch.Put("b", "two");
    batch.Delete("a");
    batch.Put("a", "new");
    ASSERT_TRUE(opened.value()->Write(batch).ok());
    EXPECT_EQ(opened.value()->Get("a").value(), "new");
    EXPECT_EQ(opened.value()->Get("b").value(), "two");
  }

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").value(), "new");
  EXPECT_EQ(reopened.value()->Get("b").value(), "two");
}

TEST(DBTest, WriteBatchFlushesOnlyAfterTheWholeBatchIsApplied) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 1;
  {
    auto opened = tinylsm::DB::Open(dir.path(), options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();

    tinylsm::WriteBatch batch;
    batch.Put("a", "one");
    batch.Put("b", "two");
    batch.Delete("a");
    ASSERT_TRUE(opened.value()->Write(batch).ok());
    EXPECT_EQ(opened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
    EXPECT_EQ(opened.value()->Get("b").value(), "two");
  }

  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(reopened.value()->Get("b").value(), "two");
}

TEST(DBTest, ConcurrentCallersAreSerializedWithoutLosingWrites) {
  TempDir dir;
  tinylsm::Options options;
  options.sync_on_write = false;
  options.memtable_bytes = 32U * 1024U;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kWritesPerThread = 200;
  std::barrier start(kThreads);
  std::vector<std::thread> threads;
  std::vector<tinylsm::Status> statuses(kThreads);
  threads.reserve(kThreads);
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      start.arrive_and_wait();
      for (std::size_t index = 0; index < kWritesPerThread; ++index) {
        const auto key =
            "thread-" + std::to_string(thread) + "-key-" + std::to_string(index);
        statuses[thread] = opened.value()->Put(key, "value-" + std::to_string(index));
        if (!statuses[thread].ok())
          return;
        auto read = opened.value()->Get(key);
        if (!read.ok()) {
          statuses[thread] = read.status();
          return;
        }
      }
    });
  }
  for (auto& thread : threads)
    thread.join();
  for (const auto& status : statuses)
    ASSERT_TRUE(status.ok()) << status.ToString();

  auto scan = opened.value()->Scan({}, {});
  ASSERT_TRUE(scan.ok()) << scan.status().ToString();
  EXPECT_EQ(scan.value().size(), kThreads * kWritesPerThread);
}

TEST(DBTest, IncompleteWalBatchIsDiscardedInFull) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    tinylsm::WriteBatch batch;
    batch.Put("a", "one");
    batch.Put("b", "two");
    ASSERT_TRUE(opened.value()->Write(batch).ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }

  const auto wal = dir.path() / "000001.wal";
  const auto complete_size = std::filesystem::file_size(wal);
  ASSERT_GT(complete_size, 1U);
  std::filesystem::resize_file(wal, complete_size - 1);

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(reopened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(std::filesystem::file_size(wal), 0U);
}

TEST(DBTest, InvalidWriteBatchChangesNeitherWalNorMemtable) {
  TempDir dir;
  tinylsm::Options options;
  options.max_key_bytes = 3;
  options.max_value_bytes = 4;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  tinylsm::WriteBatch batch;
  batch.Put("ok", "data");
  batch.Put("long", "data");
  const auto wal = dir.path() / "000001.wal";
  const auto original_size = std::filesystem::file_size(wal);
  EXPECT_EQ(opened.value()->Write(batch).code(), tinylsm::StatusCode::kInvalidArgument);
  EXPECT_EQ(std::filesystem::file_size(wal), original_size);
  EXPECT_EQ(opened.value()->Get("ok").status().code(), tinylsm::StatusCode::kNotFound);
}

TEST(DBErrorTest, WriteBatchSyncFailureIsUnconfirmedButFullyRecoverable) {
  TempDir dir;
  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();

  tinylsm::WriteBatch batch;
  batch.Put("a", "one");
  batch.Put("b", "two");
  plan->Fail(FaultOperation::kSync, "000001.wal");
  EXPECT_EQ(opened.value()->Write(batch).code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(opened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").value(), "one");
  EXPECT_EQ(reopened.value()->Get("b").value(), "two");
}

TEST(DBErrorTest, WriteBatchAppendFailureNeverPartiallyApplies) {
  for (const auto timing : {FaultTiming::kBefore, FaultTiming::kAfter}) {
    SCOPED_TRACE(timing == FaultTiming::kBefore ? "before" : "after");
    TempDir dir;
    auto plan = std::make_shared<tinylsm::test::FaultPlan>();
    auto opened = tinylsm::internal::DBTestPeer::Open(
        dir.path(), {}, tinylsm::test::NewFaultInjectionFileSystem(plan));
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();

    tinylsm::WriteBatch batch;
    batch.Put("a", "one");
    batch.Put("b", "two");
    plan->Fail(FaultOperation::kAppend, "000001.wal", 1, timing);
    EXPECT_EQ(opened.value()->Write(batch).code(), tinylsm::StatusCode::kIOError);
    EXPECT_EQ(opened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
    EXPECT_EQ(opened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
    opened.value().reset();

    auto reopened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
    if (timing == FaultTiming::kBefore) {
      EXPECT_EQ(reopened.value()->Get("a").status().code(),
                tinylsm::StatusCode::kNotFound);
      EXPECT_EQ(reopened.value()->Get("b").status().code(),
                tinylsm::StatusCode::kNotFound);
    } else {
      EXPECT_EQ(reopened.value()->Get("a").value(), "one");
      EXPECT_EQ(reopened.value()->Get("b").value(), "two");
    }
  }
}

TEST(DBTest, PreservesBinaryKeysValuesAndBytewiseScanOrderAcrossReopen) {
  TempDir dir;
  const std::string zero("\0", 1);
  const std::string zero_high("\0\x80", 2);
  const std::string high("\x80", 1);
  const std::string binary_value("v\0\xff", 3);
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    EXPECT_TRUE(opened.value()->Put(high, "high").ok());
    EXPECT_TRUE(opened.value()->Put(zero_high, binary_value).ok());
    EXPECT_TRUE(opened.value()->Put(zero, "zero").ok());
    EXPECT_TRUE(opened.value()->Close().ok());
  }

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get(zero_high).value(), binary_value);
  auto scan = reopened.value()->Scan("", "");
  ASSERT_TRUE(scan.ok()) << scan.status().ToString();
  EXPECT_EQ(scan.value(),
            (std::vector<tinylsm::Entry>{
                {zero, "zero"}, {zero_high, binary_value}, {high, "high"}}));
}

TEST(DBTest, WalRecoveryReopensLatestValues) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    EXPECT_TRUE(opened.value()->Put("a", "one").ok());
    EXPECT_TRUE(opened.value()->Put("b", "two").ok());
    EXPECT_TRUE(opened.value()->Delete("a").ok());
    EXPECT_TRUE(opened.value()->Close().ok());
  }
  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_EQ(reopened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);
  ASSERT_TRUE(reopened.value()->Get("b").ok());
  EXPECT_EQ(reopened.value()->Get("b").value(), "two");
}

TEST(DBTest, ReopenTruncatesIncompleteWalTail) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Put("safe", "value").ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  const auto wal = dir.path() / "000001.wal";
  const auto valid_size = std::filesystem::file_size(wal);
  {
    std::ofstream output(wal, std::ios::binary | std::ios::app);
    output.write("WAL", 3);
  }
  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_EQ(reopened.value()->Get("safe").value(), "value");
  EXPECT_EQ(std::filesystem::file_size(wal), valid_size);
}

TEST(DBTest, RepeatedFlushPreservesNewestValuesAcrossReopen) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 1;
  options.sstable_block_bytes = 40;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().message();

  auto expect_manifest = [&](std::size_t table_count, std::uint64_t last_sequence) {
    auto fs = tinylsm::internal::NewPosixFileSystem();
    auto manifest = tinylsm::internal::ManifestState::Load(*fs, dir.path());
    ASSERT_TRUE(manifest.ok()) << manifest.status().ToString();
    EXPECT_EQ(manifest.value().live_tables.size(), table_count);
    EXPECT_EQ(manifest.value().active_wal_number, table_count * 2 + 1);
    EXPECT_EQ(manifest.value().next_file_number, table_count * 2 + 2);
    EXPECT_EQ(manifest.value().last_sequence, last_sequence);
    for (std::size_t i = 0; i < manifest.value().live_tables.size(); ++i) {
      EXPECT_EQ(manifest.value().live_tables[i].min_sequence, i + 1);
      EXPECT_EQ(manifest.value().live_tables[i].max_sequence, i + 1);
      if (i != 0) {
        EXPECT_LT(manifest.value().live_tables[i - 1].max_sequence,
                  manifest.value().live_tables[i].min_sequence);
      }
    }
  };

  ASSERT_TRUE(opened.value()->Put("a", "old-a").ok());
  expect_manifest(1, 1);
  EXPECT_EQ(opened.value()->Get("a").value(), "old-a");

  ASSERT_TRUE(opened.value()->Put("b", "live-b").ok());
  expect_manifest(2, 2);
  auto first_scan = opened.value()->Scan("", "");
  ASSERT_TRUE(first_scan.ok());
  EXPECT_EQ(first_scan.value(),
            (std::vector<tinylsm::Entry>{{"a", "old-a"}, {"b", "live-b"}}));
  EXPECT_TRUE(opened.value()->Close().ok());

  opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  ASSERT_TRUE(opened.value()->Delete("a").ok());
  expect_manifest(3, 3);
  EXPECT_EQ(opened.value()->Get("a").status().code(), tinylsm::StatusCode::kNotFound);

  ASSERT_TRUE(opened.value()->Put("a", "new-a").ok());
  expect_manifest(4, 4);
  EXPECT_EQ(opened.value()->Get("a").value(), "new-a");

  ASSERT_TRUE(opened.value()->Delete("b").ok());
  expect_manifest(5, 5);
  auto final_scan = opened.value()->Scan("", "");
  ASSERT_TRUE(final_scan.ok());
  EXPECT_EQ(final_scan.value(), (std::vector<tinylsm::Entry>{{"a", "new-a"}}));
  EXPECT_TRUE(opened.value()->Close().ok());

  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_EQ(reopened.value()->Get("a").value(), "new-a");
  EXPECT_EQ(reopened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
}

TEST(DBTest, OpensAndReadsMultipleManifestTablesInOldestToNewestOrder) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  auto oldest = BuildTable(*fs, dir.path(), 2,
                           {{"a", 1, tinylsm::internal::ValueType::kValue, "old-a"},
                            {"b", 2, tinylsm::internal::ValueType::kValue, "old-b"}});
  auto middle = BuildTable(*fs, dir.path(), 4,
                           {{"a", 3, tinylsm::internal::ValueType::kValue, "new-a"},
                            {"c", 4, tinylsm::internal::ValueType::kTombstone, ""}});
  auto newest = BuildTable(*fs, dir.path(), 6,
                           {{"b", 5, tinylsm::internal::ValueType::kTombstone, ""},
                            {"d", 6, tinylsm::internal::ValueType::kValue, "live-d"}});
  ASSERT_TRUE(oldest.ok());
  ASSERT_TRUE(middle.ok());
  ASSERT_TRUE(newest.ok());
  ASSERT_TRUE(CreateEmptyWal(*fs, dir.path(), 7).ok());

  tinylsm::internal::ManifestSnapshot snapshot{
      7, 8, 6, {oldest.value(), middle.value(), newest.value()}};
  tinylsm::internal::ManifestState manifest(*fs, dir.path(), snapshot);
  ASSERT_TRUE(manifest.Publish(snapshot).durable());

  auto opened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  EXPECT_EQ(opened.value()->Get("a").value(), "new-a");
  EXPECT_EQ(opened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Get("c").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(opened.value()->Get("d").value(), "live-d");
  auto scan = opened.value()->Scan("", "");
  ASSERT_TRUE(scan.ok()) << scan.status().ToString();
  EXPECT_EQ(scan.value(),
            (std::vector<tinylsm::Entry>{{"a", "new-a"}, {"d", "live-d"}}));

  ASSERT_TRUE(opened.value()->Put("e", "from-wal").ok());
  EXPECT_EQ(opened.value()->Get("e").value(), "from-wal");
  ASSERT_TRUE(opened.value()->Close().ok());
  opened.value().reset();

  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().ToString();
  EXPECT_EQ(reopened.value()->Get("a").value(), "new-a");
  EXPECT_EQ(reopened.value()->Get("b").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_EQ(reopened.value()->Get("e").value(), "from-wal");
}

TEST(DBTest, RejectsManifestMetadataThatDoesNotMatchTheSstable) {
  for (int mismatch = 0; mismatch < 5; ++mismatch) {
    SCOPED_TRACE(mismatch);
    TempDir dir;
    auto fs = tinylsm::internal::NewPosixFileSystem();
    ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
    auto table = BuildTable(*fs, dir.path(), 2,
                            {{"a", 1, tinylsm::internal::ValueType::kValue, "one"},
                             {"z", 2, tinylsm::internal::ValueType::kValue, "two"}});
    ASSERT_TRUE(table.ok());
    ASSERT_TRUE(CreateEmptyWal(*fs, dir.path(), 3).ok());

    auto wrong = table.value();
    switch (mismatch) {
    case 0:
      ++wrong.file_size;
      break;
    case 1:
      wrong.smallest_key = "b";
      break;
    case 2:
      wrong.largest_key = "y";
      break;
    case 3:
      wrong.min_sequence = 2;
      break;
    case 4:
      wrong.max_sequence = 1;
      break;
    default:
      FAIL() << "unexpected metadata mismatch case";
    }

    tinylsm::internal::ManifestSnapshot snapshot{3, 4, 2, {std::move(wrong)}};
    tinylsm::internal::ManifestState manifest(*fs, dir.path(), snapshot);
    ASSERT_TRUE(manifest.Publish(snapshot).durable());

    auto opened = tinylsm::DB::Open(dir.path());
    EXPECT_EQ(opened.status().code(), tinylsm::StatusCode::kCorruption);
  }
}

TEST(DBTest, MissingManifestReferencedTableIsCorruption) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 128;
  {
    auto opened = tinylsm::DB::Open(dir.path(), options);
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  std::filesystem::remove(dir.path() / "000002.sst");
  auto reopened = tinylsm::DB::Open(dir.path(), options);
  EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(DBTest, MissingManifestReferencedWalIsCorruption) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  std::filesystem::remove(dir.path() / "000001.wal");
  auto reopened = tinylsm::DB::Open(dir.path());
  EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(DBTest, CorruptManifestFramingIsReportedThroughOpen) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  CorruptByte(dir.path() / "MANIFEST", 0);
  auto reopened = tinylsm::DB::Open(dir.path());
  EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(DBTest, CorruptReferencedIndexAndFooterAreReportedThroughOpen) {
  for (const bool corrupt_footer : {false, true}) {
    SCOPED_TRACE(corrupt_footer ? "footer" : "index");
    TempDir dir;
    tinylsm::Options options;
    options.memtable_bytes = 128;
    {
      auto opened = tinylsm::DB::Open(dir.path(), options);
      ASSERT_TRUE(opened.ok());
      ASSERT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
      ASSERT_TRUE(opened.value()->Close().ok());
    }

    const auto table = dir.path() / "000002.sst";
    const auto size = std::filesystem::file_size(table);
    const auto footer_start = size - tinylsm::internal::kSstableFooterSize;
    CorruptByte(table, corrupt_footer ? footer_start : footer_start - 1);
    auto reopened = tinylsm::DB::Open(dir.path(), options);
    EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kCorruption);
  }
}

TEST(DBTest, ManifestIsAuthoritativeAndIgnoresOrphans) {
  TempDir dir;
  {
    auto opened = tinylsm::DB::Open(dir.path());
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Put("safe", "value").ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  std::ofstream(dir.path() / "999999.sst", std::ios::binary).write("junk", 4);
  std::ofstream(dir.path() / "000002.wal", std::ios::binary).write("junk", 4);
  auto reopened = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_EQ(reopened.value()->Get("safe").value(), "value");
}

TEST(DBTest, CorruptReferencedDataBlockIsReportedOnOpen) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 128;
  {
    auto opened = tinylsm::DB::Open(dir.path(), options);
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }
  const auto table = dir.path() / "000002.sst";
  {
    std::fstream file(table, std::ios::binary | std::ios::in | std::ios::out);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 1;
    file.seekp(0);
    file.write(&byte, 1);
  }
  auto reopened = tinylsm::DB::Open(dir.path(), options);
  EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(DBTest, SstableReadFailureDuringOpenIsPropagated) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 128;
  {
    auto opened = tinylsm::DB::Open(dir.path(), options);
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->Put("key", std::string(64, 'v')).ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }

  auto plan = std::make_shared<tinylsm::test::FaultPlan>();
  plan->Fail(FaultOperation::kReadAt, ".sst", 3);
  auto reopened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), options, tinylsm::test::NewFaultInjectionFileSystem(plan));
  EXPECT_EQ(reopened.status().code(), tinylsm::StatusCode::kIOError);
}
