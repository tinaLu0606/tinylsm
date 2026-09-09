#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "db/db_test_peer.h"
#include "iterator/internal_iterator.h"
#include "memtable/memtable.h"
#include "sstable/sstable_builder.h"
#include "sstable/sstable_reader.h"
#include "test_support/fault_injection_fs.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"
#include "tinylsm/options.h"

namespace {

using tinylsm::internal::InternalEntry;
using tinylsm::internal::InternalIterator;
using tinylsm::internal::ValueType;
using tinylsm::test::FaultOperation;
using tinylsm::test::FaultPlan;
using tinylsm::test::TempDir;

class VectorIterator final : public InternalIterator {
public:
  explicit VectorIterator(std::vector<InternalEntry> entries,
                          std::optional<std::size_t> fail_on_next = std::nullopt)
      : entries_(std::move(entries)), fail_on_next_(fail_on_next) {}

  [[nodiscard]] bool Valid() const noexcept override {
    return status_.ok() && position_ < entries_.size();
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    EXPECT_TRUE(Valid());
    return entries_[position_];
  }

  tinylsm::Status Next() override {
    if (!status_.ok())
      return status_;
    if (!Valid())
      return tinylsm::Status::Ok();
    ++next_calls_;
    if (fail_on_next_ == next_calls_) {
      position_ = entries_.size();
      status_ = tinylsm::Status::IOError("injected iterator failure");
      return status_;
    }
    ++position_;
    return tinylsm::Status::Ok();
  }

