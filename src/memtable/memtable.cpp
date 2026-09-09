#include "memtable/memtable.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace tinylsm::internal {
namespace {
std::size_t EntryBytes(const InternalEntry& entry) {
  return sizeof(InternalEntry) + entry.user_key.size() + entry.value.size();
}
} // namespace

class MemTable::Iterator final : public InternalIterator {
public:
  Iterator(const MemTable& source, std::string_view begin, std::string_view end)
      : source_(source), current_(source.entries_.end()), end_(end) {
    Seek(begin).IgnoreError();
  }

  [[nodiscard]] bool Valid() const noexcept override {
    return current_ != source_.entries_.end() &&
           (end_.empty() || BytewiseLess{}(current_->first, end_));
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return current_->second[version_index_];
  }

  Status Seek(std::string_view target) override {
    current_ = source_.entries_.lower_bound(target);
    version_index_ = 0;
    return Status::Ok();
  }

  Status Next() override {
    if (!Valid())
      return Status::Ok();
    ++version_index_;
    if (version_index_ == current_->second.size()) {
      ++current_;
      version_index_ = 0;
    }
    return Status::Ok();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  const MemTable& source_;
  decltype(entries_)::const_iterator current_;
  std::size_t version_index_ = 0;
  std::string end_;
  Status status_;
};

Status MemTable::Apply(InternalEntry entry) {
  if (entry.sequence == 0)
    return Status::Corruption("memtable sequence is zero");
  const auto added = EntryBytes(entry);
  if (added > std::numeric_limits<std::size_t>::max() - bytes_)
    return Status::ResourceExhausted("memtable size accounting overflow");

  auto [it, inserted] = entries_.try_emplace(entry.user_key);
  auto& versions = it->second;
  if (!versions.empty() && entry.sequence <= versions.front().sequence)
    return Status::Corruption("memtable sequence did not increase");
  versions.insert(versions.begin(), std::move(entry));
  bytes_ += added;
  return Status::Ok();
}

Status MemTable::ApplyBatch(std::span<const InternalEntry> entries) {
  if (entries.empty())
    return Status::Ok();

  std::uint64_t previous_sequence = 0;
  std::size_t added_bytes = 0;
  for (const auto& entry : entries) {
    if (entry.sequence == 0 ||
        (previous_sequence != 0 && entry.sequence <= previous_sequence)) {
      return Status::Corruption("memtable batch sequence did not increase");
    }
    const auto added = EntryBytes(entry);
    if (added > std::numeric_limits<std::size_t>::max() - added_bytes)
      return Status::ResourceExhausted("memtable batch size overflow");
    added_bytes += added;
    previous_sequence = entry.sequence;
  }
  if (added_bytes > std::numeric_limits<std::size_t>::max() - bytes_)
    return Status::ResourceExhausted("memtable batch size overflow");

  // Build complete replacement vectors only for keys touched by this batch.
  // All allocation happens before entries_ is changed; the commit below uses
  // vector swaps and map node transfer, so allocation failure cannot expose a
  // partial WriteBatch without copying the whole MemTable.
  decltype(entries_) staged;
  for (const auto& entry : entries) {
    staged[entry.user_key].push_back(entry);
  }
  for (auto& [key, versions] : staged) {
    const auto existing = entries_.find(key);
    if (existing != entries_.end() && !existing->second.empty() &&
        versions.front().sequence <= existing->second.front().sequence) {
      return Status::Corruption("memtable batch sequence did not increase");
    }

    std::reverse(versions.begin(), versions.end());
    if (existing != entries_.end()) {
      versions.reserve(versions.size() + existing->second.size());
      versions.insert(versions.end(), existing->second.begin(), existing->second.end());
    }
  }

  for (auto it = staged.begin(); it != staged.end();) {
    const auto existing = entries_.find(it->first);
    if (existing != entries_.end()) {
      existing->second.swap(it->second);
      ++it;
      continue;
    }

    auto node = staged.extract(it++);
    const auto inserted = entries_.insert(std::move(node));
    assert(inserted.inserted);
  }
  bytes_ += added_bytes;
  return Status::Ok();
}

Result<InternalEntry> MemTable::Get(std::string_view key,
                                    std::uint64_t sequence) const {
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return Status::NotFound("key is absent");
  const auto visible = std::find_if(
      it->second.begin(), it->second.end(),
      [sequence](const InternalEntry& entry) { return entry.sequence <= sequence; });
  if (visible == it->second.end())
    return Status::NotFound("key has no visible version");
  return *visible;
}

Result<std::unique_ptr<InternalIterator>>
MemTable::NewIterator(std::string_view begin, std::string_view end) const {
  const BytewiseLess less;
  if (!end.empty() && less(end, begin))
    return Status::InvalidArgument("iterator begin is greater than end");
  return std::unique_ptr<InternalIterator>(new Iterator(*this, begin, end));
}

std::vector<InternalEntry> MemTable::Scan(std::string_view begin,
                                          std::string_view end) const {
  std::vector<InternalEntry> result;
  for (auto it = entries_.lower_bound(begin);
       it != entries_.end() && (end.empty() || BytewiseLess{}(it->first, end)); ++it) {
    result.insert(result.end(), it->second.begin(), it->second.end());
  }
  return result;
}

void MemTable::Clear() {
  entries_.clear();
  bytes_ = 0;
}

} // namespace tinylsm::internal
