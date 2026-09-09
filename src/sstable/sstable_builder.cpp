#include "sstable/sstable_builder.h"

#include <algorithm>
#include <limits>
#include <optional>

#include "sstable/sstable_format.h"
#include "util/bytewise_less.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace tinylsm::internal {
namespace {
bool FitsU32(std::size_t n) { return n <= std::numeric_limits<std::uint32_t>::max(); }
bool InternalLess(const InternalEntry& left, const InternalEntry& right) {
  const BytewiseLess less;
  return less(left.user_key, right.user_key) ||
         (left.user_key == right.user_key && left.sequence > right.sequence);
}
} // namespace

Result<std::string> EncodeDataBlockV1(const std::vector<InternalEntry>& entries) {
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

namespace {

Result<std::vector<InternalEntry>> DecodeDataBlockV1(std::span<const std::byte> bytes) {
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
  std::optional<InternalEntry> previous;

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

    if ((previous && !InternalLess(*previous, e)) ||
        (e.type == ValueType::kTombstone && !e.value.empty()))
      return Status::Corruption("data block entries are not strictly ordered");
    previous = e;
    out.push_back(std::move(e));
  }
  return out;
}

bool DecodeV2Entry(std::span<const std::byte> bytes, std::size_t& position,
                   std::size_t entries_end, std::string_view previous_key,
                   InternalEntry& entry) {
  std::uint32_t shared = 0, unshared = 0, value_size = 0;
  std::uint64_t sequence = 0;
  if (!GetFixed32(bytes, position, shared) ||
      !GetFixed32(bytes, position + 4, unshared) ||
      !GetFixed64(bytes, position + 8, sequence) ||
      !GetFixed32(bytes, position + 17, value_size)) {
    return false;
  }
  const std::size_t header_size = 21;
  if (shared > previous_key.size() || position > entries_end ||
      entries_end - position < header_size ||
      static_cast<std::uint64_t>(unshared) + value_size >
          entries_end - position - header_size) {
    return false;
  }
  const auto type = std::to_integer<std::uint8_t>(bytes[position + 16]);
  if ((type != 1 && type != 2) || sequence == 0)
    return false;

  entry.sequence = sequence;
  entry.type = static_cast<ValueType>(type);
  entry.user_key.assign(previous_key.substr(0, shared));
  entry.user_key.append(
      reinterpret_cast<const char*>(bytes.data() + position + header_size), unshared);
  entry.value.assign(
      reinterpret_cast<const char*>(bytes.data() + position + header_size + unshared),
      value_size);
  position += header_size + static_cast<std::size_t>(unshared) + value_size;
  return entry.type != ValueType::kTombstone || entry.value.empty();
}

struct V2BlockLayout {
  std::size_t entries_end = 0;
  std::vector<std::uint32_t> restarts;
  std::uint32_t entry_count = 0;
};

Result<V2BlockLayout> ParseV2BlockLayout(std::span<const std::byte> bytes) {
  constexpr std::size_t kTrailerBytes = 12;
  if (bytes.size() < kTrailerBytes)
    return Status::Corruption("v2 data block is truncated");

  std::uint32_t restart_count = 0, entry_count = 0, expected = 0;
  GetFixed32(bytes, bytes.size() - 12, restart_count);
  GetFixed32(bytes, bytes.size() - 8, entry_count);
  GetFixed32(bytes, bytes.size() - 4, expected);
  if (Crc32c(bytes.first(bytes.size() - 4)) != expected)
    return Status::Corruption("v2 data block checksum mismatch");
  if (entry_count == 0 || restart_count == 0 || restart_count > entry_count)
    return Status::Corruption("v2 data block restart count is invalid");

  const std::uint64_t restart_bytes = 4ULL * restart_count;
  if (restart_bytes > bytes.size() - kTrailerBytes)
    return Status::Corruption("v2 data block restarts are truncated");
  const std::size_t restarts_begin =
      bytes.size() - kTrailerBytes - static_cast<std::size_t>(restart_bytes);
  V2BlockLayout layout;
  layout.entries_end = restarts_begin;
  layout.entry_count = entry_count;
  layout.restarts.reserve(restart_count);
  for (std::uint32_t i = 0; i < restart_count; ++i) {
    std::uint32_t restart = 0;
    GetFixed32(bytes, restarts_begin + 4ULL * i, restart);
    if (restart >= restarts_begin || (i > 0 && restart <= layout.restarts.back())) {
      return Status::Corruption("v2 data block restart offset is invalid");
    }
    layout.restarts.push_back(restart);
  }
  if (layout.restarts.front() != 0)
    return Status::Corruption("v2 data block first restart is invalid");
  return layout;
}

Result<std::vector<InternalEntry>> DecodeDataBlockV2(std::span<const std::byte> bytes) {
  auto layout = ParseV2BlockLayout(bytes);
  if (!layout.ok())
    return layout.status();

  std::vector<InternalEntry> out;
  out.reserve(layout.value().entry_count);
  std::size_t position = 0;
  std::size_t restart_index = 0;
  std::string previous_key;
  for (std::uint32_t i = 0; i < layout.value().entry_count; ++i) {
    if (restart_index < layout.value().restarts.size() &&
        position == layout.value().restarts[restart_index]) {
      previous_key.clear();
      ++restart_index;
    }
    InternalEntry entry;
    if (!DecodeV2Entry(bytes, position, layout.value().entries_end, previous_key,
                       entry))
      return Status::Corruption("v2 data block entry is invalid");
    if (!out.empty() && !InternalLess(out.back(), entry)) {
      return Status::Corruption("v2 data block entries are not strictly ordered");
    }
    previous_key = entry.user_key;
    out.push_back(std::move(entry));
  }
  if (position != layout.value().entries_end ||
      restart_index != layout.value().restarts.size()) {
    return Status::Corruption("v2 data block entry or restart layout is invalid");
  }
  return out;
}

} // namespace

