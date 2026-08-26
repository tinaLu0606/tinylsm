#include "sstable/sstable_builder.h"

#include <algorithm>
#include <limits>

#include "sstable/sstable_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace tinylsm::internal {
namespace {
bool FitsU32(std::size_t n) { return n <= std::numeric_limits<std::uint32_t>::max(); }
} // namespace

Result<std::string> EncodeDataBlock(const std::vector<InternalEntry>& entries) {
  if (entries.empty())
    return Status::InvalidArgument("cannot encode empty data block");
  std::string out;
  std::vector<std::uint32_t> offsets;
  for (const auto& e : entries) {
    if (!FitsU32(out.size()) || !FitsU32(e.user_key.size()) || !FitsU32(e.value.size()))
      return Status::InvalidArgument("SSTable entry is too large");
    offsets.push_back(static_cast<std::uint32_t>(out.size()));
    PutFixed64(out, e.sequence);
    out.push_back(static_cast<char>(e.type));
    PutFixed32(out, static_cast<std::uint32_t>(e.user_key.size()));
    PutFixed32(out, static_cast<std::uint32_t>(e.value.size()));
    out += e.user_key;
    out += e.value;
  }
  for (auto offset : offsets)
    PutFixed32(out, offset);
  PutFixed32(out, static_cast<std::uint32_t>(entries.size()));
  PutFixed32(out, Crc32c(AsBytes(out)));
  return out;
}

Result<std::vector<InternalEntry>> DecodeDataBlock(std::span<const std::byte> bytes) {
  if (bytes.size() < 8)
    return Status::Corruption("data block is truncated");
  std::uint32_t count = 0, expected = 0;
  GetFixed32(bytes, bytes.size() - 8, count);
  GetFixed32(bytes, bytes.size() - 4, expected);
  if (Crc32c(bytes.first(bytes.size() - 4)) != expected)
    return Status::Corruption("data block checksum mismatch");
  const std::uint64_t trailer = 8ULL + 4ULL * count;
  if (trailer > bytes.size())
    return Status::Corruption("data block offsets are truncated");
  const std::size_t offsets_start = bytes.size() - static_cast<std::size_t>(trailer);
  std::vector<InternalEntry> out;
  out.reserve(count);
  std::string previous;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t offset = 0, next = static_cast<std::uint32_t>(offsets_start);
    GetFixed32(bytes, offsets_start + 4ULL * i, offset);
    if (i + 1 < count)
      GetFixed32(bytes, offsets_start + 4ULL * (i + 1), next);
    if (offset > next || next > offsets_start || next - offset < 17)
      return Status::Corruption("data block entry offset is invalid");
    std::uint64_t seq = 0;
    std::uint32_t key_size = 0, value_size = 0;
    GetFixed64(bytes, offset, seq);
    GetFixed32(bytes, offset + 9, key_size);
    GetFixed32(bytes, offset + 13, value_size);
    if (17ULL + key_size + value_size != next - offset)
      return Status::Corruption("data block entry length is invalid");
    const auto type = std::to_integer<std::uint8_t>(bytes[offset + 8]);
    if (type != 1 && type != 2)
      return Status::Corruption("data block type is invalid");
    InternalEntry e;
    e.sequence = seq;
    e.type = static_cast<ValueType>(type);
    e.user_key.assign(reinterpret_cast<const char*>(bytes.data() + offset + 17),
                      key_size);
    e.value.assign(reinterpret_cast<const char*>(bytes.data() + offset + 17 + key_size),
                   value_size);
    if ((i > 0 && !(previous < e.user_key)) ||
        (e.type == ValueType::kTombstone && !e.value.empty()))
      return Status::Corruption("data block entries are not strictly ordered");
    previous = e.user_key;
    out.push_back(std::move(e));
  }
  return out;
}

Result<std::string> EncodeIndex(const std::vector<BlockMeta>& blocks) {
  std::string out;
  PutFixed32(out, static_cast<std::uint32_t>(blocks.size()));
  for (const auto& b : blocks) {
    if (!FitsU32(b.first_key.size()) || !FitsU32(b.last_key.size()))
      return Status::InvalidArgument("index key too large");
    PutFixed32(out, static_cast<std::uint32_t>(b.first_key.size()));
    PutFixed32(out, static_cast<std::uint32_t>(b.last_key.size()));
    PutFixed64(out, b.offset);
    PutFixed64(out, b.size);
    out += b.first_key;
    out += b.last_key;
  }
  PutFixed32(out, Crc32c(AsBytes(out)));
  return out;
}

