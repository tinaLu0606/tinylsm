#include "tinylsm/db.h"

#include <cassert>
#include <chrono>
#include <utility>

#include "db/db_impl.h"
#include "iterator/internal_iterator.h"
#include "util/bytewise_less.h"

namespace tinylsm {
namespace {
using LockClock = std::chrono::steady_clock;

void RecordReadLockWait(const std::shared_ptr<internal::ReadMetricsState>& metrics,
                        LockClock::time_point started) {
  metrics->read_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
  metrics->read_lock_wait_nanoseconds.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() - started)
          .count(),
      std::memory_order_relaxed);
}
} // namespace

Snapshot::Snapshot(std::shared_ptr<internal::SnapshotState> state,
                   std::uint64_t sequence, std::size_t max_active)
    : state_(std::move(state)), sequence_(sequence) {
  registered_ = state_->TryRegister(sequence_, max_active);
}
Snapshot::~Snapshot() {
  if (state_ && registered_)
    state_->Unregister(sequence_);
}
Snapshot::Snapshot(Snapshot&& other) noexcept
    : state_(std::move(other.state_)), sequence_(std::exchange(other.sequence_, 0)),
      registered_(std::exchange(other.registered_, false)) {}
Snapshot& Snapshot::operator=(Snapshot&& other) noexcept {
  if (this != &other) {
    if (state_ && registered_)
      state_->Unregister(sequence_);
    state_ = std::move(other.state_);
    sequence_ = std::exchange(other.sequence_, 0);
    registered_ = std::exchange(other.registered_, false);
  }
  return *this;
}

class Iterator::Impl {
public:
  Impl(std::unique_ptr<internal::InternalIterator> iterator,
       std::vector<std::shared_ptr<internal::SSTableReader>> readers, std::string begin)
      : iterator_(std::move(iterator)), readers_(std::move(readers)),
        begin_(std::move(begin)) {}

  std::unique_ptr<internal::InternalIterator> iterator_;
  // Internal SSTable iterators borrow their reader; retain the table snapshot.
  std::vector<std::shared_ptr<internal::SSTableReader>> readers_;
  std::string begin_;
};

Iterator::Iterator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Iterator::~Iterator() = default;
Iterator::Iterator(Iterator&&) noexcept = default;
Iterator& Iterator::operator=(Iterator&&) noexcept = default;
Status Iterator::Seek(std::string_view target) {
  if (!impl_ || !impl_->iterator_)
    return Status::AlreadyClosed("iterator is empty");
  return impl_->iterator_->Seek(internal::BytewiseLess{}(target, impl_->begin_)
                                    ? std::string_view(impl_->begin_)
                                    : target);
}
bool Iterator::Valid() const noexcept {
  return impl_ && impl_->iterator_ && impl_->iterator_->Valid();
}
std::string_view Iterator::key() const {
  assert(Valid());
  return impl_->iterator_->entry().user_key;
}
std::string_view Iterator::value() const {
  assert(Valid());
  return impl_->iterator_->entry().value;
}
Status Iterator::Next() {
  if (!impl_ || !impl_->iterator_)
    return Status::AlreadyClosed("iterator is empty");
  return impl_->iterator_->Next();
}
const Status& Iterator::status() const noexcept {
  static const Status empty = Status::AlreadyClosed("iterator is empty");
  return impl_ && impl_->iterator_ ? impl_->iterator_->status() : empty;
}

Result<std::shared_ptr<const Snapshot>> DB::Impl::GetSnapshot() const {
  const auto lock_started = LockClock::now();
  std::shared_lock lock(mutex_);
  RecordReadLockWait(read_metrics_, lock_started);
  auto open = CheckOpen();
  if (!open.ok())
    return open;
  auto snapshot = std::shared_ptr<const Snapshot>(
      new Snapshot(snapshot_state_, next_sequence_ - 1, options_.max_active_snapshots));
  if (!snapshot->registered_)
    return Status::ResourceExhausted("active Snapshot limit is reached");
  return snapshot;
}

