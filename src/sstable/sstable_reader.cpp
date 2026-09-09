#include "sstable/sstable_reader.h"

#include <algorithm>
#include <cassert>
#include <limits>

#include "util/bytewise_less.h"

namespace tinylsm::internal {

class SSTableReader::Iterator final : public InternalIterator {
public:
  Iterator(const SSTableReader& source, std::string_view end)
      : source_(source), end_(end), block_index_(source.blocks_.size()) {}

  Status Seek(std::string_view begin) override {
    const BytewiseLess less;
    block_index_ = 0;
    while (block_index_ < source_.blocks_.size() &&
           less(source_.blocks_[block_index_].last_key, begin)) {
      ++block_index_;
    }
    if (block_index_ == source_.blocks_.size() ||
        (!end_.empty() && !less(source_.blocks_[block_index_].first_key, end_))) {
      SetEof();
      return Status::Ok();
    }

    auto status = LoadBlock();
    if (!status.ok())
      return status;
    entry_index_ = static_cast<std::size_t>(
        std::lower_bound(entries_->begin(), entries_->end(), begin,
                         [&less](const InternalEntry& entry, std::string_view key) {
                           return less(entry.user_key, key);
                         }) -
        entries_->begin());
    UpdateValidity();
    return status_;
  }

  [[nodiscard]] bool Valid() const noexcept override { return status_.ok() && valid_; }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return (*entries_)[entry_index_];
  }

