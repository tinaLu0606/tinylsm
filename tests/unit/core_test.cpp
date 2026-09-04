#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "manifest/manifest_codec.h"
#include "memtable/memtable.h"
#include "sstable/sstable_format.h"
#include "tinylsm/result.h"
#include "util/coding.h"
#include "util/crc32c.h"
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

std::string ManifestFrame(std::uint16_t version, std::string_view payload) {
  std::string frame;
  ti::PutFixed32(frame, 0x31464e4dU);
  ti::PutFixed16(frame, version);
  ti::PutFixed16(frame, 0);
  ti::PutFixed32(frame, static_cast<std::uint32_t>(payload.size()));
  const auto checksum = version == 1
                            ? ti::Crc32c(ti::AsBytes(payload))
                            : ti::Crc32c(ti::AsBytes(frame), ti::AsBytes(payload));
  ti::PutFixed32(frame, checksum);
  frame += payload;
  return frame;
}

std::string ManifestPayload(const std::string& frame) {
  return frame.substr(ti::kManifestHeaderBytes);
}

bool LegacyV1FrameGateAccepts(const std::string& frame) {
  if (frame.size() < ti::kManifestHeaderBytes)
    return false;
  std::uint32_t magic = 0, payload_size = 0, checksum = 0;
  std::uint16_t version = 0, flags = 0;
  const auto bytes = ti::AsBytes(frame);
  ti::GetFixed32(bytes, 0, magic);
  ti::GetFixed16(bytes, 4, version);
  ti::GetFixed16(bytes, 6, flags);
  ti::GetFixed32(bytes, 8, payload_size);
  ti::GetFixed32(bytes, 12, checksum);
  return magic == 0x31464e4dU && version == 1 && flags == 0 &&
         payload_size == frame.size() - ti::kManifestHeaderBytes &&
         ti::Crc32c(bytes.subspan(ti::kManifestHeaderBytes)) == checksum;
}
} // namespace

TEST(StatusTest, PreservesCodeWhileAddingDiagnosticContext) {
  const auto status = tinylsm::Status::IOError("read file").WithContext("replay WAL");
  EXPECT_EQ(status.code(), tinylsm::StatusCode::kIOError);
  EXPECT_EQ(status.message(), "replay WAL: read file");
  EXPECT_EQ(status.ToString(), "IOError: replay WAL: read file");
  EXPECT_EQ(tinylsm::Status::Ok().WithContext("unused").ToString(), "OK");
  EXPECT_EQ(tinylsm::Status::ResourceExhausted("sequence space").code(),
            tinylsm::StatusCode::kResourceExhausted);
}

