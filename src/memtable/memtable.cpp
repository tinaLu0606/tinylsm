#include "memtable/memtable.h"

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
      : current_(source.entries_.lower_bound(begin)), finish_(source.entries_.end()),
        end_(end) {}

  [[nodiscard]] bool Valid() const noexcept override {
    return current_ != finish_ &&
           (end_.empty() || BytewiseLess{}(current_->first, end_));
  }

  [[nodiscard]] const InternalEntry& entry() const override {
    assert(Valid());
    return current_->second;
  }

  Status Next() override {
    if (Valid())
      ++current_;
    return Status::Ok();
  }

  [[nodiscard]] const Status& status() const noexcept override { return status_; }

private:
  decltype(entries_)::const_iterator current_;
  decltype(entries_)::const_iterator finish_;
  std::string end_;
  Status status_;
};

Status MemTable::Apply(InternalEntry entry) {
  auto it = entries_.find(entry.user_key);
  if (it != entries_.end() && entry.sequence <= it->second.sequence) {
    return Status::Corruption("memtable sequence did not increase");
  }
  if (it != entries_.end()) {
    bytes_ -= EntryBytes(it->second);
    it->second = std::move(entry);
    bytes_ += EntryBytes(it->second);
  } else {
    bytes_ += EntryBytes(entry);
    std::string key = entry.user_key;
    entries_.emplace(std::move(key), std::move(entry));
  }
  return Status::Ok();
}

Status MemTable::ApplyBatch(std::span<const InternalEntry> entries) {
  if (entries.empty())
    return Status::Ok();

  std::uint64_t previous_sequence = 0;
  std::map<std::string, InternalEntry, BytewiseLess> staged;
  for (const auto& entry : entries) {
    if (entry.sequence == 0 ||
        (previous_sequence != 0 && entry.sequence <= previous_sequence)) {
      return Status::Corruption("memtable batch sequence did not increase");
    }
    staged.insert_or_assign(entry.user_key, entry);
    previous_sequence = entry.sequence;
  }

  std::size_t next_bytes = bytes_;
  for (const auto& [key, entry] : staged) {
    const auto existing = entries_.find(key);
    if (existing != entries_.end())
      next_bytes -= EntryBytes(existing->second);
    const auto added = EntryBytes(entry);
    if (added > std::numeric_limits<std::size_t>::max() - next_bytes)
      return Status::ResourceExhausted("memtable batch size overflow");
    next_bytes += added;
    if (existing != entries_.end() && entry.sequence <= existing->second.sequence) {
      return Status::Corruption("memtable batch sequence did not increase");
    }
  }

  while (!staged.empty()) {
    auto node = staged.extract(staged.begin());
    auto existing = entries_.find(node.key());
    if (existing == entries_.end()) {
      entries_.insert(std::move(node));
    } else {
      existing->second = std::move(node.mapped());
    }
  }
  bytes_ = next_bytes;
  return Status::Ok();
}

Result<InternalEntry> MemTable::Get(std::string_view key) const {
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return Status::NotFound("key is absent");
  return it->second;
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
       it != entries_.end() && (end.empty() || it->first < end); ++it) {
    result.push_back(it->second);
  }
  return result;
}

void MemTable::Clear() {
  entries_.clear();
  bytes_ = 0;
}

} // namespace tinylsm::internal
