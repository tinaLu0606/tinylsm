#include "wal/wal_record_codec.h"

#include <limits>

#include "util/coding.h"
#include "util/crc32c.h"

namespace tinylsm::internal {

Result<std::string> EncodeWalRecord(const InternalEntry& e,
                                    const DecodeLimits& limits) {
  if (e.user_key.size() > limits.max_key_bytes ||
      e.value.size() > limits.max_value_bytes ||
      e.user_key.size() > std::numeric_limits<std::uint32_t>::max() ||
      e.value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("WAL key or value exceeds decode limits");
  }
  if (e.type == ValueType::kTombstone && !e.value.empty()) {
    return Status::InvalidArgument("tombstone WAL record has a value");
  }
  std::string payload;
  PutFixed64(payload, e.sequence);
  PutFixed32(payload, static_cast<std::uint32_t>(e.user_key.size()));
  PutFixed32(payload, static_cast<std::uint32_t>(e.value.size()));
  payload += e.user_key;
  payload += e.value;
  std::string checked;
  checked.push_back(static_cast<char>(e.type));
  checked += payload;
  std::string out;
  PutFixed32(out, kWalMagic);
  PutFixed16(out, kWalVersion);
  out.push_back(static_cast<char>(e.type));
  out.push_back(0);
  PutFixed32(out, static_cast<std::uint32_t>(payload.size()));
  PutFixed32(out, Crc32c(AsBytes(checked)));
  out += payload;
  return out;
}

Result<InternalEntry> DecodeWalRecord(std::span<const std::byte> record,
                                      const DecodeLimits& limits) {
  if (record.size() < kWalHeaderSize + kWalPayloadHeaderSize)
    return Status::Corruption("WAL record is truncated");
  std::uint32_t magic = 0, payload_size = 0, expected_crc = 0;
  std::uint16_t version = 0;
  GetFixed32(record, 0, magic);
  GetFixed16(record, 4, version);
  GetFixed32(record, 8, payload_size);
  GetFixed32(record, 12, expected_crc);
  if (magic != kWalMagic || version != kWalVersion ||
      std::to_integer<std::uint8_t>(record[7]) != 0)
    return Status::Corruption("WAL header is invalid");
  if (payload_size != record.size() - kWalHeaderSize)
    return Status::Corruption("WAL payload length is invalid");
  const auto type_byte = std::to_integer<std::uint8_t>(record[6]);
  if (type_byte != static_cast<std::uint8_t>(ValueType::kValue) &&
      type_byte != static_cast<std::uint8_t>(ValueType::kTombstone))
    return Status::Corruption("WAL value type is invalid");
  std::string checked(1, static_cast<char>(type_byte));
  checked.append(reinterpret_cast<const char*>(record.data() + kWalHeaderSize),
                 payload_size);
  if (Crc32c(AsBytes(checked)) != expected_crc)
    return Status::Corruption("WAL checksum mismatch");
  std::uint64_t seq = 0;
  std::uint32_t key_size = 0, value_size = 0;
  GetFixed64(record, 16, seq);
  GetFixed32(record, 24, key_size);
  GetFixed32(record, 28, value_size);
  if (key_size > limits.max_key_bytes || value_size > limits.max_value_bytes ||
      static_cast<std::uint64_t>(key_size) + value_size + kWalPayloadHeaderSize !=
          payload_size)
    return Status::Corruption("WAL field length is invalid");
  InternalEntry e;
  e.sequence = seq;
  e.type = static_cast<ValueType>(type_byte);
  e.user_key.assign(reinterpret_cast<const char*>(record.data() + 32), key_size);
  e.value.assign(reinterpret_cast<const char*>(record.data() + 32 + key_size),
                 value_size);
  if (e.type == ValueType::kTombstone && !e.value.empty())
    return Status::Corruption("WAL tombstone has a value");
  return e;
}

} // namespace tinylsm::internal
