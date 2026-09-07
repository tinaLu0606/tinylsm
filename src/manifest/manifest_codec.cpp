#include "manifest/manifest_codec.h"

#include <optional>
#include <string>
#include <unordered_set>

#include "manifest.pb.h"
#include "util/bytewise_less.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include <google/protobuf/message.h>

namespace tinylsm::internal {
namespace {
constexpr std::uint32_t kMagic = 0x31464e4dU;
constexpr std::uint16_t kVersion1 = 1;
constexpr std::uint16_t kVersion2 = 2;
constexpr std::uint16_t kVersion3 = 3;

std::optional<std::string> ValidateSnapshot(const ManifestSnapshot& snapshot) {
  if (snapshot.active_wal_number == 0)
    return "manifest active WAL number is invalid";
  if (snapshot.immutable_wal_number == snapshot.active_wal_number)
    return "manifest active and immutable WAL numbers collide";
  if (snapshot.next_file_number <= snapshot.active_wal_number ||
      snapshot.next_file_number <= snapshot.immutable_wal_number)
    return "manifest next file number is invalid";

  std::unordered_set<std::uint64_t> file_numbers;
  file_numbers.reserve(snapshot.live_tables.size());
  BytewiseLess less;

  for (std::size_t i = 0; i < snapshot.live_tables.size(); ++i) {
    const auto& table = snapshot.live_tables[i];
    if (table.file_number == 0 || table.file_number == snapshot.active_wal_number ||
        table.file_number == snapshot.immutable_wal_number ||
        table.file_number >= snapshot.next_file_number ||
        !file_numbers.insert(table.file_number).second)
      return "manifest table file numbers are invalid";
    if (table.file_size == 0 || less(table.largest_key, table.smallest_key))
      return "manifest table key metadata is invalid";
    if (table.min_sequence == 0 || table.max_sequence == 0 ||
        table.min_sequence > table.max_sequence ||
        table.max_sequence > snapshot.last_sequence)
      return "manifest table sequence metadata is invalid";
    if (i > 0 && snapshot.live_tables[i - 1].max_sequence >= table.min_sequence)
      return "manifest table sequence ranges are not strictly ordered";
  }
  return std::nullopt;
}

bool HasUnknownFields(const google::protobuf::Message& message) {
  return message.GetReflection()->GetUnknownFields(message).field_count() != 0;
}
} // namespace

Result<std::string> ManifestCodec::Encode(const ManifestSnapshot& s) {
  if (auto invalid = ValidateSnapshot(s))
    return Status::InvalidArgument(std::move(*invalid));
  for (const auto& table : s.live_tables) {
    if (table.smallest_key.size() > kMaxManifestFileBytes - kManifestHeaderBytes ||
        table.largest_key.size() > kMaxManifestFileBytes - kManifestHeaderBytes)
      return Status::ResourceExhausted("manifest exceeds the file size limit");
  }

  proto::ManifestSnapshotProto message;
  message.set_active_wal_number(s.active_wal_number);
  message.set_immutable_wal_number(s.immutable_wal_number);
  message.set_next_file_number(s.next_file_number);
  message.set_last_sequence(s.last_sequence);

  for (const auto& table : s.live_tables) {
    auto* encoded = message.add_live_tables();
    encoded->set_file_number(table.file_number);
    encoded->set_file_size(table.file_size);
    encoded->set_smallest_key(table.smallest_key);
    encoded->set_largest_key(table.largest_key);
    encoded->set_min_sequence(table.min_sequence);
    encoded->set_max_sequence(table.max_sequence);
  }

  const auto payload_size = message.ByteSizeLong();
  if (payload_size > kMaxManifestFileBytes - kManifestHeaderBytes)
    return Status::ResourceExhausted("manifest exceeds the file size limit");

  std::string payload;
  if (!message.SerializeToString(&payload))
    return Status::InvalidArgument("manifest protobuf serialization failed");

  std::string out;
  out.reserve(kManifestHeaderBytes + payload.size());
  PutFixed32(out, kMagic);
  PutFixed16(out, kVersion3);
  PutFixed16(out, 0);
  PutFixed32(out, static_cast<std::uint32_t>(payload.size()));
  PutFixed32(out, Crc32c(AsBytes(out), AsBytes(payload)));
  out += payload;
  return out;
}
Result<ManifestSnapshot> ManifestCodec::Decode(std::span<const std::byte> bytes) {
  if (bytes.size() < kManifestHeaderBytes || bytes.size() > kMaxManifestFileBytes)
    return Status::Corruption("manifest is truncated or too large");

  std::uint32_t magic = 0, size = 0, crc = 0;
  std::uint16_t version = 0, reserved = 0;
  GetFixed32(bytes, 0, magic);
  GetFixed16(bytes, 4, version);
  GetFixed16(bytes, 6, reserved);
  GetFixed32(bytes, 8, size);
  GetFixed32(bytes, 12, crc);

  if (magic != kMagic ||
      (version != kVersion1 && version != kVersion2 && version != kVersion3) ||
      reserved != 0 || size != bytes.size() - kManifestHeaderBytes)
    return Status::Corruption("manifest framing is invalid");

  const auto payload = bytes.subspan(kManifestHeaderBytes);
  const auto actual_crc =
      version == kVersion1 ? Crc32c(payload) : Crc32c(bytes.first(12), payload);
  if (actual_crc != crc)
    return Status::Corruption("manifest checksum mismatch");

  proto::ManifestSnapshotProto message;
  if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size())))
    return Status::Corruption("manifest protobuf payload is invalid");
  if (HasUnknownFields(message))
    return Status::Corruption("manifest contains unknown fields");
  if (version == kVersion1 && message.live_tables_size() > 1)
    return Status::Corruption("version 1 manifest contains multiple tables");

  ManifestSnapshot s;
  s.active_wal_number = message.active_wal_number();
  s.immutable_wal_number = version == kVersion3 ? message.immutable_wal_number() : 0;
  s.next_file_number = message.next_file_number();
  s.last_sequence = message.last_sequence();
  s.live_tables.reserve(message.live_tables_size());
  for (const auto& table : message.live_tables()) {
    if (HasUnknownFields(table))
      return Status::Corruption("manifest table contains unknown fields");
    s.live_tables.push_back({table.file_number(), table.file_size(),
                             table.smallest_key(), table.largest_key(),
                             table.min_sequence(), table.max_sequence()});
  }

  if (auto invalid = ValidateSnapshot(s))
    return Status::Corruption(std::move(*invalid));
  return s;
}
} // namespace tinylsm::internal
