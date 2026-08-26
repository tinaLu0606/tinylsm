#include "wal/wal_reader.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "util/coding.h"

namespace tinylsm::internal {
namespace {
Result<std::size_t> ReadUpTo(SequentialFile& file, std::span<std::byte> dst) {
  std::size_t done = 0;
  while (done < dst.size()) {
    auto n = file.Read(dst.subspan(done));
    if (!n.ok())
      return n.status();
    if (n.value() == 0)
      break;
    done += n.value();
  }
  return done;
}
} // namespace

Result<WalReplayResult>
WalReader::Replay(const std::function<Status(const InternalEntry&)>& apply) {
  WalReplayResult result;
  while (true) {
    std::array<std::byte, kWalHeaderSize> header{};
    auto header_read = ReadUpTo(*file_, header);
    if (!header_read.ok())
      return header_read.status();
    if (header_read.value() == 0)
      return result;
    if (header_read.value() < header.size()) {
      result.truncated_tail = true;
      return result;
    }
    std::uint32_t magic = 0, payload_size = 0;
    std::uint16_t version = 0;
    GetFixed32(header, 0, magic);
    GetFixed16(header, 4, version);
    GetFixed32(header, 8, payload_size);
    if (magic != kWalMagic || version != kWalVersion)
      return Status::Corruption("invalid WAL record in the middle of the log");
    const std::uint64_t max_payload =
        static_cast<std::uint64_t>(kWalPayloadHeaderSize) + limits_.max_key_bytes +
        limits_.max_value_bytes;
    if (payload_size > max_payload)
      return Status::Corruption("WAL payload exceeds decode limits");
    std::vector<std::byte> record(kWalHeaderSize + payload_size);
    std::copy(header.begin(), header.end(), record.begin());
    auto payload_read = ReadUpTo(*file_, std::span(record).subspan(kWalHeaderSize));
    if (!payload_read.ok())
      return payload_read.status();
    if (payload_read.value() < payload_size) {
      result.truncated_tail = true;
      return result;
    }
    auto decoded = DecodeWalRecord(record, limits_);
    if (!decoded.ok())
      return decoded.status();
    Status status = apply(decoded.value());
    if (!status.ok())
      return status;
    result.max_sequence = std::max(result.max_sequence, decoded.value().sequence);
    result.valid_bytes += record.size();
  }
}

} // namespace tinylsm::internal
