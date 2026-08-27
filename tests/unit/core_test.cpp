#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "manifest/manifest_codec.h"
#include "memtable/memtable.h"
#include "sstable/sstable_format.h"
#include "util/coding.h"
#include "wal/wal_reader.h"
#include "wal/wal_record_codec.h"

namespace ti = tinylsm::internal;

namespace {
class StringSequentialFile final : public ti::SequentialFile {
public:
  StringSequentialFile(std::string data, std::size_t chunk)
      : data_(std::move(data)), chunk_(chunk) {}
  tinylsm::Result<std::size_t> Read(std::span<std::byte> buffer) override {
    const std::size_t count = std::min({buffer.size(), chunk_, data_.size() - offset_});
    if (count != 0)
      std::memcpy(buffer.data(), data_.data() + offset_, count);
    offset_ += count;
    return count;
  }

private:
  std::string data_;
  std::size_t chunk_;
  std::size_t offset_ = 0;
};
} // namespace

TEST(MemTableTest, PreservesEmptyValueTombstoneAndSequenceRules) {
  ti::MemTable table;
  EXPECT_TRUE(table.Apply({"key", 1, ti::ValueType::kValue, ""}).ok());
  auto empty = table.Get("key");
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(empty.value().value, "");
  EXPECT_FALSE(table.Apply({"key", 1, ti::ValueType::kValue, "later"}).ok());
  EXPECT_TRUE(table.Apply({"key", 2, ti::ValueType::kTombstone, ""}).ok());
  ASSERT_TRUE(table.Get("key").ok());
  EXPECT_EQ(table.Get("key").value().type, ti::ValueType::kTombstone);
  EXPECT_FALSE(table.Get("missing").ok());
}

TEST(WalCodecTest, HasStableHeaderAndRejectsCorruptionAndLimits) {
  ti::InternalEntry entry{"k", 1, ti::ValueType::kValue, "v"};
  auto encoded = ti::EncodeWalRecord(entry, {});
  ASSERT_TRUE(encoded.ok());
  const auto& bytes = encoded.value();
  ASSERT_EQ(bytes.size(), 34U);
  const std::string golden("\x57\x41\x4c\x31\x01\x00\x01\x00\x12\x00\x00\x00\xef\xcc"
                           "\xf1\x59\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00"
                           "\x01\x00\x00\x00\x6b\x76",
                           34);
  EXPECT_EQ(bytes, golden);
  auto decoded = ti::DecodeWalRecord(ti::AsBytes(bytes), {});
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), entry);
  std::string corrupt = bytes;
  corrupt.back() ^= 1;
  EXPECT_EQ(ti::DecodeWalRecord(ti::AsBytes(corrupt), {}).status().code(),
            tinylsm::StatusCode::kCorruption);
  ti::DecodeLimits tiny{0, 0};
  EXPECT_EQ(ti::DecodeWalRecord(ti::AsBytes(bytes), tiny).status().code(),
            tinylsm::StatusCode::kCorruption);
}

TEST(WalReaderTest, ReplaysValidPrefixAndClassifiesTruncatedTail) {
  auto first = ti::EncodeWalRecord({"a", 7, ti::ValueType::kValue, "one"}, {});
  auto second = ti::EncodeWalRecord({"b", 9, ti::ValueType::kValue, "two"}, {});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  std::string log = first.value() + second.value().substr(0, 5);
  ti::WalReader reader(std::make_unique<StringSequentialFile>(log, 3), {});
  std::vector<ti::InternalEntry> applied;
  auto replay = reader.Replay([&](const ti::InternalEntry& entry) {
    applied.push_back(entry);
    return tinylsm::Status::Ok();
  });
  ASSERT_TRUE(replay.ok());
  EXPECT_TRUE(replay.value().truncated_tail);
  EXPECT_EQ(replay.value().valid_bytes, first.value().size());
  EXPECT_EQ(replay.value().max_sequence, 7U);
  ASSERT_EQ(applied.size(), 1U);

  std::string corrupt = first.value() + second.value();
  corrupt[first.value().size() + 12] ^= 1;
  ti::WalReader corrupt_reader(
      std::make_unique<StringSequentialFile>(corrupt, corrupt.size()), {});
  auto corrupt_result = corrupt_reader.Replay(
      [](const ti::InternalEntry&) { return tinylsm::Status::Ok(); });
  EXPECT_EQ(corrupt_result.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(SstableFormatTest, ChecksOrderingAndIndependentChecksums) {
  std::vector<ti::InternalEntry> entries{{"a", 3, ti::ValueType::kValue, "one"},
                                         {"z", 4, ti::ValueType::kTombstone, ""}};
  auto encoded = ti::EncodeDataBlock(entries);
  ASSERT_TRUE(encoded.ok());
  auto decoded = ti::DecodeDataBlock(ti::AsBytes(encoded.value()));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), entries);
  encoded.value()[0] ^= 1;
  EXPECT_EQ(ti::DecodeDataBlock(ti::AsBytes(encoded.value())).status().code(),
            tinylsm::StatusCode::kCorruption);

  auto index = ti::EncodeIndex({{"a", "z", 0, 100}});
  ASSERT_TRUE(index.ok());
  index.value().back() ^= 1;
  EXPECT_EQ(ti::DecodeIndex(ti::AsBytes(index.value())).status().code(),
            tinylsm::StatusCode::kCorruption);

  auto footer = ti::EncodeFooter({10, 20});
  footer[4] ^= 1;
  EXPECT_EQ(ti::DecodeFooter(ti::AsBytes(footer)).status().code(),
            tinylsm::StatusCode::kCorruption);
}

TEST(ManifestCodecTest, RoundTripsProtobufPayloadAndChecksFraming) {
  ti::ManifestSnapshot snapshot{7, 11, 42, ti::TableMeta{9, 100, "a", "z", 1, 42}};
  auto encoded = ti::ManifestCodec::Encode(snapshot);
  ASSERT_TRUE(encoded.ok());
  auto decoded = ti::ManifestCodec::Decode(ti::AsBytes(encoded.value()));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().active_wal_number, 7U);
  ASSERT_TRUE(decoded.value().live_table.has_value());
  EXPECT_EQ(decoded.value().live_table->smallest_key, "a");
  encoded.value().back() ^= 1;
  EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(encoded.value())).status().code(),
            tinylsm::StatusCode::kCorruption);
}