  [[nodiscard]] const tinylsm::Status& status() const noexcept override {
    return status_;
  }

private:
  std::vector<InternalEntry> entries_;
  std::optional<std::size_t> fail_on_next_;
  std::size_t position_ = 0;
  std::size_t next_calls_ = 0;
  tinylsm::Status status_;
};

std::unique_ptr<InternalIterator>
MakeIterator(std::vector<InternalEntry> entries,
             std::optional<std::size_t> fail_on_next = std::nullopt) {
  return std::make_unique<VectorIterator>(std::move(entries), fail_on_next);
}

TEST(MemTableIteratorTest, BorrowsEntriesAndHonorsBytewiseHalfOpenRange) {
  tinylsm::internal::MemTable memtable;
  const std::string high_key(1, static_cast<char>(0x80));
  ASSERT_TRUE(memtable.Apply({"", 1, ValueType::kValue, "empty"}).ok());
  ASSERT_TRUE(
      memtable.Apply({std::string("a\0", 2), 2, ValueType::kValue, "nul"}).ok());
  ASSERT_TRUE(memtable.Apply({"b", 3, ValueType::kTombstone, ""}).ok());
  ASSERT_TRUE(memtable.Apply({high_key, 4, ValueType::kValue, "high"}).ok());

  auto iterator = memtable.NewIterator(std::string("a\0", 2), high_key);
  ASSERT_TRUE(iterator.ok()) << iterator.status().ToString();
  ASSERT_TRUE(iterator.value()->Valid());
  EXPECT_EQ(iterator.value()->entry().user_key, std::string("a\0", 2));
  ASSERT_TRUE(iterator.value()->Next().ok());
  ASSERT_TRUE(iterator.value()->Valid());
  EXPECT_EQ(iterator.value()->entry().user_key, "b");
  EXPECT_EQ(iterator.value()->entry().type, ValueType::kTombstone);
  EXPECT_TRUE(iterator.value()->Next().ok());
  EXPECT_FALSE(iterator.value()->Valid());
  EXPECT_TRUE(iterator.value()->status().ok());

  auto empty = memtable.NewIterator("b", "b");
  ASSERT_TRUE(empty.ok());
  EXPECT_FALSE(empty.value()->Valid());
  EXPECT_TRUE(empty.value()->status().ok());
}

TEST(MergingIteratorTest, OrdersEveryVersionByUserKeyThenSequenceDescending) {
  std::vector<std::unique_ptr<InternalIterator>> inputs;
  inputs.push_back(MakeIterator({{"a", 1, ValueType::kValue, "old-a"},
                                 {"b", 2, ValueType::kValue, "old-b"},
                                 {"d", 3, ValueType::kValue, "old-d"}}));
  inputs.push_back(MakeIterator({{"a", 4, ValueType::kValue, "new-a"},
                                 {"b", 5, ValueType::kTombstone, ""},
                                 {"c", 6, ValueType::kValue, "live-c"}}));

  auto merged = tinylsm::internal::NewMergingIterator(std::move(inputs));
  ASSERT_TRUE(merged.ok()) << merged.status().ToString();
  std::vector<InternalEntry> actual;
  while (merged.value()->Valid()) {
    actual.push_back(merged.value()->entry());
    ASSERT_TRUE(merged.value()->Next().ok());
  }

  EXPECT_EQ(actual, (std::vector<InternalEntry>{{"a", 4, ValueType::kValue, "new-a"},
                                                {"a", 1, ValueType::kValue, "old-a"},
                                                {"b", 5, ValueType::kTombstone, ""},
                                                {"b", 2, ValueType::kValue, "old-b"},
                                                {"c", 6, ValueType::kValue, "live-c"},
                                                {"d", 3, ValueType::kValue, "old-d"}}));
  EXPECT_TRUE(merged.value()->status().ok());
}

TEST(MergingIteratorTest, FiltersVersionsForTheRequestedSnapshotSequence) {
  std::vector<std::unique_ptr<InternalIterator>> inputs;
  inputs.push_back(MakeIterator(
      {{"a", 1, ValueType::kValue, "old-a"}, {"b", 2, ValueType::kValue, "old-b"}}));
  inputs.push_back(MakeIterator({{"a", 4, ValueType::kValue, "new-a"},
                                 {"b", 5, ValueType::kTombstone, ""},
                                 {"c", 6, ValueType::kValue, "live-c"}}));
  auto merged = tinylsm::internal::NewMergingIterator(std::move(inputs));
  ASSERT_TRUE(merged.ok()) << merged.status().ToString();
  auto visible = tinylsm::internal::NewVisibilityIterator(std::move(merged.value()), 3);
  ASSERT_TRUE(visible.ok()) << visible.status().ToString();

  std::vector<InternalEntry> actual;
  while (visible.value()->Valid()) {
    actual.push_back(visible.value()->entry());
    ASSERT_TRUE(visible.value()->Next().ok());
  }
  EXPECT_EQ(actual, (std::vector<InternalEntry>{{"a", 1, ValueType::kValue, "old-a"},
                                                {"b", 2, ValueType::kValue, "old-b"}}));
  EXPECT_TRUE(visible.value()->status().ok());
}

TEST(MergingIteratorTest, RejectsDuplicateSequenceAndMakesErrorsSticky) {
  std::vector<std::unique_ptr<InternalIterator>> duplicates;
  duplicates.push_back(MakeIterator({{"a", 7, ValueType::kValue, "one"}}));
  duplicates.push_back(MakeIterator({{"a", 7, ValueType::kValue, "two"}}));
  auto duplicate = tinylsm::internal::NewMergingIterator(std::move(duplicates));
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.status().code(), tinylsm::StatusCode::kCorruption);