Result<std::string> EncodeDataBlock(const std::vector<InternalEntry>& entries,
                                    std::uint32_t restart_interval) {
  if (entries.empty())
    return Status::InvalidArgument("cannot encode empty data block");
  if (restart_interval == 0)
    return Status::InvalidArgument("SSTable restart interval must be non-zero");

  std::string out;
  std::vector<std::uint32_t> restarts;
  std::string previous_key;
  std::optional<InternalEntry> previous;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    if ((previous && !InternalLess(*previous, entry)) ||
        (entry.type == ValueType::kTombstone && !entry.value.empty())) {
      return Status::InvalidArgument("SSTable entries are not strictly ordered");
    }
    const bool restart = i % restart_interval == 0;
    const std::size_t shared =
        restart ? 0
                : std::mismatch(previous_key.begin(), previous_key.end(),
                                entry.user_key.begin(), entry.user_key.end())
                          .first -
                      previous_key.begin();
    const std::size_t unshared = entry.user_key.size() - shared;
    if (!FitsU32(out.size()) || !FitsU32(shared) || !FitsU32(unshared) ||
        !FitsU32(entry.value.size())) {
      return Status::InvalidArgument("SSTable entry is too large");
    }
    if (restart)
      restarts.push_back(static_cast<std::uint32_t>(out.size()));
    PutFixed32(out, static_cast<std::uint32_t>(shared));
    PutFixed32(out, static_cast<std::uint32_t>(unshared));
    PutFixed64(out, entry.sequence);
    out.push_back(static_cast<char>(entry.type));
    PutFixed32(out, static_cast<std::uint32_t>(entry.value.size()));
    out.append(entry.user_key.data() + shared, unshared);
    out += entry.value;
    previous_key = entry.user_key;
    previous = entry;
  }
  for (const auto restart : restarts)
    PutFixed32(out, restart);
  PutFixed32(out, static_cast<std::uint32_t>(restarts.size()));
  PutFixed32(out, static_cast<std::uint32_t>(entries.size()));
  PutFixed32(out, Crc32c(AsBytes(out)));
  return out;
}

Result<std::vector<InternalEntry>> DecodeDataBlock(std::span<const std::byte> bytes,
                                                   std::uint32_t version) {
  if (version == kSstableVersionV1)
    return DecodeDataBlockV1(bytes);
  if (version == kSstableVersion)
    return DecodeDataBlockV2(bytes);
  return Status::Corruption("SSTable data block version is unsupported");
}