TEST(ResultTest, EnforcesValueOrErrorInvariantInAllBuildModes) {
  tinylsm::Result<std::string> value("ready");
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(*value, "ready");
  EXPECT_EQ(value->size(), 5U);

  tinylsm::Result<std::unique_ptr<int>> move_only(std::make_unique<int>(42));
  ASSERT_TRUE(move_only.ok());
  EXPECT_EQ(**move_only, 42);

  tinylsm::Result<int> error(tinylsm::Status::NotFound("missing"));
  ASSERT_FALSE(error.ok());
  try {
    static_cast<void>(error.value());
    FAIL() << "value() should reject an error Result";
  } catch (const tinylsm::BadResultAccess& access) {
    EXPECT_EQ(access.status().code(), tinylsm::StatusCode::kNotFound);
    EXPECT_NE(std::string(access.what()).find("missing"), std::string::npos);
  }

  EXPECT_THROW(static_cast<void>(tinylsm::Result<int>(tinylsm::Status::Ok())),
               std::invalid_argument);
  EXPECT_EQ(tinylsm::Result<int>(7).value(), 7);
}

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
  for (const std::size_t offset : {0U, 4U, 8U, 12U}) {
    SCOPED_TRACE(offset);
    std::string corrupt = bytes;
    corrupt[offset] ^= 1;
    EXPECT_EQ(ti::DecodeWalRecord(ti::AsBytes(corrupt), {}).status().code(),
              tinylsm::StatusCode::kCorruption);
  }

  EXPECT_EQ(ti::DecodeWalRecord(ti::AsBytes(bytes), {0, 1}).status().code(),
            tinylsm::StatusCode::kCorruption);
  EXPECT_EQ(ti::DecodeWalRecord(ti::AsBytes(bytes), {1, 0}).status().code(),
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
  auto replay = reader.Replay(0, [&](const ti::InternalEntry& entry) {
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
      0, [](const ti::InternalEntry&) { return tinylsm::Status::Ok(); });
  EXPECT_EQ(corrupt_result.status().code(), tinylsm::StatusCode::kCorruption);
}

TEST(WalReaderTest, EnforcesPublishedFloorAndStrictlyIncreasingSequence) {
  const auto make_log = [](std::initializer_list<std::uint64_t> sequences) {
    std::string log;
    for (const auto sequence : sequences) {
      auto encoded = ti::EncodeWalRecord(
          {"key-" + std::to_string(sequence), sequence, ti::ValueType::kValue, "value"},
          {});
      if (encoded.ok())
        log += encoded.value();
    }
    return log;
  };

  std::vector<ti::InternalEntry> applied;
  auto valid_log = make_log({8, 10, 13});
  ti::WalReader valid_reader(
      std::make_unique<StringSequentialFile>(valid_log, valid_log.size()), {});
  auto valid = valid_reader.Replay(7, [&](const ti::InternalEntry& entry) {
    applied.push_back(entry);
    return tinylsm::Status::Ok();
  });
  ASSERT_TRUE(valid.ok()) << valid.status().ToString();
  EXPECT_EQ(valid.value().max_sequence, 13U);
  EXPECT_EQ(applied.size(), 3U);

  for (const auto& [floor, sequences] :
       std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>>{
           {7, {7}}, {7, {6}}, {0, {0}}, {7, {8, 8}}, {7, {9, 8}}}) {
    SCOPED_TRACE(floor);
    std::string log;
    for (const auto sequence : sequences) {
      auto encoded = ti::EncodeWalRecord(
          {"key-" + std::to_string(sequence), sequence, ti::ValueType::kValue, "value"},
          {});
      ASSERT_TRUE(encoded.ok());
      log += encoded.value();
    }
    std::size_t callback_count = 0;
    ti::WalReader reader(std::make_unique<StringSequentialFile>(log, log.size()), {});
    auto replay = reader.Replay(floor, [&](const ti::InternalEntry&) {
      ++callback_count;
      return tinylsm::Status::Ok();
    });
    EXPECT_EQ(replay.status().code(), tinylsm::StatusCode::kCorruption);
    EXPECT_LT(callback_count, sequences.size());
  }
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

TEST(ManifestCodecTest, ReadsFixedVersionOneGoldenFiles) {
  const std::string empty(
      "\x4d\x4e\x46\x31\x01\x00\x00\x00\x04\x00\x00\x00\x29\x3b\x9c\xc5"
      "\x08\x01\x10\x02",
      20);
  const std::string one_table(
      "\x4d\x4e\x46\x31\x01\x00\x00\x00\x16\x00\x00\x00\x8a\xe8\x14\xd3"
      "\x08\x07\x10\x0b\x18\x2a\x22\x0e\x08\x09\x10\x64\x1a\x01\x61\x22"
      "\x01\x7a\x28\x01\x30\x2a",
      38);

  auto decoded_empty = ti::ManifestCodec::Decode(ti::AsBytes(empty));
  ASSERT_TRUE(decoded_empty.ok()) << decoded_empty.status().ToString();
  EXPECT_EQ(decoded_empty.value(), (ti::ManifestSnapshot{1, 2, 0, {}}));

  auto decoded_table = ti::ManifestCodec::Decode(ti::AsBytes(one_table));
  ASSERT_TRUE(decoded_table.ok()) << decoded_table.status().ToString();
  EXPECT_EQ(
      decoded_table.value(),
      (ti::ManifestSnapshot{7, 11, 42, {ti::TableMeta{9, 100, "a", "z", 1, 42}}}));
}

TEST(ManifestCodecTest, WritesVersionTwoAndPreservesMultipleTableOrder) {
  const std::vector<ti::ManifestSnapshot> snapshots{
      {1, 2, 0, {}},
      {7, 10, 2, {{2, 100, "a", "z", 1, 2}}},
      {7,
       10,
       6,
       {{2, 100, "", "m", 1, 2},
        {4, 200, "n", "z", 3, 4},
        {6, 300, std::string("\x80", 1), std::string("\xff", 1), 5, 6}}}};

  for (const auto& snapshot : snapshots) {
    auto encoded = ti::ManifestCodec::Encode(snapshot);
    ASSERT_TRUE(encoded.ok()) << encoded.status().ToString();

    std::uint16_t version = 0;
    ASSERT_TRUE(ti::GetFixed16(ti::AsBytes(encoded.value()), 4, version));
    EXPECT_EQ(version, 2U);
    EXPECT_FALSE(LegacyV1FrameGateAccepts(encoded.value()));

    auto decoded = ti::ManifestCodec::Decode(ti::AsBytes(encoded.value()));
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    EXPECT_EQ(decoded.value(), snapshot);
  }
}

TEST(ManifestCodecTest, RejectsFramingChecksumAndUnknownFieldDamage) {
  const ti::ManifestSnapshot snapshot{7, 11, 42, {{9, 100, "a", "z", 1, 42}}};
  auto encoded = ti::ManifestCodec::Encode(snapshot);
  ASSERT_TRUE(encoded.ok());

  for (const std::size_t offset : {0U, 4U, 6U, 8U, 12U, 16U}) {
    SCOPED_TRACE(offset);
    std::string corrupt = encoded.value();
    corrupt[offset] ^= 1;
    EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(corrupt)).status().code(),
              tinylsm::StatusCode::kCorruption);
  }

  std::string top_level_unknown = ManifestPayload(encoded.value());
  top_level_unknown.append("\x28\x01", 2);
  auto top_level = ManifestFrame(2, top_level_unknown);
  EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(top_level)).status().code(),
            tinylsm::StatusCode::kCorruption);

  std::string nested_unknown = ManifestPayload(encoded.value());
  ASSERT_EQ(static_cast<unsigned char>(nested_unknown[6]), 0x22U);
  ASSERT_EQ(static_cast<unsigned char>(nested_unknown[7]), 0x0eU);
  nested_unknown[7] = static_cast<char>(0x10);
  nested_unknown.insert(22, "\x38\x01", 2);
  auto nested = ManifestFrame(2, nested_unknown);
  EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(nested)).status().code(),
            tinylsm::StatusCode::kCorruption);

  auto multiple = ti::ManifestCodec::Encode(
      {7, 10, 4, {{2, 100, "a", "m", 1, 2}, {4, 100, "n", "z", 3, 4}}});
  ASSERT_TRUE(multiple.ok());
  auto invalid_v1 = ManifestFrame(1, ManifestPayload(multiple.value()));
  EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(invalid_v1)).status().code(),
            tinylsm::StatusCode::kCorruption);

  std::string missing_active = ManifestPayload(encoded.value());
  missing_active.erase(0, 2);
  auto semantic_damage = ManifestFrame(2, missing_active);
  EXPECT_EQ(ti::ManifestCodec::Decode(ti::AsBytes(semantic_damage)).status().code(),
            tinylsm::StatusCode::kCorruption);
}