  std::vector<std::unique_ptr<InternalIterator>> failing;
  failing.push_back(MakeIterator(
      {{"a", 1, ValueType::kValue, "a"}, {"c", 3, ValueType::kValue, "c"}}, 2));
  failing.push_back(MakeIterator(
      {{"b", 2, ValueType::kValue, "b"}, {"d", 4, ValueType::kValue, "d"}}));
  auto merged = tinylsm::internal::NewMergingIterator(std::move(failing));
  ASSERT_TRUE(merged.ok());
  EXPECT_EQ(merged.value()->entry().user_key, "a");
  ASSERT_TRUE(merged.value()->Next().ok());
  EXPECT_EQ(merged.value()->entry().user_key, "b");
  ASSERT_TRUE(merged.value()->Next().ok());
  EXPECT_EQ(merged.value()->entry().user_key, "c");
  const auto failed = merged.value()->Next();
  EXPECT_EQ(failed.code(), tinylsm::StatusCode::kIOError);
  EXPECT_FALSE(merged.value()->Valid());
  EXPECT_EQ(merged.value()->status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(merged.value()->Next().code(), tinylsm::StatusCode::kIOError);
}

TEST(SstableIteratorTest, LoadsBlocksLazilyAndMakesReadFailureSticky) {
  TempDir dir;
  auto fs = tinylsm::internal::NewPosixFileSystem();
  ASSERT_TRUE(fs->CreateDir(dir.path()).ok());
  auto writable = fs->OpenWritable(dir.path() / "table.sst", false);
  ASSERT_TRUE(writable.ok());
  tinylsm::internal::SSTableBuilder builder(std::move(writable.value()), 40);
  ASSERT_TRUE(builder.Add({"a", 1, ValueType::kValue, std::string(20, 'a')}).ok());
  ASSERT_TRUE(builder.Add({"m", 2, ValueType::kValue, std::string(20, 'm')}).ok());
  ASSERT_TRUE(builder.Add({"z", 3, ValueType::kValue, std::string(20, 'z')}).ok());
  ASSERT_TRUE(builder.Finish().ok());

  auto plan = std::make_shared<FaultPlan>();
  // v2 open probes v1 footer bytes, then reads the v2 footer, index, and
  // properties. The first block is read by Seek; fail the second lazy block.
  plan->Fail(FaultOperation::kReadAt, "table.sst", 6);
  auto fault_fs = tinylsm::test::NewFaultInjectionFileSystem(plan);
  auto random = fault_fs->OpenRandomAccess(dir.path() / "table.sst");
  ASSERT_TRUE(random.ok());
  auto reader = tinylsm::internal::SSTableReader::Open(std::move(random.value()));
  ASSERT_TRUE(reader.ok());
  auto iterator = reader.value()->NewIterator("", "");
  ASSERT_TRUE(iterator.ok()) << iterator.status().ToString();
  ASSERT_TRUE(iterator.value()->Valid());
  EXPECT_EQ(iterator.value()->entry().user_key, "a");

  const auto failed = iterator.value()->Next();
  EXPECT_EQ(failed.code(), tinylsm::StatusCode::kIOError);
  EXPECT_FALSE(iterator.value()->Valid());
  EXPECT_EQ(iterator.value()->status().code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(iterator.value()->Next().code(), tinylsm::StatusCode::kIOError);
}

TEST(DBIteratorTest, LateSstableReadFailureReturnsNoPartialScanResult) {
  TempDir dir;
  tinylsm::Options options;
  options.memtable_bytes = 250;
  options.sstable_block_bytes = 40;
  {
    auto opened = tinylsm::DB::Open(dir.path(), options);
    ASSERT_TRUE(opened.ok()) << opened.status().ToString();
    ASSERT_TRUE(opened.value()->Put("a", std::string(20, 'a')).ok());
    ASSERT_TRUE(opened.value()->Put("m", std::string(20, 'm')).ok());
    ASSERT_TRUE(opened.value()->Put("z", std::string(20, 'z')).ok());
    ASSERT_TRUE(opened.value()->Close().ok());
  }

  auto plan = std::make_shared<FaultPlan>();
  // The v2 footer/properties add two open-time reads before validation and
  // the later lazy Scan read.
  plan->Fail(FaultOperation::kReadAt, ".sst", 9);
  auto opened = tinylsm::internal::DBTestPeer::Open(
      dir.path(), options, tinylsm::test::NewFaultInjectionFileSystem(plan));
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  auto scan = opened.value()->Scan("", "");
  ASSERT_FALSE(scan.ok());
  EXPECT_EQ(scan.status().code(), tinylsm::StatusCode::kIOError);
}

} // namespace