Result<InternalEntry> FindDataBlockEntry(std::span<const std::byte> bytes,
                                         std::uint32_t version, std::string_view key,
                                         std::uint64_t sequence) {
  if (version == kSstableVersionV1) {
    auto entries = DecodeDataBlockV1(bytes);
    if (!entries.ok())
      return entries.status();
    const BytewiseLess less;
    for (const auto& entry : entries.value()) {
      if (less(key, entry.user_key))
        break;
      if (entry.user_key == key && entry.sequence <= sequence)
        return entry;
    }
    return Status::NotFound("key has no visible version");
  }
  if (version != kSstableVersion)
    return Status::Corruption("SSTable data block version is unsupported");

  auto layout = ParseV2BlockLayout(bytes);
  if (!layout.ok())
    return layout.status();
  const BytewiseLess less;
  const auto restart_key = [&](std::size_t index) -> Result<std::string> {
    std::size_t position = layout.value().restarts[index];
    InternalEntry entry;
    if (!DecodeV2Entry(bytes, position, layout.value().entries_end, {}, entry) ||
        position > layout.value().entries_end) {
      return Status::Corruption("v2 data block restart entry is invalid");
    }
    return entry.user_key;
  };

  std::size_t first = 0;
  std::size_t last = layout.value().restarts.size();
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    auto candidate = restart_key(middle);
    if (!candidate.ok())
      return candidate.status();
    if (!less(key, candidate.value()))
      first = middle + 1;
    else
      last = middle;
  }
  const std::size_t group = first == 0 ? 0 : first - 1;
  const std::size_t group_end = group + 1 == layout.value().restarts.size()
                                    ? layout.value().entries_end
                                    : layout.value().restarts[group + 1];
  std::size_t position = layout.value().restarts[group];
  std::string previous_key;
  while (position < group_end) {
    InternalEntry entry;
    if (!DecodeV2Entry(bytes, position, layout.value().entries_end, previous_key,
                       entry) ||
        position > group_end) {
      return Status::Corruption("v2 data block restart group is invalid");
    }
    if (less(entry.user_key, key)) {
      previous_key = entry.user_key;
      continue;
    }
    if (less(key, entry.user_key))
      break;
    if (entry.sequence <= sequence)
      return entry;
    previous_key = entry.user_key;
  }
  return Status::NotFound("key has no visible version");
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

Result<std::string> EncodeProperties(const TableProperties& properties) {
  if (properties.entry_count == 0 || properties.min_sequence == 0 ||
      properties.min_sequence > properties.max_sequence ||
      properties.smallest_key > properties.largest_key ||
      !FitsU32(properties.smallest_key.size()) ||
      !FitsU32(properties.largest_key.size())) {
    return Status::InvalidArgument("SSTable properties are invalid");
  }

  std::string out;
  PutFixed64(out, properties.entry_count);
  PutFixed64(out, properties.min_sequence);
  PutFixed64(out, properties.max_sequence);
  PutFixed32(out, static_cast<std::uint32_t>(properties.smallest_key.size()));
  PutFixed32(out, static_cast<std::uint32_t>(properties.largest_key.size()));
  out += properties.smallest_key;
  out += properties.largest_key;
  PutFixed32(out, Crc32c(AsBytes(out)));
  return out;
}

Result<TableProperties> DecodeProperties(std::span<const std::byte> bytes) {
  constexpr std::size_t kHeaderBytes = 32;
  constexpr std::size_t kChecksumBytes = 4;
  if (bytes.size() < kHeaderBytes + kChecksumBytes)
    return Status::Corruption("SSTable properties are truncated");

  std::uint32_t smallest_size = 0, largest_size = 0, expected = 0;
  TableProperties properties;
  GetFixed64(bytes, 0, properties.entry_count);
  GetFixed64(bytes, 8, properties.min_sequence);
  GetFixed64(bytes, 16, properties.max_sequence);
  GetFixed32(bytes, 24, smallest_size);
  GetFixed32(bytes, 28, largest_size);
  GetFixed32(bytes, bytes.size() - kChecksumBytes, expected);
  if (Crc32c(bytes.first(bytes.size() - kChecksumBytes)) != expected)
    return Status::Corruption("SSTable properties checksum mismatch");
  if (properties.entry_count == 0 || properties.min_sequence == 0 ||
      properties.min_sequence > properties.max_sequence ||
      static_cast<std::uint64_t>(smallest_size) + largest_size !=
          bytes.size() - kHeaderBytes - kChecksumBytes) {
    return Status::Corruption("SSTable properties length is invalid");
  }
  properties.smallest_key.assign(
      reinterpret_cast<const char*>(bytes.data() + kHeaderBytes), smallest_size);
  properties.largest_key.assign(
      reinterpret_cast<const char*>(bytes.data() + kHeaderBytes + smallest_size),
      largest_size);
  if (properties.smallest_key > properties.largest_key)
    return Status::Corruption("SSTable properties key range is invalid");
  return properties;
}