TEST(ManifestCodecTest, EnforcesSnapshotInvariantsAndFileSizeLimit) {
  const ti::ManifestSnapshot valid{7,
                                   10,
                                   6,
                                   {{2, 100, "a", "m", 1, 2},
                                    {4, 100, "n", "z", 3, 4},
                                    {6, 100, "", std::string("\xff", 1), 5, 6}}};
  std::vector<ti::ManifestSnapshot> invalid;

  auto changed = valid;
  changed.active_wal_number = 0;
  invalid.push_back(changed);
  changed = valid;
  changed.next_file_number = 7;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].file_number = 2;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].file_number = 7;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].file_size = 0;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].smallest_key = "zz";
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].min_sequence = 0;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].max_sequence = 7;
  invalid.push_back(changed);
  changed = valid;
  changed.live_tables[1].min_sequence = 2;
  invalid.push_back(changed);

  for (const auto& snapshot : invalid) {
    auto result = ti::ManifestCodec::Encode(snapshot);
    EXPECT_EQ(result.status().code(), tinylsm::StatusCode::kInvalidArgument);
  }

  auto oversized = valid;
  oversized.live_tables.back().largest_key.assign(ti::kMaxManifestFileBytes, 'x');
  EXPECT_EQ(ti::ManifestCodec::Encode(oversized).status().code(),
            tinylsm::StatusCode::kResourceExhausted);
  std::string{}.swap(oversized.live_tables.back().largest_key);

  std::vector<std::byte> oversized_file(ti::kMaxManifestFileBytes + 1);
  EXPECT_EQ(ti::ManifestCodec::Decode(oversized_file).status().code(),
            tinylsm::StatusCode::kCorruption);
}
