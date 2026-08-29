#include "manifest/manifest_codec.h"

#include <limits>

#include "manifest.pb.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace tinylsm::internal {
namespace {
constexpr std::uint32_t kMagic = 0x31464e4dU;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeader = 16;
} // namespace

Result<std::string> ManifestCodec::Encode(const ManifestSnapshot& s) {
  proto::ManifestSnapshotProto message;
  message.set_active_wal_number(s.active_wal_number);
  message.set_next_file_number(s.next_file_number);
  message.set_last_sequence(s.last_sequence);

  if (s.live_table) {
    auto* t = message.mutable_live_table();
    t->set_file_number(s.live_table->file_number);
    t->set_file_size(s.live_table->file_size);
    t->set_smallest_key(s.live_table->smallest_key);
    t->set_largest_key(s.live_table->largest_key);
    t->set_min_sequence(s.live_table->min_sequence);
    t->set_max_sequence(s.live_table->max_sequence);
  }

  std::string payload;
  if (!message.SerializeToString(&payload) ||
      payload.size() > std::numeric_limits<std::uint32_t>::max())
    return Status::InvalidArgument("manifest protobuf payload is too large");

  std::string out;
  PutFixed32(out, kMagic);
  PutFixed16(out, kVersion);
  PutFixed16(out, 0);
  PutFixed32(out, static_cast<std::uint32_t>(payload.size()));
  PutFixed32(out, Crc32c(AsBytes(payload)));
  out += payload;
  return out;
}
Result<ManifestSnapshot> ManifestCodec::Decode(std::span<const std::byte> bytes) {
  if (bytes.size() < kHeader ||
      bytes.size() - kHeader >
          static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return Status::Corruption("manifest is truncated or too large");

  std::uint32_t magic = 0, size = 0, crc = 0;
  std::uint16_t version = 0, reserved = 0;
  GetFixed32(bytes, 0, magic);
  GetFixed16(bytes, 4, version);
  GetFixed16(bytes, 6, reserved);
  GetFixed32(bytes, 8, size);
  GetFixed32(bytes, 12, crc);

  if (magic != kMagic || version != kVersion || reserved != 0 ||
      size != bytes.size() - kHeader)
    return Status::Corruption("manifest framing is invalid");

  auto payload = bytes.subspan(kHeader);
  if (Crc32c(payload) != crc)
    return Status::Corruption("manifest checksum mismatch");

  proto::ManifestSnapshotProto message;
  if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size())))
    return Status::Corruption("manifest protobuf payload is invalid");

  ManifestSnapshot s;
  s.active_wal_number = message.active_wal_number();
  s.next_file_number = message.next_file_number();
  s.last_sequence = message.last_sequence();
  if (s.active_wal_number == 0 || s.next_file_number == 0)
    return Status::Corruption("manifest file numbers are invalid");

  if (message.has_live_table()) {
    const auto& p = message.live_table();
    TableMeta t{p.file_number(), p.file_size(),    p.smallest_key(),
                p.largest_key(), p.min_sequence(), p.max_sequence()};
    if (t.file_number == 0 || t.smallest_key > t.largest_key ||
        t.min_sequence > t.max_sequence)
      return Status::Corruption("manifest table metadata is invalid");
    s.live_table = std::move(t);
  }

  return s;
}
} // namespace tinylsm::internal