std::string EncodeFooter(const Footer& f) {
  std::string out;
  PutFixed32(out, f.version);
  PutFixed64(out, f.index_offset);
  PutFixed64(out, f.index_size);
  PutFixed64(out, f.properties_offset);
  PutFixed64(out, f.properties_size);
  PutFixed64(out, kSstableMagic);
  PutFixed32(out, Crc32c(AsBytes(out)));
  PutFixed32(out, 0);
  return out;
}

std::string EncodeFooterV1(std::uint64_t index_offset, std::uint64_t index_size) {
  std::string out;
  PutFixed32(out, kSstableVersionV1);
  PutFixed64(out, index_offset);
  PutFixed64(out, index_size);
  PutFixed64(out, kSstableMagic);
  PutFixed32(out, Crc32c(AsBytes(out)));
  PutFixed32(out, 0);
  return out;
}
Result<Footer> DecodeFooter(std::span<const std::byte> bytes) {
  if (bytes.size() != kSstableV1FooterSize && bytes.size() != kSstableFooterSize)
    return Status::Corruption("footer size is invalid");

  std::uint32_t version = 0, expected = 0, reserved = 0;
  std::uint64_t magic = 0;
  Footer f;
  GetFixed32(bytes, 0, version);
  GetFixed64(bytes, 4, f.index_offset);
  GetFixed64(bytes, 12, f.index_size);
  const bool is_v1_layout = bytes.size() == kSstableV1FooterSize;
  const std::size_t magic_offset = is_v1_layout ? 20 : 36;
  const std::size_t checksum_offset = is_v1_layout ? 28 : 44;
  const std::size_t reserved_offset = is_v1_layout ? 32 : 48;
  if (!is_v1_layout) {
    GetFixed64(bytes, 20, f.properties_offset);
    GetFixed64(bytes, 28, f.properties_size);
  }
  GetFixed64(bytes, magic_offset, magic);
  GetFixed32(bytes, checksum_offset, expected);
  GetFixed32(bytes, reserved_offset, reserved);

  if ((is_v1_layout && version != kSstableVersionV1) ||
      (!is_v1_layout && version != kSstableVersion) || magic != kSstableMagic ||
      reserved != 0 || Crc32c(bytes.first(checksum_offset)) != expected) {
    return Status::Corruption("footer is invalid");
  }
  f.version = version;
  return f;
}

Status SSTableBuilder::Add(InternalEntry e) {
  if (finished_)
    return Status::InvalidArgument("SSTable builder is finished");
  if (restart_interval_ == 0)
    return Status::InvalidArgument("SSTable restart interval must be non-zero");
  if ((!pending_.empty() && !InternalLess(pending_.back(), e)) ||
      (!blocks_.empty() && !(blocks_.back().last_key < e.user_key)))
    return Status::InvalidArgument("SSTable keys must increase");

  const std::size_t estimate = 21 + e.user_key.size() + e.value.size();
  if (!pending_.empty() && pending_.back().user_key != e.user_key &&
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
  if (info_.entry_count == std::numeric_limits<std::uint64_t>::max())
    return Status::ResourceExhausted("SSTable entry count is exhausted");
  ++info_.entry_count;
  pending_bytes_ += estimate;
  pending_.push_back(std::move(e));
  return Status::Ok();
}
Status SSTableBuilder::FlushBlock() {
  if (pending_.empty())
    return Status::Ok();

  auto encoded = EncodeDataBlock(pending_, restart_interval_);
  if (!encoded.ok())
    return encoded.status();
  if (encoded.value().size() > std::numeric_limits<std::uint64_t>::max() - offset_)
    return Status::ResourceExhausted("SSTable offset space is exhausted");

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
    return Status::ResourceExhausted("SSTable index offset space is exhausted");

  const std::uint64_t index_offset = offset_;
  s = file_->Append(AsBytes(index.value()));
  if (!s.ok())
    return s;
  offset_ += index.value().size();

  auto properties =
      EncodeProperties({info_.entry_count, info_.smallest_key, info_.largest_key,
                        info_.min_sequence, info_.max_sequence});
  if (!properties.ok())
    return properties.status();
  if (properties.value().size() > std::numeric_limits<std::uint64_t>::max() - offset_)
    return Status::ResourceExhausted("SSTable properties offset space is exhausted");
  const std::uint64_t properties_offset = offset_;
  s = file_->Append(AsBytes(properties.value()));
  if (!s.ok())
    return s;
  offset_ += properties.value().size();

  auto footer = EncodeFooter({kSstableVersion, index_offset, index.value().size(),
                              properties_offset, properties.value().size()});
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