  Status Next() override {
    if (!status_.ok())
      return status_;
    if (!valid_)
      return Status::Ok();

    ++entry_index_;
    if (entry_index_ < entries_->size()) {
      UpdateValidity();
      return status_;
    }

    ++block_index_;
    if (block_index_ == source_.blocks_.size() ||
        (!end_.empty() &&
         !BytewiseLess{}(source_.blocks_[block_index_].first_key, end_))) {
      SetEof();
      return Status::Ok();
    }

    auto status = LoadBlock();
    if (!status.ok())
      return status;
    entry_index_ = 0;
    UpdateValidity();
    return status_;
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  Status LoadBlock() {
    auto entries = source_.ReadBlock(source_.blocks_[block_index_]);
    if (!entries.ok()) {
      status_ = entries.status();
      valid_ = false;
      entries_.reset();
      return status_;
    }
    entries_ = std::move(entries.value());
    return Status::Ok();
  }

  void UpdateValidity() {
    valid_ = entries_ && entry_index_ < entries_->size() &&
             (end_.empty() || BytewiseLess{}((*entries_)[entry_index_].user_key, end_));
    if (!valid_)
      SetEof();
  }

  void SetEof() {
    valid_ = false;
    entries_.reset();
    entry_index_ = 0;
    block_index_ = source_.blocks_.size();
  }

  const SSTableReader& source_;
  std::string end_;
  std::size_t block_index_;
  BlockCache::BlockPtr entries_;
  std::size_t entry_index_ = 0;
  bool valid_ = false;
  Status status_;
};

Result<std::unique_ptr<SSTableReader>>
SSTableReader::Open(std::unique_ptr<RandomAccessFile> file, std::uint64_t table_number,
                    std::shared_ptr<BlockCache> block_cache,
                    std::shared_ptr<ReadMetricsState> metrics) {
  auto size = file->Size();
  if (!size.ok())
    return size.status();
  if (size.value() < kSstableV1FooterSize)
    return Status::Corruption("SSTable is too short");

  std::vector<std::byte> v1_footer_bytes(kSstableV1FooterSize);
  auto s = ReadExactly(*file, size.value() - kSstableV1FooterSize, v1_footer_bytes);
  if (!s.ok())
    return s;
  auto v1_footer = DecodeFooter(v1_footer_bytes);
  Footer footer;
  if (v1_footer.ok()) {
    footer = v1_footer.value();
  } else {
    if (size.value() < kSstableFooterSize)
      return v1_footer.status();
    std::vector<std::byte> v2_footer_bytes(kSstableFooterSize);
    s = ReadExactly(*file, size.value() - kSstableFooterSize, v2_footer_bytes);
    if (!s.ok())
      return s;
    auto v2_footer = DecodeFooter(v2_footer_bytes);
    if (!v2_footer.ok())
      return v2_footer.status();
    footer = v2_footer.value();
  }
  const std::size_t footer_size =
      footer.version == kSstableVersionV1 ? kSstableV1FooterSize : kSstableFooterSize;
  const std::uint64_t sections_end = size.value() - footer_size;

  if (footer.index_offset > sections_end ||
      footer.index_size > sections_end - footer.index_offset)
    return Status::Corruption("SSTable index range is invalid");
  if (footer.version == kSstableVersionV1) {
    if (footer.index_offset + footer.index_size != sections_end)
      return Status::Corruption("v1 SSTable contains unreferenced bytes");
  } else if (footer.index_offset + footer.index_size != footer.properties_offset ||
             footer.properties_offset > sections_end ||
             footer.properties_size > sections_end - footer.properties_offset ||
             footer.properties_offset + footer.properties_size != sections_end) {
    return Status::Corruption("v2 SSTable section ranges are invalid");
  }

  std::vector<std::byte> index_bytes(footer.index_size);
  s = ReadExactly(*file, footer.index_offset, index_bytes);
  if (!s.ok())
    return s;
  auto blocks = DecodeIndex(index_bytes);
  if (!blocks.ok())
    return blocks.status();

  std::optional<TableProperties> persisted_properties;
  if (footer.version == kSstableVersion) {
    std::vector<std::byte> properties_bytes(footer.properties_size);
    s = ReadExactly(*file, footer.properties_offset, properties_bytes);
    if (!s.ok())
      return s;
    auto properties = DecodeProperties(properties_bytes);
    if (!properties.ok())
      return properties.status();
    persisted_properties = std::move(properties.value());
  }
  for (const auto& b : blocks.value())
    if (b.offset > footer.index_offset || b.size > footer.index_offset - b.offset)
      return Status::Corruption("SSTable block range is invalid");
  return std::unique_ptr<SSTableReader>(
      new SSTableReader(std::move(file), size.value(), footer.version,
                        std::move(blocks.value()), std::move(persisted_properties),
                        table_number, std::move(block_cache), std::move(metrics)));
}
Result<BlockCache::BlockPtr> SSTableReader::ReadBlock(const BlockMeta& m,
                                                      bool use_cache) const {
  const BlockCacheKey key{table_number_, m.offset};
  if (use_cache && block_cache_) {
    auto cached = block_cache_->Lookup(key);
    if (cached)
      return cached;
  }

  if (m.size > std::numeric_limits<std::size_t>::max())
    return Status::Corruption("SSTable block is too large for this platform");
  std::vector<std::byte> bytes(m.size);
  auto s = ReadExactly(*file_, m.offset, bytes);
  if (!s.ok())
    return s;
  if (metrics_)
    metrics_->block_reads.fetch_add(1, std::memory_order_relaxed);
  auto entries = DecodeDataBlock(bytes, version_);
  if (!entries.ok())
    return entries.status();
  if (metrics_)
    metrics_->block_decodes.fetch_add(1, std::memory_order_relaxed);
  if (entries.value().empty())
    return Status::Corruption("SSTable contains an empty data block");
  if (entries.value().front().user_key != m.first_key ||
      entries.value().back().user_key != m.last_key)
    return Status::Corruption("SSTable index key range does not match data block");
  for (const auto& entry : entries.value())
    if (entry.sequence == 0)
      return Status::Corruption("SSTable entry sequence is invalid");
  auto owned = std::make_shared<const BlockCache::Block>(std::move(entries.value()));
  if (use_cache && block_cache_)
    block_cache_->Insert(key, owned, BlockCharge(*owned));
  return owned;
}

Result<InternalEntry> SSTableReader::FindInV2Block(const BlockMeta& m,
                                                   std::string_view key,
                                                   std::uint64_t sequence) const {
  if (m.size > std::numeric_limits<std::size_t>::max())
    return Status::Corruption("SSTable block is too large for this platform");
  std::vector<std::byte> bytes(m.size);
  auto status = ReadExactly(*file_, m.offset, bytes);
  if (!status.ok())
    return status;
  if (metrics_) {
    metrics_->block_reads.fetch_add(1, std::memory_order_relaxed);
    metrics_->block_decodes.fetch_add(1, std::memory_order_relaxed);
  }
  return FindDataBlockEntry(bytes, kSstableVersion, key, sequence);
}
Result<SSTableProperties> SSTableReader::ValidateAndGetProperties() const {
  if (blocks_.empty())
    return Status::Corruption("SSTable contains no data blocks");

  SSTableProperties properties;
  properties.file_size = file_size_;
  BytewiseLess less;
  std::string previous_key;
  bool has_entries = false;

  for (const auto& block : blocks_) {
    auto entries = ReadBlock(block, false);
    if (!entries.ok())
      return entries.status();
    if (has_entries && !less(previous_key, entries.value()->front().user_key))
      return Status::Corruption("SSTable data block keys are not strictly ordered");

    for (const auto& entry : *entries.value()) {
      if (!has_entries) {
        properties.smallest_key = entry.user_key;
        properties.min_sequence = entry.sequence;
        properties.max_sequence = entry.sequence;
        has_entries = true;
      }
      properties.largest_key = entry.user_key;
      properties.min_sequence = std::min(properties.min_sequence, entry.sequence);
      properties.max_sequence = std::max(properties.max_sequence, entry.sequence);
      ++properties.entry_count;
    }
    previous_key = entries.value()->back().user_key;
  }

  if (persisted_properties_ &&
      (properties.entry_count != persisted_properties_->entry_count ||
       properties.smallest_key != persisted_properties_->smallest_key ||
       properties.largest_key != persisted_properties_->largest_key ||
       properties.min_sequence != persisted_properties_->min_sequence ||
       properties.max_sequence != persisted_properties_->max_sequence)) {
    return Status::Corruption("SSTable properties do not match data blocks");
  }
  return properties;
}

Result<std::unique_ptr<InternalIterator>>
SSTableReader::NewIterator(std::string_view begin, std::string_view end) const {
  const BytewiseLess less;
  if (!end.empty() && less(end, begin))
    return Status::InvalidArgument("iterator begin is greater than end");
  auto iterator = std::unique_ptr<Iterator>(new Iterator(*this, end));
  auto status = iterator->Seek(begin);
  if (!status.ok())
    return status;
  return std::unique_ptr<InternalIterator>(std::move(iterator));
}
Result<InternalEntry> SSTableReader::Get(std::string_view key,
                                         std::uint64_t sequence) const {
  if (metrics_)
    metrics_->table_probes.fetch_add(1, std::memory_order_relaxed);
  auto it = std::lower_bound(
      blocks_.begin(), blocks_.end(), key,
      [](const BlockMeta& b, std::string_view k) { return b.last_key < k; });
  if (it == blocks_.end() || key < it->first_key)
    return Status::NotFound("key is absent");

  if (version_ == kSstableVersion && (!block_cache_ || !block_cache_->enabled()))
    return FindInV2Block(*it, key, sequence);

  auto entries = ReadBlock(*it);
  if (!entries.ok())
    return entries.status();

  auto e = std::lower_bound(
      entries.value()->begin(), entries.value()->end(), key,
      [](const InternalEntry& a, std::string_view k) { return a.user_key < k; });
  if (e == entries.value()->end() || e->user_key != key)
    return Status::NotFound("key is absent");
  while (e != entries.value()->end() && e->user_key == key) {
    if (e->sequence <= sequence)
      return *e;
    ++e;
  }
  return Status::NotFound("key has no visible version");
}
Result<std::vector<InternalEntry>> SSTableReader::Scan(std::string_view begin,
                                                       std::string_view end) const {
  auto iterator = NewIterator(begin, end);
  if (!iterator.ok())
    return iterator.status();

  std::vector<InternalEntry> out;
  while (iterator.value()->Valid()) {
    out.push_back(iterator.value()->entry());
    auto status = iterator.value()->Next();
    if (!status.ok())
      return status;
  }
  if (!iterator.value()->status().ok())
    return iterator.value()->status();
  return out;
}
} // namespace tinylsm::internal
