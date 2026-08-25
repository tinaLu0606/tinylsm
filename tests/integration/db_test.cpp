#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "io/file.h"
#include "sstable/sstable_builder.h"
#include "sstable/sstable_reader.h"
#include "tinylsm/db.h"
#include "util/coding.h"

namespace {
class TempDir {
public:
  TempDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("tinylsm-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};
} // namespace

TEST(SstableTest, BuildsMultipleBlocksAndReadsBoundaryKeys) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  auto writable = fs->OpenWritable(dir.path() / "table.sst", false);
  ASSERT_TRUE(writable.ok());
  tinylsm::internal::SSTableBuilder builder(std::move(writable.value()), 40);
  ASSERT_TRUE(
      builder.Add({"a", 1, tinylsm::internal::ValueType::kValue, std::string(20, 'a')}).ok());
  ASSERT_TRUE(
      builder.Add({"m", 2, tinylsm::internal::ValueType::kValue, std::string(20, 'm')}).ok());
  ASSERT_TRUE(
      builder.Add({"z", 3, tinylsm::internal::ValueType::kValue, std::string(20, 'z')}).ok());
  ASSERT_TRUE(builder.Finish().ok());
  auto random = fs->OpenRandomAccess(dir.path() / "table.sst");
  ASSERT_TRUE(random.ok());
  auto reader = tinylsm::internal::SSTableReader::Open(std::move(random.value()));
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ(reader.value()->Get("a").value().sequence, 1U);
  EXPECT_EQ(reader.value()->Get("m").value().sequence, 2U);
  EXPECT_EQ(reader.value()->Get("z").value().sequence, 3U);
  auto scan = reader.value()->Scan("m", "zz");
  ASSERT_TRUE(scan.ok());
  ASSERT_EQ(scan.value().size(), 2U);
  EXPECT_EQ(scan.value().front().user_key, "m");
  EXPECT_EQ(scan.value().back().user_key, "z");
}

TEST(FileSystemTest, WritableFileRetriesShortWritesAndPropagatesPathErrors) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystemForTesting(
      [](int fd, const void* data, std::size_t size) {
        return static_cast<std::ptrdiff_t>(::write(fd, data, std::min<std::size_t>(size, 1)));
      });
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  const auto path = dir.path() / "short-write";
  auto writable = fs->OpenWritable(path, false);
  ASSERT_TRUE(writable.ok());
  const std::string expected = "complete despite short writes";
  ASSERT_TRUE(writable.value()->Append(tinylsm::internal::AsBytes(expected)).ok());
  ASSERT_TRUE(writable.value()->Sync().ok());
  ASSERT_TRUE(writable.value()->Close().ok());
  std::ifstream input(path, std::ios::binary);
  const std::string actual{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(fs->Rename(dir.path() / "missing", dir.path() / "target").code(),
            tinylsm::StatusCode::kIOError);
  EXPECT_EQ(fs->SyncDir(dir.path() / "missing").code(), tinylsm::StatusCode::kIOError);
}

TEST(DBTest, InMemoryDistinguishesMissingEmptyAndTombstone) {
  auto opened = tinylsm::DB::OpenInMemory();
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

TEST(DBTest, FlushMergesSstableAndMemtableAndRejectsSecondFlushEarly) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 128;
  options.sstable_block_bytes = 40;
  auto opened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  ASSERT_TRUE(opened.value()->Put("a", std::string(64, 'a')).ok());
  ASSERT_TRUE(std::filesystem::exists(dir.path() / "000002.sst"));
  ASSERT_TRUE(opened.value()->Put("z", "new").ok());
  ASSERT_TRUE(opened.value()->Put("z", std::string(40, 'n')).ok());
  auto scan = opened.value()->Scan("", "");
  ASSERT_TRUE(scan.ok());
  EXPECT_EQ(scan.value().size(), 2U);
  const auto wal_size = std::filesystem::file_size(dir.path() / "000003.wal");
  const auto rejected = opened.value()->Put("x", std::string(128, 'x'));
  EXPECT_EQ(rejected.code(), tinylsm::StatusCode::kNotSupported);
  EXPECT_EQ(std::filesystem::file_size(dir.path() / "000003.wal"), wal_size);
  EXPECT_EQ(opened.value()->Get("x").status().code(), tinylsm::StatusCode::kNotFound);
  EXPECT_TRUE(opened.value()->Close().ok());

  auto reopened = tinylsm::DB::Open(dir.path(), options);
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_TRUE(reopened.value()->Get("a").ok());
  EXPECT_EQ(reopened.value()->Get("z").value(), std::string(40, 'n'));
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

TEST(DBTest, CorruptReferencedDataBlockIsReportedOnRead) {
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
  ASSERT_TRUE(reopened.ok()) << reopened.status().message();
  EXPECT_EQ(reopened.value()->Get("key").status().code(), tinylsm::StatusCode::kCorruption);
}
