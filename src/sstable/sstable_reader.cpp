#include "sstable/sstable_reader.h"

#include <algorithm>
#include <limits>

namespace tinylsm::internal {
Result<std::unique_ptr<SSTableReader>> SSTableReader::Open(std::unique_ptr<RandomAccessFile> file) {
  auto size = file->Size();
  if (!size.ok())
    return size.status();
  if (size.value() < kSstableFooterSize)
    return Status::Corruption("SSTable is too short");
  std::vector<std::byte> footer_bytes(kSstableFooterSize);
  auto s = ReadExactly(*file, size.value() - kSstableFooterSize, footer_bytes);
  if (!s.ok())
    return s;
  auto footer = DecodeFooter(footer_bytes);
  if (!footer.ok())
    return footer.status();
  if (footer.value().index_offset > size.value() - kSstableFooterSize ||
      footer.value().index_size > size.value() - kSstableFooterSize - footer.value().index_offset)
    return Status::Corruption("SSTable index range is invalid");
  if (footer.value().index_offset + footer.value().index_size != size.value() - kSstableFooterSize)
    return Status::Corruption("SSTable contains unreferenced bytes");
  std::vector<std::byte> index_bytes(footer.value().index_size);
  s = ReadExactly(*file, footer.value().index_offset, index_bytes);
  if (!s.ok())
    return s;
  auto blocks = DecodeIndex(index_bytes);
  if (!blocks.ok())
    return blocks.status();
  for (const auto& b : blocks.value())
    if (b.offset > b.offset + b.size || b.offset + b.size > footer.value().index_offset)
      return Status::Corruption("SSTable block range is invalid");
  return std::unique_ptr<SSTableReader>(
      new SSTableReader(std::move(file), std::move(blocks.value())));
}
Result<std::vector<InternalEntry>> SSTableReader::ReadBlock(const BlockMeta& m) const {
  if (m.size > std::numeric_limits<std::size_t>::max())
    return Status::Corruption("SSTable block is too large for this platform");
  std::vector<std::byte> bytes(m.size);
  auto s = ReadExactly(*file_, m.offset, bytes);
  if (!s.ok())
    return s;
  return DecodeDataBlock(bytes);
}
Result<InternalEntry> SSTableReader::Get(std::string_view key) const {
  auto it = std::lower_bound(blocks_.begin(), blocks_.end(), key,
                             [](const BlockMeta& b, std::string_view k) { return b.last_key < k; });
  if (it == blocks_.end() || key < it->first_key)
    return Status::NotFound("key is absent");
  auto entries = ReadBlock(*it);
  if (!entries.ok())
    return entries.status();
  auto e =
      std::lower_bound(entries.value().begin(), entries.value().end(), key,
                       [](const InternalEntry& a, std::string_view k) { return a.user_key < k; });
  if (e == entries.value().end() || e->user_key != key)
    return Status::NotFound("key is absent");
  return *e;
}
Result<std::vector<InternalEntry>> SSTableReader::Scan(std::string_view begin,
                                                       std::string_view end) const {
  std::vector<InternalEntry> out;
  for (const auto& b : blocks_) {
    if (b.last_key < begin || (!end.empty() && b.first_key >= end))
      continue;
    auto entries = ReadBlock(b);
    if (!entries.ok())
      return entries.status();
    for (const auto& e : entries.value())
      if (e.user_key >= begin && (end.empty() || e.user_key < end))
        out.push_back(e);
  }
  return out;
}
} // namespace tinylsm::internal