Result<std::unique_ptr<Iterator>>
DB::Impl::NewIterator(std::string_view begin, std::string_view end,
                      const Snapshot* snapshot) const {
  const auto lock_started = LockClock::now();
  std::shared_lock lock(mutex_);
  RecordReadLockWait(read_metrics_, lock_started);
  auto open = CheckOpen();
  if (!open.ok())
    return open;
  const internal::BytewiseLess less;
  if (!end.empty() && less(end, begin))
    return Status::InvalidArgument("iterator begin is greater than end");
  if (snapshot && snapshot->state_ != snapshot_state_)
    return Status::InvalidArgument("Snapshot belongs to another DB");
  if (manifest_->current().live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  const auto sequence = snapshot ? snapshot->sequence() : next_sequence_ - 1;
  std::vector<std::unique_ptr<internal::InternalIterator>> inputs;
  std::vector<std::shared_ptr<internal::SSTableReader>> readers;
  inputs.reserve(tables_.size() + 2);
  readers.reserve(tables_.size());
  for (std::size_t i = 0; i < tables_.size(); ++i) {
    const auto& meta = manifest_->current().live_tables[i];
    if ((!end.empty() && !less(meta.smallest_key, end)) ||
        less(meta.largest_key, begin))
      continue;
    auto disk = tables_[i]->NewIterator(begin, end);
    if (!disk.ok())
      return disk.status();
    readers.push_back(tables_[i]);
    inputs.push_back(std::move(disk.value()));
    read_metrics_->scan_table_inputs.fetch_add(1, std::memory_order_relaxed);
  }

  auto memory = internal::NewVectorIterator(memtable_.Scan(begin, end), begin, end);
  if (!memory.ok())
    return memory.status();
  inputs.push_back(std::move(memory.value()));
  if (immutable_memtable_) {
    auto immutable =
        internal::NewVectorIterator(immutable_memtable_->Scan(begin, end), begin, end);
    if (!immutable.ok())
      return immutable.status();
    inputs.push_back(std::move(immutable.value()));
  }

  auto merged = internal::NewMergingIterator(std::move(inputs));
  if (!merged.ok())
    return merged.status();
  auto visible = internal::NewVisibilityIterator(std::move(merged.value()), sequence);
  if (!visible.ok())
    return visible.status();
  read_metrics_->range_scans.fetch_add(1, std::memory_order_relaxed);
  return std::unique_ptr<Iterator>(new Iterator(std::make_unique<Iterator::Impl>(
      std::move(visible.value()), std::move(readers), std::string(begin))));
}

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;
DB::DB(DB&&) noexcept = default;
DB& DB::operator=(DB&&) noexcept = default;
Result<std::unique_ptr<DB>> DB::Open(const std::filesystem::path& p, Options o) {
  auto impl = Impl::Open(p, std::move(o));
  if (!impl.ok())
    return impl.status();
  return std::unique_ptr<DB>(new DB(std::move(impl.value())));
}
Status DB::Put(std::string_view k, std::string_view v) { return impl_->Put(k, v); }
Result<std::string> DB::Get(std::string_view k) const { return impl_->Get(k); }
Result<std::string> DB::Get(std::string_view k, const Snapshot* snapshot) const {
  return impl_->Get(k, snapshot);
}
Status DB::Delete(std::string_view k) { return impl_->Delete(k); }
Status DB::Write(const WriteBatch& batch) { return impl_->Write(batch); }
Result<std::vector<Entry>> DB::Scan(std::string_view b, std::string_view e) const {
  return impl_->Scan(b, e);
}
Result<std::shared_ptr<const Snapshot>> DB::GetSnapshot() const {
  return impl_->GetSnapshot();
}
Result<std::unique_ptr<Iterator>> DB::NewIterator(std::string_view begin,
                                                  std::string_view end,
                                                  const Snapshot* snapshot) const {
  return impl_->NewIterator(begin, end, snapshot);
}
ReadMetrics DB::GetReadMetrics() const noexcept {
  return impl_ ? impl_->GetReadMetrics() : ReadMetrics{};
}
WriteMetrics DB::GetWriteMetrics() const noexcept {
  return impl_ ? impl_->GetWriteMetrics() : WriteMetrics{};
}
CompactionMetrics DB::GetCompactionMetrics() const noexcept {
  return impl_ ? impl_->GetCompactionMetrics() : CompactionMetrics{};
}
SnapshotMetrics DB::GetSnapshotMetrics() const noexcept {
  return impl_ ? impl_->GetSnapshotMetrics() : SnapshotMetrics{};
}
Status DB::Compact() { return impl_->Compact(); }
Status DB::Close() { return impl_->Close(); }
} // namespace tinylsm