Result<std::vector<BlockMeta>> DecodeIndex(std::span<const std::byte> bytes) {
  if (bytes.size() < 8)
    return Status::Corruption("index is truncated");
  std::uint32_t expected = 0, count = 0;
  GetFixed32(bytes, bytes.size() - 4, expected);
  GetFixed32(bytes, 0, count);
  if (Crc32c(bytes.first(bytes.size() - 4)) != expected)
    return Status::Corruption("index checksum mismatch");
  std::size_t p = 4;
  std::vector<BlockMeta> out;
  out.reserve(count);
  std::string previous;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t first = 0, last = 0;
    std::uint64_t offset = 0, size = 0;
    if (!GetFixed32(bytes, p, first) || !GetFixed32(bytes, p + 4, last) ||
        !GetFixed64(bytes, p + 8, offset) || !GetFixed64(bytes, p + 16, size))
      return Status::Corruption("index entry is truncated");
    p += 24;
    if (static_cast<std::uint64_t>(first) + last > bytes.size() - 4 - p)
      return Status::Corruption("index key length is invalid");
    BlockMeta b;
    b.offset = offset;
    b.size = size;
    b.first_key.assign(reinterpret_cast<const char*>(bytes.data() + p), first);
    p += first;
    b.last_key.assign(reinterpret_cast<const char*>(bytes.data() + p), last);
    p += last;
    if (b.first_key > b.last_key || (i > 0 && !(previous < b.first_key)))
      return Status::Corruption("index key ranges overlap");
    previous = b.last_key;
    out.push_back(std::move(b));
  }
  if (p != bytes.size() - 4)
    return Status::Corruption("index has trailing bytes");
  return out;
}

std::string EncodeFooter(const Footer& f) {
  std::string out;
  PutFixed32(out, kSstableVersion);
  PutFixed64(out, f.index_offset);
  PutFixed64(out, f.index_size);
  PutFixed64(out, kSstableMagic);
  PutFixed32(out, Crc32c(AsBytes(out)));
  PutFixed32(out, 0);
  return out;
}
Result<Footer> DecodeFooter(std::span<const std::byte> bytes) {
  if (bytes.size() != kSstableFooterSize)
    return Status::Corruption("footer size is invalid");
  std::uint32_t version = 0, expected = 0, reserved = 0;
  std::uint64_t magic = 0;
  Footer f;
  GetFixed32(bytes, 0, version);
  GetFixed64(bytes, 4, f.index_offset);
  GetFixed64(bytes, 12, f.index_size);
  GetFixed64(bytes, 20, magic);
  GetFixed32(bytes, 28, expected);
  GetFixed32(bytes, 32, reserved);
  if (version != kSstableVersion || magic != kSstableMagic || reserved != 0 ||
      Crc32c(bytes.first(28)) != expected)
    return Status::Corruption("footer is invalid");
  return f;
}

Status SSTableBuilder::Add(InternalEntry e) {
  if (finished_)
    return Status::InvalidArgument("SSTable builder is finished");
  if ((!pending_.empty() && !(pending_.back().user_key < e.user_key)) ||
      (!blocks_.empty() && !(blocks_.back().last_key < e.user_key)))
    return Status::InvalidArgument("SSTable keys must increase");
  const std::size_t estimate = 21 + e.user_key.size() + e.value.size();
  if (!pending_.empty() &&
      pending_bytes_ + estimate + (pending_.size() + 1) * 4 + 8 > block_bytes_) {
    auto s = FlushBlock();
    if (!s.ok())
      return s;
  }
  if (!has_entries_) {
    info_.smallest_key = e.user_key;
    info_.min_sequence = e.sequence;
    info_.max_sequence = e.sequence;
    has_entries_ = true;
  }
  info_.largest_key = e.user_key;
  info_.min_sequence = std::min(info_.min_sequence, e.sequence);
  info_.max_sequence = std::max(info_.max_sequence, e.sequence);
  pending_bytes_ += estimate;
  pending_.push_back(std::move(e));
  return Status::Ok();
}
Status SSTableBuilder::FlushBlock() {
  if (pending_.empty())
    return Status::Ok();
  auto encoded = EncodeDataBlock(pending_);
  if (!encoded.ok())
    return encoded.status();
  if (encoded.value().size() > std::numeric_limits<std::uint64_t>::max() - offset_)
    return Status::InvalidArgument("SSTable offset overflow");
  BlockMeta meta{pending_.front().user_key, pending_.back().user_key, offset_,
                 encoded.value().size()};
  auto s = file_->Append(AsBytes(encoded.value()));
  if (!s.ok())
    return s;
  offset_ += encoded.value().size();
  blocks_.push_back(std::move(meta));
  pending_.clear();
  pending_bytes_ = 0;
  return Status::Ok();
}
Result<BuiltTableInfo> SSTableBuilder::Finish() {
  if (finished_)
    return Status::InvalidArgument("SSTable builder already finished");
  if (pending_.empty() && blocks_.empty())
    return Status::InvalidArgument("cannot build empty SSTable");
  auto s = FlushBlock();
  if (!s.ok())
    return s;
  auto index = EncodeIndex(blocks_);
  if (!index.ok())
    return index.status();
  if (index.value().size() > std::numeric_limits<std::uint64_t>::max() - offset_)
    return Status::InvalidArgument("SSTable index offset overflow");
  Footer f{offset_, index.value().size()};
  s = file_->Append(AsBytes(index.value()));
  if (!s.ok())
    return s;
  offset_ += index.value().size();
  auto footer = EncodeFooter(f);
  s = file_->Append(AsBytes(footer));
  if (!s.ok())
    return s;
  offset_ += footer.size();
  s = file_->Sync();
  if (!s.ok())
    return s;
  s = file_->Close();
  if (!s.ok())
    return s;
  finished_ = true;
  info_.file_size = offset_;
  return info_;
}

} // namespace tinylsm::internal
