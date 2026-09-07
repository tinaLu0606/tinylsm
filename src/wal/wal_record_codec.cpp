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

Result<std::string> EncodeWalBatch(std::span<const InternalEntry> entries,
                                   const DecodeLimits& limits) {
  if (entries.empty())
    return Status::InvalidArgument("WAL batch is empty");
  if (entries.size() > kMaxWalBatchOperations)
    return Status::InvalidArgument("WAL batch has too many operations");

  std::uint64_t payload_size = kWalBatchPayloadHeaderSize;
  std::uint64_t previous_sequence = 0;
  for (const auto& entry : entries) {
    if (entry.sequence == 0 ||
        (previous_sequence != 0 && entry.sequence != previous_sequence + 1)) {
      return Status::InvalidArgument("WAL batch sequences are not consecutive");
    }
    if (entry.user_key.size() > limits.max_key_bytes ||
        entry.value.size() > limits.max_value_bytes ||
        entry.user_key.size() > std::numeric_limits<std::uint32_t>::max() ||
        entry.value.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::InvalidArgument("WAL batch key or value exceeds decode limits");
    }
    if (entry.type == ValueType::kTombstone && !entry.value.empty())
      return Status::InvalidArgument("WAL batch tombstone has a value");
    if (entry.type != ValueType::kValue && entry.type != ValueType::kTombstone)
      return Status::InvalidArgument("WAL batch value type is invalid");

    payload_size +=
        kWalBatchEntryHeaderSize + entry.user_key.size() + entry.value.size();
    if (payload_size > kMaxWalBatchBytes ||
        payload_size > std::numeric_limits<std::uint32_t>::max()) {
      return Status::InvalidArgument("WAL batch exceeds encoded size limit");
    }
    previous_sequence = entry.sequence;
  }

  std::string payload;
  payload.reserve(static_cast<std::size_t>(payload_size));
  PutFixed32(payload, static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    PutFixed64(payload, entry.sequence);
    payload.push_back(static_cast<char>(entry.type));
    payload.append(3, '\0');
    PutFixed32(payload, static_cast<std::uint32_t>(entry.user_key.size()));
    PutFixed32(payload, static_cast<std::uint32_t>(entry.value.size()));
    payload += entry.user_key;
    payload += entry.value;
  }

  std::string checked(1, static_cast<char>(kWalBatchRecordType));
  checked += payload;

  std::string out;
  PutFixed32(out, kWalMagic);
  PutFixed16(out, kWalBatchVersion);
  out.push_back(static_cast<char>(kWalBatchRecordType));
  out.push_back(0);
  PutFixed32(out, static_cast<std::uint32_t>(payload.size()));
  PutFixed32(out, Crc32c(AsBytes(checked)));
  out += payload;
  return out;
}

Result<std::vector<InternalEntry>> DecodeWalBatch(std::span<const std::byte> record,
                                                  const DecodeLimits& limits) {
  if (record.size() < kWalHeaderSize + kWalBatchPayloadHeaderSize)
    return Status::Corruption("WAL batch is truncated");

  std::uint32_t magic = 0, payload_size = 0, expected_crc = 0;
  std::uint16_t version = 0;
  GetFixed32(record, 0, magic);
  GetFixed16(record, 4, version);
  GetFixed32(record, 8, payload_size);
  GetFixed32(record, 12, expected_crc);
  if (magic != kWalMagic || version != kWalBatchVersion ||
      std::to_integer<std::uint8_t>(record[6]) != kWalBatchRecordType ||
      std::to_integer<std::uint8_t>(record[7]) != 0) {
    return Status::Corruption("WAL batch header is invalid");
  }
  if (payload_size != record.size() - kWalHeaderSize ||
      payload_size > kMaxWalBatchBytes) {
    return Status::Corruption("WAL batch payload length is invalid");
  }

  std::string checked(1, static_cast<char>(kWalBatchRecordType));
  checked.append(reinterpret_cast<const char*>(record.data() + kWalHeaderSize),
                 payload_size);
  if (Crc32c(AsBytes(checked)) != expected_crc)
    return Status::Corruption("WAL batch checksum mismatch");

  std::uint32_t count = 0;
  GetFixed32(record, kWalHeaderSize, count);
  if (count == 0 || count > kMaxWalBatchOperations ||
      static_cast<std::uint64_t>(count) * kWalBatchEntryHeaderSize >
          payload_size - kWalBatchPayloadHeaderSize) {
    return Status::Corruption("WAL batch operation count is invalid");
  }

  std::vector<InternalEntry> entries;
  entries.reserve(count);
  std::size_t offset = kWalHeaderSize + kWalBatchPayloadHeaderSize;
  std::uint64_t previous_sequence = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (record.size() - offset < kWalBatchEntryHeaderSize)
      return Status::Corruption("WAL batch entry header is truncated");

    std::uint64_t sequence = 0;
    std::uint32_t key_size = 0, value_size = 0;
    GetFixed64(record, offset, sequence);
    const auto type_byte = std::to_integer<std::uint8_t>(record[offset + 8]);
    if (std::to_integer<std::uint8_t>(record[offset + 9]) != 0 ||
        std::to_integer<std::uint8_t>(record[offset + 10]) != 0 ||
        std::to_integer<std::uint8_t>(record[offset + 11]) != 0) {
      return Status::Corruption("WAL batch entry flags are invalid");
    }
    GetFixed32(record, offset + 12, key_size);
    GetFixed32(record, offset + 16, value_size);
    offset += kWalBatchEntryHeaderSize;

    if (sequence == 0 ||
        (previous_sequence != 0 &&
         (previous_sequence == std::numeric_limits<std::uint64_t>::max() ||
          sequence != previous_sequence + 1))) {
      return Status::Corruption("WAL batch sequences are not consecutive");
    }
    if (type_byte != static_cast<std::uint8_t>(ValueType::kValue) &&
        type_byte != static_cast<std::uint8_t>(ValueType::kTombstone)) {
      return Status::Corruption("WAL batch value type is invalid");
    }
    if (key_size > limits.max_key_bytes || value_size > limits.max_value_bytes ||
        static_cast<std::uint64_t>(key_size) + value_size > record.size() - offset) {
      return Status::Corruption("WAL batch field length is invalid");
    }

    InternalEntry entry;
    entry.sequence = sequence;
    entry.type = static_cast<ValueType>(type_byte);
    entry.user_key.assign(reinterpret_cast<const char*>(record.data() + offset),
                          key_size);
    offset += key_size;
    entry.value.assign(reinterpret_cast<const char*>(record.data() + offset),
                       value_size);
    offset += value_size;
    if (entry.type == ValueType::kTombstone && !entry.value.empty())
      return Status::Corruption("WAL batch tombstone has a value");

    entries.push_back(std::move(entry));
    previous_sequence = sequence;
  }
  if (offset != record.size())
    return Status::Corruption("WAL batch contains trailing bytes");
  return entries;
}

} // namespace tinylsm::internal
