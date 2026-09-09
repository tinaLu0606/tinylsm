#include "db/db_impl.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>

#include "db/filename.h"
#include "iterator/internal_iterator.h"
#include "sstable/sstable_builder.h"
#include "util/bytewise_less.h"
#include "wal/wal_reader.h"

namespace tinylsm {
namespace {
internal::DecodeLimits Limits(const Options& o) {
  return {o.max_key_bytes, o.max_value_bytes};
}
bool Matches(const internal::TableMeta& meta,
             const internal::SSTableProperties& properties) {
  return meta.file_size == properties.file_size &&
         meta.smallest_key == properties.smallest_key &&
         meta.largest_key == properties.largest_key &&
         meta.min_sequence == properties.min_sequence &&
         meta.max_sequence == properties.max_sequence;
}
using LockClock = std::chrono::steady_clock;

void RecordReadLockWait(const std::shared_ptr<internal::ReadMetricsState>& metrics,
                        LockClock::time_point started) {
  metrics->read_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
  metrics->read_lock_wait_nanoseconds.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() - started)
          .count(),
      std::memory_order_relaxed);
}

void RecordWriteLockWait(const std::shared_ptr<internal::ReadMetricsState>& metrics,
                         LockClock::time_point started) {
  metrics->write_lock_acquisitions.fetch_add(1, std::memory_order_relaxed);
  metrics->write_lock_wait_nanoseconds.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() - started)
          .count(),
      std::memory_order_relaxed);
}

template <typename T> T LoadMetric(const std::atomic<T>& value) noexcept {
  return value.load(std::memory_order_relaxed);
}

void UpdateMaximum(std::atomic<std::size_t>& destination, std::size_t value) noexcept {
  auto current = destination.load(std::memory_order_relaxed);
  while (current < value &&
         !destination.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
  }
}
} // namespace

WriteMetrics internal::WriteMetricsState::Snapshot() const noexcept {
  return {LoadMetric(writes),
          LoadMetric(write_batches),
          LoadMetric(logical_write_bytes),
          LoadMetric(wal_syncs),
          LoadMetric(memtable_rotations),
          LoadMetric(background_flushes),
          LoadMetric(background_flush_failures),
          LoadMetric(backpressure_waits),
          LoadMetric(backpressure_wait_nanoseconds),
          LoadMetric(group_commits),
          LoadMetric(grouped_write_requests),
          LoadMetric(writer_queue_wait_nanoseconds),
          LoadMetric(writer_queue_depth),
          LoadMetric(max_writer_queue_depth),
          LoadMetric(background_queue_depth),
          LoadMetric(max_background_queue_depth),
          LoadMetric(immutable_memtable_bytes),
          LoadMetric(max_immutable_memtable_bytes)};
}

CompactionMetrics internal::CompactionMetricsState::Snapshot() const noexcept {
  return {LoadMetric(compactions),
          LoadMetric(background_compactions),
          LoadMetric(compaction_failures),
          LoadMetric(compaction_input_tables),
          LoadMetric(compaction_output_tables),
          LoadMetric(compaction_input_bytes),
          LoadMetric(compaction_output_bytes),
          LoadMetric(flush_output_bytes),
          LoadMetric(table_count),
          LoadMetric(live_sstable_bytes),
          LoadMetric(compaction_debt_tables),
          LoadMetric(compaction_debt_bytes)};
}

Result<std::unique_ptr<DB::Impl>> DB::Impl::Open(const std::filesystem::path& path,
                                                 Options options) {
  return Open(path, std::move(options), internal::NewPosixFileSystem());
}

Result<std::unique_ptr<DB::Impl>>
DB::Impl::Open(const std::filesystem::path& path, Options options,
               std::unique_ptr<internal::FileSystem> fs) {
  if (path.empty())
    return Status::InvalidArgument("database path is empty");
  if (options.memtable_bytes == 0 || options.sstable_block_bytes == 0 ||
      options.sstable_restart_interval == 0 ||
      options.max_pending_write_requests == 0 || options.max_pending_write_bytes == 0 ||
      options.max_group_commit_requests == 0 || options.max_group_commit_bytes == 0)
    return Status::InvalidArgument("size options must be non-zero");
  if (options.compaction_table_trigger == 1)
    return Status::InvalidArgument(
        "compaction table trigger must be zero or at least two");
  if (!fs)
    return Status::InvalidArgument("filesystem is null");

  auto impl = std::unique_ptr<Impl>(new Impl());
  impl->options_ = options;
  impl->path_ = path;
  impl->fs_ = std::move(fs);
  impl->read_metrics_ = std::make_shared<internal::ReadMetricsState>();
  impl->write_metrics_ = std::make_shared<internal::WriteMetricsState>();
  impl->compaction_metrics_ = std::make_shared<internal::CompactionMetricsState>();
  impl->block_cache_ = std::make_shared<internal::BlockCache>(options.block_cache_bytes,
                                                              impl->read_metrics_);
  impl->snapshot_state_ = std::make_shared<internal::SnapshotState>();

  auto s = impl->EnsureDatabaseDirectory();
  if (!s.ok())
    return s;

  auto lock = impl->fs_->LockFile(path / "LOCK");
  if (!lock.ok())
    return lock.status().WithContext("open database");
  impl->db_lock_ = std::move(lock.value());

  const auto manifest_path = path / "MANIFEST";
  auto manifest_exists = impl->fs_->FileExists(manifest_path);
  if (!manifest_exists.ok())
    return manifest_exists.status().WithContext("inspect MANIFEST");

  auto loaded =
      manifest_exists.value() ? impl->LoadManifest() : impl->CreateInitialManifest();
  if (!loaded.ok())
    return loaded.status();
  auto snapshot = std::move(loaded.value());

  impl->manifest_ =
      std::make_unique<internal::ManifestState>(*impl->fs_, path, snapshot);

  s = impl->OpenManifestSSTables(snapshot);
  if (!s.ok())
    return s;
  impl->UpdateCompactionGauges();

  s = impl->RecoverWals(snapshot);
  if (!s.ok())
    return s;

  impl->CleanupObsoleteFiles();

  s = impl->StartBackgroundWorker();
  if (!s.ok())
    return s;

  return impl;
}

Status DB::Impl::EnsureDatabaseDirectory() {
  auto path_exists = fs_->FileExists(*path_);
  if (!path_exists.ok())
    return path_exists.status().WithContext("open database");
  if (path_exists.value())
    return Status::Ok();

  if (!options_.create_if_missing)
    return Status::NotFound("database directory does not exist");

  auto s = fs_->CreateDir(*path_);
  return s.ok() ? s : s.WithContext("create database directory");
}

Result<internal::ManifestSnapshot> DB::Impl::LoadManifest() {
  auto loaded = internal::ManifestState::Load(*fs_, *path_);
  if (!loaded.ok())
    return loaded.status().WithContext("load MANIFEST");
  return std::move(loaded.value());
}

Result<bool> DB::Impl::InspectInitialFiles() {
  auto listed = fs_->ListDir(*path_);
  if (!listed.ok())
    return listed.status().WithContext("inspect database directory");

  bool has_initial_wal = false;
  bool has_manifest_temp = false;
  for (const auto& path : listed.value()) {
    const auto name = path.filename().string();
    if (name == "MANIFEST.tmp") {
      has_manifest_temp = true;
      continue;
    }
    const auto numbered = internal::ParseNumberedFileName(name);
    if (numbered && numbered->type == internal::NumberedFileType::kWal) {
      if (numbered->number != 1)
        return Status::Corruption(
            "database without MANIFEST contains an unexpected WAL");
      has_initial_wal = true;
      continue;
    }
    if (numbered && (numbered->type == internal::NumberedFileType::kSstable ||
                     numbered->type == internal::NumberedFileType::kSstableTemp)) {
      return Status::Corruption("database without MANIFEST contains an SSTable");
    }
  }

  if (!has_initial_wal) {
    if (has_manifest_temp)
      return Status::Corruption(
          "database without MANIFEST has an incomplete initialization state");
    return false;
  }

  auto wal = fs_->OpenRandomAccess(*path_ / internal::WalFileName(1));
  if (!wal.ok())
    return wal.status().WithContext("inspect pre-Manifest WAL");
  auto size = wal.value()->Size();
  if (!size.ok())
    return size.status().WithContext("inspect pre-Manifest WAL size");
  if (size.value() != 0) {
    return Status::NotSupported(
        "non-empty pre-Manifest WAL migration is not supported");
  }
  return true;
}

Result<internal::ManifestSnapshot> DB::Impl::CreateInitialManifest() {
  if (!options_.create_if_missing)
    return Status::NotFound("database manifest does not exist");

  auto inspected = InspectInitialFiles();
  if (!inspected.ok())
    return inspected.status();

  internal::ManifestSnapshot snapshot;
  snapshot.active_wal_number = 1;
  snapshot.next_file_number = 2;

  auto initial =
      fs_->OpenWritable(*path_ / internal::WalFileName(1), inspected.value());
  if (!initial.ok())
    return initial.status().WithContext("create initial WAL");

  auto s = initial.value()->Sync();
  if (!s.ok())
    return s.WithContext("sync initial WAL");

  s = initial.value()->Close();
  if (!s.ok())
    return s.WithContext("close initial WAL");

  internal::ManifestState state(*fs_, *path_, snapshot);
  auto published = state.Publish(snapshot);
  if (!published.durable()) {
    const auto context =
        published.state() == internal::ManifestPublishState::kVisibleNotDurable
            ? "publish initial MANIFEST: replacement may be visible; retry DB::Open"
            : "publish initial MANIFEST";
    return published.status().WithContext(context);
  }

  return snapshot;
}

Status DB::Impl::OpenManifestSSTables(const internal::ManifestSnapshot& snapshot) {
  std::vector<std::shared_ptr<internal::SSTableReader>> opened;
  opened.reserve(snapshot.live_tables.size());

  for (const auto& meta : snapshot.live_tables) {
    const auto sst_path = *path_ / internal::SstableFileName(meta.file_number);
    auto table_exists = fs_->FileExists(sst_path);
    if (!table_exists.ok())
      return table_exists.status().WithContext("inspect Manifest SSTable");
    if (!table_exists.value())
      return Status::Corruption("manifest references a missing SSTable");

    auto file = fs_->OpenRandomAccess(sst_path);
    if (!file.ok())
      return file.status().WithContext("open Manifest SSTable");
    auto reader = internal::SSTableReader::Open(
        std::move(file.value()), meta.file_number, block_cache_, read_metrics_);
    if (!reader.ok())
      return reader.status().WithContext("open Manifest SSTable reader");
    if (reader.value()->file_size() != meta.file_size)
      return Status::Corruption("manifest SSTable size does not match file");

    auto properties = reader.value()->ValidateAndGetProperties();
    if (!properties.ok())
      return properties.status().WithContext("validate Manifest SSTable data");
    if (!Matches(meta, properties.value()))
      return Status::Corruption("manifest SSTable metadata does not match file");
    opened.push_back(
        std::shared_ptr<internal::SSTableReader>(std::move(reader.value())));
  }

  tables_.swap(opened);
  return Status::Ok();
}

Status DB::Impl::RecoverWal(std::uint64_t wal_number, std::uint64_t floor,
                            internal::MemTable& target, std::uint64_t* recovered_max) {
  const auto wal_path = *path_ / internal::WalFileName(wal_number);
  auto wal_exists = fs_->FileExists(wal_path);
  if (!wal_exists.ok())
    return wal_exists.status().WithContext("inspect WAL");
  if (!wal_exists.value())
    return Status::Corruption("manifest references a missing WAL");

  auto seq = fs_->OpenSequential(wal_path);
  if (!seq.ok())
    return seq.status().WithContext("open WAL for replay");

  internal::WalReader reader(std::move(seq.value()), Limits(options_));
  auto replay =
      reader.Replay(floor, [&](std::span<const internal::InternalEntry> entries) {
        return target.ApplyBatch(entries);
      });
  if (!replay.ok())
    return replay.status().WithContext("replay WAL");

  if (replay.value().truncated_tail) {
    auto s = fs_->Truncate(wal_path, replay.value().valid_bytes);
    if (!s.ok())
      return s.WithContext("truncate incomplete WAL tail");
  }

  *recovered_max = std::max(floor, replay.value().max_sequence);
  return Status::Ok();
}

Status DB::Impl::RecoverWals(const internal::ManifestSnapshot& snapshot) {
  std::uint64_t recovered_max = snapshot.last_sequence;
  if (snapshot.immutable_wal_number != 0) {
    auto immutable = std::make_unique<internal::MemTable>();
    auto s = RecoverWal(snapshot.immutable_wal_number, recovered_max, *immutable,
                        &recovered_max);
    if (!s.ok())
      return s.WithContext("replay immutable WAL");
    if (immutable->Empty())
      return Status::Corruption("manifest immutable WAL is empty");
    write_metrics_->immutable_memtable_bytes.store(immutable->ApproximateMemoryUsage(),
                                                   std::memory_order_relaxed);
    write_metrics_->background_queue_depth.store(1, std::memory_order_relaxed);
    UpdateMaximum(write_metrics_->max_background_queue_depth, 1);
    UpdateMaximum(write_metrics_->max_immutable_memtable_bytes,
                  immutable->ApproximateMemoryUsage());
    immutable_memtable_ = std::move(immutable);
  }

  auto s =
      RecoverWal(snapshot.active_wal_number, recovered_max, memtable_, &recovered_max);
  if (!s.ok())
    return s.WithContext("replay active WAL");

  const auto max_seq = recovered_max;
  if (max_seq == std::numeric_limits<std::uint64_t>::max())
    return Status::ResourceExhausted("sequence space is exhausted");
  next_sequence_ = max_seq + 1;

  const auto active_wal_path =
      *path_ / internal::WalFileName(snapshot.active_wal_number);
  auto writable = fs_->OpenWritable(active_wal_path, true);
  if (!writable.ok())
    return writable.status().WithContext("open active WAL for append");
  wal_ = std::make_unique<internal::WalWriter>(std::move(writable.value()),
                                               Limits(options_));
  return Status::Ok();
}

Status DB::Impl::CheckOpen() const {
  if (closed_ || closing_)
    return Status::AlreadyClosed("database is closed");
  if (terminal_error_)
    return *terminal_error_;
  return background_error_.value_or(Status::Ok());
}
struct DB::Impl::WriterRequest {
  WriteBatch batch;
  std::size_t queue_bytes = 0;
  Status status;
  bool complete = false;
  std::condition_variable complete_cv;
};

Status DB::Impl::Put(std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return SubmitWrite(std::move(batch));
}
Status DB::Impl::Delete(std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return SubmitWrite(std::move(batch));
}
Status DB::Impl::ValidateWriteBatch(const WriteBatch& batch,
                                    std::size_t& queue_bytes) const {
  queue_bytes = 0;
  if (batch.Empty())
    return Status::Ok();
  if (batch.Count() > internal::kMaxWalBatchOperations)
    return Status::ResourceExhausted("batch exceeds operation limit");

  for (const auto& operation : batch.Operations()) {
    if (operation.key.size() > options_.max_key_bytes ||
        operation.value.size() > options_.max_value_bytes) {
      return Status::InvalidArgument("batch key or value exceeds configured limit");
    }
    const std::size_t entry_bytes =
        sizeof(internal::InternalEntry) + operation.key.size() + operation.value.size();
    if (entry_bytes > std::numeric_limits<std::size_t>::max() - queue_bytes)
      return Status::ResourceExhausted("batch size accounting overflow");
    queue_bytes += entry_bytes;
  }
  if (queue_bytes > internal::kMaxWalBatchBytes)
    return Status::InvalidArgument("batch exceeds encoded size limit");
  return Status::Ok();
}

Status DB::Impl::SubmitWrite(WriteBatch batch) {
  std::size_t queue_bytes = 0;
  auto status = ValidateWriteBatch(batch, queue_bytes);
  if (!status.ok())
    return status;
  if (batch.Empty()) {
    std::shared_lock lock(mutex_);
    return CheckOpen();
  }
  if (queue_bytes > options_.max_pending_write_bytes)
    return Status::ResourceExhausted("batch exceeds bounded writer queue bytes");

  const auto queued_at = LockClock::now();
  auto request = std::make_shared<WriterRequest>();
  request->batch = std::move(batch);
  request->queue_bytes = queue_bytes;

  std::unique_lock queue_lock(writer_mutex_);
  while (!writer_stopping_ &&
         (writer_queue_.size() >= options_.max_pending_write_requests ||
          queue_bytes > options_.max_pending_write_bytes - writer_queue_bytes_)) {
    writer_cv_.wait(queue_lock);
  }
  if (writer_stopping_)
    return Status::AlreadyClosed("database is closing");

  writer_queue_.push_back(request);
  writer_queue_bytes_ += queue_bytes;
  write_metrics_->writer_queue_depth.store(writer_queue_.size(),
                                           std::memory_order_relaxed);
  UpdateMaximum(write_metrics_->max_writer_queue_depth, writer_queue_.size());
  if (!writer_leader_) {
    writer_leader_ = true;
    queue_lock.unlock();
    DrainWriterQueue();
    write_metrics_->writer_queue_wait_nanoseconds.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() -
                                                             queued_at)
            .count(),
        std::memory_order_relaxed);
    return request->status;
  }

  request->complete_cv.wait(queue_lock, [&] { return request->complete; });
  const auto result = request->status;
  queue_lock.unlock();
  write_metrics_->writer_queue_wait_nanoseconds.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() - queued_at)
          .count(),
      std::memory_order_relaxed);
  return result;
}

void DB::Impl::DrainWriterQueue() {
  while (true) {
    std::vector<std::shared_ptr<WriterRequest>> group;
    {
      std::unique_lock queue_lock(writer_mutex_);
      if (writer_queue_.empty()) {
        writer_leader_ = false;
        write_metrics_->writer_queue_depth.store(0, std::memory_order_relaxed);
        writer_cv_.notify_all();
        return;
      }

      std::size_t group_bytes = 0;
      std::size_t group_operations = 0;
      while (!writer_queue_.empty()) {
        const auto& candidate = writer_queue_.front();
        const bool limit_reached =
            !group.empty() &&
            (group.size() >= options_.max_group_commit_requests ||
             group_bytes >= options_.max_group_commit_bytes ||
             candidate->queue_bytes > options_.max_group_commit_bytes - group_bytes ||
             candidate->batch.Count() >
                 internal::kMaxWalBatchOperations - group_operations);
        if (limit_reached)
          break;
        group_bytes += candidate->queue_bytes;
        group_operations += candidate->batch.Count();
        group.push_back(candidate);
        writer_queue_bytes_ -= candidate->queue_bytes;
        writer_queue_.pop_front();
      }
      write_metrics_->writer_queue_depth.store(writer_queue_.size(),
                                               std::memory_order_relaxed);
      writer_cv_.notify_all();
    }

    Status status;
    try {
      status = ApplyWriteGroup(group);
    } catch (...) {
      const auto exception = std::current_exception();
      const Status failure = Status::IOError("group commit aborted by exception");

      std::unique_lock queue_lock(writer_mutex_);
      const auto complete = [&](const std::shared_ptr<WriterRequest>& request) {
        request->status = failure;
        request->complete = true;
        request->complete_cv.notify_one();
      };
      for (const auto& request : group)
        complete(request);
      while (!writer_queue_.empty()) {
        complete(writer_queue_.front());
        writer_queue_.pop_front();
      }
      writer_queue_bytes_ = 0;
      writer_leader_ = false;
      write_metrics_->writer_queue_depth.store(0, std::memory_order_relaxed);
      writer_cv_.notify_all();
      queue_lock.unlock();
      std::rethrow_exception(exception);
    }

    std::unique_lock queue_lock(writer_mutex_);
    for (const auto& request : group) {
      request->status = status;
      request->complete = true;
      request->complete_cv.notify_one();
    }
  }
}

Status
DB::Impl::ApplyWriteGroup(const std::vector<std::shared_ptr<WriterRequest>>& group) {
  if (group.empty())
    return Status::Ok();
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  auto prepared = PrepareForWrite(lock);
  if (!prepared.ok())
    return prepared;

  std::vector<internal::InternalEntry> entries;
  std::size_t operation_count = 0;
  std::uint64_t logical_bytes = 0;
  for (const auto& request : group) {
    if (request->batch.Count() >
        std::numeric_limits<std::size_t>::max() - operation_count)
      return Status::ResourceExhausted("group operation count is exhausted");
    operation_count += request->batch.Count();
    for (const auto& operation : request->batch.Operations()) {
      const auto bytes = static_cast<std::uint64_t>(operation.key.size()) +
                         static_cast<std::uint64_t>(operation.value.size());
      if (bytes > std::numeric_limits<std::uint64_t>::max() - logical_bytes)
        return Status::ResourceExhausted("group logical bytes are exhausted");
      logical_bytes += bytes;
    }
  }
  if (operation_count > std::numeric_limits<std::uint64_t>::max() - next_sequence_)
    return Status::ResourceExhausted("group exceeds sequence space");
  entries.reserve(operation_count);
  std::uint64_t sequence = next_sequence_;
  for (const auto& request : group)
    for (const auto& operation : request->batch.Operations()) {
      const auto type = operation.type == WriteBatch::OperationType::kPut
                            ? internal::ValueType::kValue
                            : internal::ValueType::kTombstone;
      entries.push_back({operation.key, sequence++, type, operation.value});
    }

  if (wal_) {
    auto status = wal_->AppendBatch(entries);
    if (!status.ok())
      return status;
    if (options_.sync_on_write) {
      status = wal_->Sync();
      if (!status.ok())
        return status;
      write_metrics_->wal_syncs.fetch_add(1, std::memory_order_relaxed);
    }
  }

  next_sequence_ = sequence;
  auto status = memtable_.ApplyBatch(entries);
  if (!status.ok())
    return status;

  write_metrics_->writes.fetch_add(operation_count, std::memory_order_relaxed);
  write_metrics_->write_batches.fetch_add(group.size(), std::memory_order_relaxed);
  write_metrics_->logical_write_bytes.fetch_add(logical_bytes,
                                                std::memory_order_relaxed);
  write_metrics_->group_commits.fetch_add(1, std::memory_order_relaxed);
  write_metrics_->grouped_write_requests.fetch_add(group.size(),
                                                   std::memory_order_relaxed);
  if (manifest_ && memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes &&
      !immutable_memtable_)
    return RotateMemTable();
  return Status::Ok();
}

Status DB::Impl::Write(const WriteBatch& batch) { return SubmitWrite(batch); }

void DB::Impl::StopWriterQueue() noexcept {
  std::unique_lock lock(writer_mutex_);
  writer_stopping_ = true;
  writer_cv_.wait(lock, [&] { return !writer_leader_; });
}

void DB::Impl::ResumeWriterQueue() noexcept {
  std::scoped_lock lock(writer_mutex_);
  if (!closed_)
    writer_stopping_ = false;
  writer_cv_.notify_all();
}

Status DB::Impl::PrepareForWrite(std::unique_lock<std::shared_mutex>& lock) {
  while (immutable_memtable_ &&
         memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes) {
    const auto started = LockClock::now();
    write_metrics_->backpressure_waits.fetch_add(1, std::memory_order_relaxed);
    background_cv_.wait(lock, [&] {
      return !immutable_memtable_ || terminal_error_.has_value() ||
             background_error_.has_value() || closing_ || closed_;
    });
    write_metrics_->backpressure_wait_nanoseconds.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(LockClock::now() - started)
            .count(),
        std::memory_order_relaxed);
  }

  auto open = CheckOpen();
  if (!open.ok())
    return open;
  if (!immutable_memtable_ &&
      memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes)
    return RotateMemTable();
  return Status::Ok();
}

Status DB::Impl::RotateMemTable() {
  if (immutable_memtable_)
    return Status::Corruption("cannot rotate while immutable MemTable exists");
  if (memtable_.Empty())
    return Status::Ok();

  const auto& current = manifest_->current();
  if (current.live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  const std::uint64_t table_number = current.next_file_number;
  if (table_number >= std::numeric_limits<std::uint64_t>::max() - 1)
    return Status::ResourceExhausted("file number space is exhausted");
  const std::uint64_t wal_number = table_number + 1;

  auto new_wal_file =
      fs_->OpenWritable(*path_ / internal::WalFileName(wal_number), false);
  if (!new_wal_file.ok())
    return new_wal_file.status().WithContext("create replacement WAL");
  auto status = new_wal_file.value()->Sync();
  if (!status.ok())
    return status.WithContext("sync replacement WAL");
  auto new_wal = std::make_unique<internal::WalWriter>(std::move(new_wal_file.value()),
                                                       Limits(options_));
  auto immutable = std::make_unique<internal::MemTable>();

  internal::ManifestSnapshot next = current;
  next.active_wal_number = wal_number;
  next.immutable_wal_number = current.active_wal_number;
  next.next_file_number = wal_number + 1;
  auto published = manifest_->Publish(std::move(next));
  if (!published.durable()) {
    if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
      terminal_error_ = published.status().WithContext(
          "publish WAL rotation MANIFEST: replacement may be visible; close and reopen "
          "the database");
      return *terminal_error_;
    }
    return published.status().WithContext("publish WAL rotation MANIFEST");
  }

  // The durable Manifest now owns both WALs. The old one becomes immutable and
  // is not removable until the worker publishes its SSTable.
  auto old_wal = std::move(wal_);
  wal_ = std::move(new_wal);
  *immutable = std::move(memtable_);
  memtable_.Clear();
  immutable_memtable_ = std::move(immutable);
  const auto immutable_bytes = immutable_memtable_->ApproximateMemoryUsage();
  write_metrics_->immutable_memtable_bytes.store(immutable_bytes,
                                                 std::memory_order_relaxed);
  write_metrics_->background_queue_depth.store(1, std::memory_order_relaxed);
  UpdateMaximum(write_metrics_->max_background_queue_depth, 1);
  UpdateMaximum(write_metrics_->max_immutable_memtable_bytes, immutable_bytes);
  write_metrics_->memtable_rotations.fetch_add(1, std::memory_order_relaxed);
  BestEffortClose(old_wal.get());
  flush_requested_ = true;
  background_cv_.notify_all();
  return Status::Ok();
}

Status DB::Impl::FlushImmutableMemTable() {
  internal::ManifestSnapshot current;
  std::vector<internal::InternalEntry> entries;
  std::uint64_t table_number = 0;
  std::uint64_t immutable_wal_number = 0;
  {
    std::unique_lock lock(mutex_);
    if (!immutable_memtable_)
      return Status::Ok();
    current = manifest_->current();
    immutable_wal_number = current.immutable_wal_number;
    if (immutable_wal_number == 0)
      return Status::Corruption("immutable MemTable has no Manifest WAL");
    if (current.live_tables.size() != tables_.size())
      return Status::Corruption("Manifest and Reader table sets differ");
    if (current.active_wal_number < 2)
      return Status::Corruption("immutable flush has no reserved SSTable number");
    table_number = current.active_wal_number - 1;
    tables_.reserve(tables_.size() + 1);
    CleanupObsoleteFiles();
    entries = immutable_memtable_->Scan({}, {});
  }
  if (entries.empty())
    return Status::Corruption("immutable MemTable is empty");

  const auto temp = *path_ / internal::SstableTempFileName(table_number);
  const auto final = *path_ / internal::SstableFileName(table_number);
  auto file = fs_->OpenWritable(temp, false);
  if (!file.ok())
    return file.status().WithContext("create temporary immutable SSTable");
  internal::SSTableBuilder builder(std::move(file.value()),
                                   options_.sstable_block_bytes,
                                   options_.sstable_restart_interval);
  for (const auto& entry : entries) {
    auto status = builder.Add(entry);
    if (!status.ok())
      return status.WithContext("build immutable SSTable");
  }
  auto built = builder.Finish();
  if (!built.ok())
    return built.status().WithContext("finish immutable SSTable");

  auto verify_file = fs_->OpenRandomAccess(temp);
  if (!verify_file.ok())
    return verify_file.status().WithContext("open immutable SSTable for validation");
  auto verified = internal::SSTableReader::Open(
      std::move(verify_file.value()), table_number, block_cache_, read_metrics_);
  if (!verified.ok())
    return verified.status().WithContext("validate immutable SSTable");
  auto properties = verified.value()->ValidateAndGetProperties();
  if (!properties.ok())
    return properties.status().WithContext("validate immutable SSTable data");
  auto status = fs_->Rename(temp, final);
  if (!status.ok())
    return status.WithContext("publish immutable SSTable filename");
  status = fs_->SyncDir(*path_);
  if (!status.ok())
    return status.WithContext("sync database directory after immutable SSTable rename");

  std::unique_lock lock(mutex_);
  if (!immutable_memtable_ ||
      manifest_->current().immutable_wal_number != immutable_wal_number ||
      manifest_->current().active_wal_number != table_number + 1) {
    return Status::Corruption("immutable flush state changed unexpectedly");
  }
  internal::ManifestSnapshot next = manifest_->current();
  next.immutable_wal_number = 0;
  next.last_sequence = built.value().max_sequence;
  next.live_tables.push_back({table_number, built.value().file_size,
                              built.value().smallest_key, built.value().largest_key,
                              built.value().min_sequence, built.value().max_sequence});
  if (!Matches(next.live_tables.back(), properties.value()))
    return Status::Corruption("built immutable SSTable metadata does not match file");

  auto published = manifest_->Publish(std::move(next));
  if (!published.durable()) {
    if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
      terminal_error_ =
          published.status().WithContext("publish immutable flush MANIFEST: "
                                         "replacement may be visible; close and reopen "
                                         "the database");
      return *terminal_error_;
    }
    return published.status().WithContext("publish immutable flush MANIFEST");
  }

  tables_.push_back(
      std::shared_ptr<internal::SSTableReader>(std::move(verified.value())));
  immutable_memtable_.reset();
  compaction_metrics_->flush_output_bytes.fetch_add(built.value().file_size,
                                                    std::memory_order_relaxed);
  UpdateCompactionGauges();
  if (NeedsBackgroundCompaction())
    compaction_requested_ = true;
  write_metrics_->background_queue_depth.store(0, std::memory_order_relaxed);
  write_metrics_->immutable_memtable_bytes.store(0, std::memory_order_relaxed);
  lock.unlock();
  if (BestEffortRemove(*path_ / internal::WalFileName(immutable_wal_number)))
    BestEffortSyncDir();
  return Status::Ok();
}

void DB::Impl::BackgroundWorkLoop() noexcept {
  std::unique_lock lock(mutex_);
  while (true) {
    background_cv_.wait(lock, [&] {
      return worker_stopping_ || flush_requested_ || compaction_requested_;
    });
    if (worker_stopping_)
      break;
    const bool flush = flush_requested_ && immutable_memtable_;
    const bool compact = !flush && compaction_requested_ && !closing_;
    flush_requested_ = false;
    compaction_requested_ = false;
    if (!flush && !compact)
      continue;
    if (flush)
      background_flush_running_ = true;
    else
      background_compaction_running_ = true;
    lock.unlock();

    Status status;
    try {
      status = flush ? FlushImmutableMemTable()
                     : CompactTablePrefix(options_.compaction_table_trigger, true);
    } catch (const std::exception& error) {
      status =
          Status::IOError(std::string("background worker exception: ") + error.what());
    } catch (...) {
      status = Status::IOError("background worker raised an unknown exception");
    }

    lock.lock();
    background_flush_running_ = false;
    background_compaction_running_ = false;
    if (!status.ok()) {
      if (!terminal_error_)
        background_error_ = status.WithContext(flush ? "background immutable flush"
                                                     : "background compaction");
      if (flush)
        write_metrics_->background_flush_failures.fetch_add(1,
                                                            std::memory_order_relaxed);
      else
        compaction_metrics_->compaction_failures.fetch_add(1,
                                                           std::memory_order_relaxed);
    } else {
      if (flush)
        write_metrics_->background_flushes.fetch_add(1, std::memory_order_relaxed);
      else if (NeedsBackgroundCompaction())
        compaction_requested_ = true;
    }
    background_cv_.notify_all();
  }
  background_cv_.notify_all();
}

Status DB::Impl::StartBackgroundWorker() {
  bool notify_worker = false;
  {
    std::unique_lock lock(mutex_);
    if (immutable_memtable_) {
      flush_requested_ = true;
      notify_worker = true;
    }
    if (NeedsBackgroundCompaction()) {
      compaction_requested_ = true;
      notify_worker = true;
    }
  }

  try {
    background_worker_ = std::thread(&Impl::BackgroundWorkLoop, this);
  } catch (const std::exception& error) {
    return Status::ResourceExhausted(std::string("start background flush worker: ") +
                                     error.what());
  }
  if (notify_worker)
    background_cv_.notify_all();
  return Status::Ok();
}

void DB::Impl::StopBackgroundWorker() noexcept {
  {
    std::unique_lock lock(mutex_);
    worker_stopping_ = true;
    background_cv_.notify_all();
  }
  if (background_worker_.joinable())
    background_worker_.join();
}

void DB::Impl::WaitForBackgroundFlush(std::unique_lock<std::shared_mutex>& lock) {
  background_cv_.wait(lock, [&] {
    return (!immutable_memtable_ && !background_flush_running_) ||
           terminal_error_.has_value() || background_error_.has_value() ||
           worker_stopping_;
  });
}

void DB::Impl::WaitForBackgroundWork(std::unique_lock<std::shared_mutex>& lock) {
  background_cv_.wait(lock, [&] {
    return (!immutable_memtable_ && !background_flush_running_ &&
            !background_compaction_running_ && !flush_requested_ &&
            !compaction_requested_) ||
           terminal_error_.has_value() || background_error_.has_value() ||
           worker_stopping_;
  });
}

Result<std::string> DB::Impl::Get(std::string_view key) const {
  return Get(key, nullptr);
}

Result<std::string> DB::Impl::Get(std::string_view key,
                                  const Snapshot* snapshot) const {
  const auto lock_started = LockClock::now();
  std::shared_lock lock(mutex_);
  RecordReadLockWait(read_metrics_, lock_started);
  auto s = CheckOpen();
  if (!s.ok())
    return s;
  if (snapshot && snapshot->state_ != snapshot_state_)
    return Status::InvalidArgument("Snapshot belongs to another DB");
  read_metrics_->point_lookups.fetch_add(1, std::memory_order_relaxed);
  const auto sequence = snapshot ? snapshot->sequence() : next_sequence_ - 1;
  std::optional<internal::InternalEntry> newest;
  const auto consider = [&newest](Result<internal::InternalEntry> candidate) -> Status {
    if (!candidate.ok()) {
      if (candidate.status().code() == StatusCode::kNotFound)
        return Status::Ok();
      return candidate.status();
    }
    if (!newest || candidate.value().sequence > newest->sequence)
      newest = std::move(candidate.value());
    return Status::Ok();
  };

  s = consider(memtable_.Get(key, sequence));
  if (!s.ok())
    return s;

  if (immutable_memtable_) {
    s = consider(immutable_memtable_->Get(key, sequence));
    if (!s.ok())
      return s;
  }

  if (manifest_ && manifest_->current().live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  const internal::BytewiseLess less;
  for (std::size_t i = 0; i < tables_.size(); ++i) {
    const auto& meta = manifest_->current().live_tables[i];
    if (less(key, meta.smallest_key) || less(meta.largest_key, key))
      continue;
    s = consider(tables_[i]->Get(key, sequence));
    if (!s.ok())
      return s;
  }

  if (!newest || newest->type == internal::ValueType::kTombstone)
    return Status::NotFound("key is absent or deleted");
  return newest->value;
}
Result<std::vector<Entry>> DB::Impl::Scan(std::string_view begin,
                                          std::string_view end) const {
  auto iterator = NewIterator(begin, end, nullptr);
  if (!iterator.ok())
    return iterator.status();
  std::vector<Entry> out;
  while (iterator.value()->Valid()) {
    out.push_back(
        {std::string(iterator.value()->key()), std::string(iterator.value()->value())});
    auto next = iterator.value()->Next();
    if (!next.ok())
      return next;
  }
  if (!iterator.value()->status().ok())
    return iterator.value()->status();
  return out;
}

ReadMetrics DB::Impl::GetReadMetrics() const noexcept {
  return read_metrics_ ? read_metrics_->Snapshot() : ReadMetrics{};
}

WriteMetrics DB::Impl::GetWriteMetrics() const noexcept {
  return write_metrics_ ? write_metrics_->Snapshot() : WriteMetrics{};
}

CompactionMetrics DB::Impl::GetCompactionMetrics() const noexcept {
  return compaction_metrics_ ? compaction_metrics_->Snapshot() : CompactionMetrics{};
}

SnapshotMetrics DB::Impl::GetSnapshotMetrics() const noexcept {
  if (!snapshot_state_)
    return {};
  const auto active = snapshot_state_->ActiveCount();
  return {active, active == 0 ? 0 : snapshot_state_->OldestOr(0),
          snapshot_state_->last_full_compaction_retained_versions(),
          snapshot_state_->last_full_compaction_retained_bytes()};
}

Status DB::Impl::Compact() {
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  auto open = CheckOpen();
  if (!open.ok())
    return open;

  WaitForBackgroundWork(lock);
  open = CheckOpen();
  if (!open.ok())
    return open;
  const auto input_count = tables_.size();
  lock.unlock();
  return CompactTablePrefix(input_count, false);
}

bool DB::Impl::NeedsBackgroundCompaction() const noexcept {
  return options_.compaction_table_trigger != 0 &&
         tables_.size() >= options_.compaction_table_trigger;
}

void DB::Impl::UpdateCompactionGauges() noexcept {
  std::size_t live_bytes = 0;
  for (const auto& table : manifest_->current().live_tables) {
    if (table.file_size > std::numeric_limits<std::size_t>::max() - live_bytes) {
      live_bytes = std::numeric_limits<std::size_t>::max();
      break;
    }
    live_bytes += static_cast<std::size_t>(table.file_size);
  }
  const auto table_count = tables_.size();
  std::size_t debt_tables = 0;
  std::size_t debt_bytes = 0;
  if (NeedsBackgroundCompaction()) {
    debt_tables = table_count - options_.compaction_table_trigger + 1;
    for (std::size_t i = 0; i < options_.compaction_table_trigger; ++i) {
      const auto bytes = manifest_->current().live_tables[i].file_size;
      if (bytes > std::numeric_limits<std::size_t>::max() - debt_bytes) {
        debt_bytes = std::numeric_limits<std::size_t>::max();
        break;
      }
      debt_bytes += static_cast<std::size_t>(bytes);
    }
  }
  compaction_metrics_->table_count.store(table_count, std::memory_order_relaxed);
  compaction_metrics_->live_sstable_bytes.store(live_bytes, std::memory_order_relaxed);
  compaction_metrics_->compaction_debt_tables.store(debt_tables,
                                                    std::memory_order_relaxed);
  compaction_metrics_->compaction_debt_bytes.store(debt_bytes,
                                                   std::memory_order_relaxed);
}

Status DB::Impl::CompactTablePrefix(std::size_t input_count, bool background) {
  internal::ManifestSnapshot selected_snapshot;
  std::vector<internal::TableMeta> selected_meta;
  std::vector<std::shared_ptr<internal::SSTableReader>> selected_tables;
  std::uint64_t replacement_number = 0;
  bool compaction_covers_all_tables = false;

  std::unique_lock<std::shared_mutex> version_lock(mutex_);
  {
    auto open = CheckOpen();
    if (!open.ok())
      return open;
    if (input_count == 0 || tables_.size() < input_count)
      return Status::Ok();
    if (manifest_->current().live_tables.size() != tables_.size())
      return Status::Corruption("Manifest and Reader table sets differ");
    if (background && input_count != options_.compaction_table_trigger)
      return Status::Corruption("background compaction input count is invalid");
    if (!background)
      CleanupObsoleteFiles();

    selected_snapshot = manifest_->current();
    replacement_number = selected_snapshot.next_file_number;
    if (replacement_number == std::numeric_limits<std::uint64_t>::max())
      return Status::ResourceExhausted("file number space is exhausted");
    selected_meta.assign(selected_snapshot.live_tables.begin(),
                         selected_snapshot.live_tables.begin() + input_count);
    selected_tables.assign(tables_.begin(), tables_.begin() + input_count);
    compaction_covers_all_tables = input_count == selected_snapshot.live_tables.size();

    if (background) {
      // Reserve the output number before releasing the version lock. A flush
      // may rotate its WAL while this job reads; it must receive a different
      // number. Explicit Compact deliberately holds the lock instead, retaining
      // its original one-Manifest publication and fault boundary.
      auto reserved = selected_snapshot;
      ++reserved.next_file_number;
      auto published = manifest_->Publish(std::move(reserved));
      if (!published.durable()) {
        if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
          terminal_error_ = published.status().WithContext(
              "reserve compaction file number: close and reopen the database");
          return *terminal_error_;
        }
        return published.status().WithContext("reserve compaction file number");
      }
      version_lock.unlock();
    }
  }

  std::vector<std::unique_ptr<internal::InternalIterator>> inputs;
  inputs.reserve(selected_tables.size());
  std::uint64_t input_bytes = 0;
  for (std::size_t i = 0; i < selected_tables.size(); ++i) {
    input_bytes += selected_meta[i].file_size;
    auto iterator = selected_tables[i]->NewIterator({}, {});
    if (!iterator.ok())
      return iterator.status().WithContext("create compaction input iterator");
    inputs.push_back(std::move(iterator.value()));
  }
  auto merged = internal::NewMergingIterator(std::move(inputs));
  if (!merged.ok())
    return merged.status().WithContext("create compaction merge iterator");

  const auto temp = *path_ / internal::SstableTempFileName(replacement_number);
  const auto final = *path_ / internal::SstableFileName(replacement_number);
  std::unique_ptr<internal::SSTableBuilder> builder;
  const auto no_snapshot = std::numeric_limits<std::uint64_t>::max();
  const auto watermark = snapshot_state_->OldestOr(no_snapshot);
  std::string previous_key;
  bool first_for_key = true;
  bool retained_watermark_version = false;
  std::uint64_t retained_versions = 0;
  std::uint64_t retained_bytes = 0;
  while (merged.value()->Valid()) {
    const auto& entry = merged.value()->entry();
    if (first_for_key || entry.user_key != previous_key) {
      previous_key = entry.user_key;
      first_for_key = false;
      retained_watermark_version = false;
    }

    bool retain = !compaction_covers_all_tables;
    if (compaction_covers_all_tables) {
      if (watermark == no_snapshot) {
        // Without a historical reader, only the latest live value can affect
        // future reads; a latest tombstone and every older version are dead.
        retain =
            !retained_watermark_version && entry.type == internal::ValueType::kValue;
        retained_watermark_version = true;
      } else if (entry.sequence > watermark) {
        // Any active snapshot is at or after the watermark, so it may observe
        // one of these newer versions.
        retain = true;
      } else if (!retained_watermark_version) {
        // Keep the one version visible to the oldest active snapshot.
        retain = true;
        retained_watermark_version = true;
      }
    }
    if (retain) {
      if (compaction_covers_all_tables) {
        ++retained_versions;
        const auto entry_bytes = static_cast<std::uint64_t>(entry.user_key.size()) +
                                 static_cast<std::uint64_t>(entry.value.size());
        retained_bytes =
            entry_bytes > std::numeric_limits<std::uint64_t>::max() - retained_bytes
                ? std::numeric_limits<std::uint64_t>::max()
                : retained_bytes + entry_bytes;
      }
      if (!builder) {
        auto file = fs_->OpenWritable(temp, false);
        if (!file.ok())
          return file.status().WithContext("create temporary compacted SSTable");
        builder = std::make_unique<internal::SSTableBuilder>(
            std::move(file.value()), options_.sstable_block_bytes,
            options_.sstable_restart_interval);
      }
      auto status = builder->Add(entry);
      if (!status.ok())
        return status.WithContext("build temporary compacted SSTable");
    }
    auto next = merged.value()->Next();
    if (!next.ok())
      return next.WithContext("read compaction input");
  }
  if (!merged.value()->status().ok())
    return merged.value()->status().WithContext("read compaction input");

  std::optional<internal::TableMeta> replacement;
  std::shared_ptr<internal::SSTableReader> replacement_reader;
  if (builder) {
    auto built = builder->Finish();
    if (!built.ok())
      return built.status().WithContext("finish temporary compacted SSTable");
    builder.reset();
    auto verify_file = fs_->OpenRandomAccess(temp);
    if (!verify_file.ok())
      return verify_file.status().WithContext("open temporary compacted SSTable");
    auto verified =
        internal::SSTableReader::Open(std::move(verify_file.value()),
                                      replacement_number, block_cache_, read_metrics_);
    if (!verified.ok())
      return verified.status().WithContext("validate temporary compacted SSTable");
    auto properties = verified.value()->ValidateAndGetProperties();
    if (!properties.ok())
      return properties.status().WithContext(
          "validate temporary compacted SSTable data");
    replacement = {replacement_number,         built.value().file_size,
                   built.value().smallest_key, built.value().largest_key,
                   built.value().min_sequence, built.value().max_sequence};
    if (!Matches(*replacement, properties.value()))
      return Status::Corruption("compacted SSTable metadata does not match file");
    auto status = fs_->Rename(temp, final);
    if (!status.ok())
      return status.WithContext("publish compacted SSTable filename");
    status = fs_->SyncDir(*path_);
    if (!status.ok())
      return status.WithContext("sync database directory after compaction rename");
    replacement_reader =
        std::shared_ptr<internal::SSTableReader>(std::move(verified.value()));
  }

  std::vector<std::filesystem::path> obsolete_paths;
  std::vector<std::uint64_t> obsolete_numbers;
  {
    if (!version_lock.owns_lock())
      version_lock.lock();
    if (manifest_->current().live_tables.size() != tables_.size() ||
        tables_.size() < selected_meta.size())
      return Status::Corruption("compaction version changed unexpectedly");
    for (std::size_t i = 0; i < selected_meta.size(); ++i) {
      if (manifest_->current().live_tables[i].file_number !=
          selected_meta[i].file_number)
        return Status::Corruption(
            "compaction input is no longer the oldest table prefix");
    }

    auto next = manifest_->current();
    next.live_tables.erase(next.live_tables.begin(),
                           next.live_tables.begin() + selected_meta.size());
    if (replacement) {
      next.live_tables.insert(next.live_tables.begin(), *replacement);
      if (next.next_file_number == replacement_number)
        ++next.next_file_number;
    } else if (next.next_file_number == replacement_number + 1)
      next.next_file_number = replacement_number;

    auto published = manifest_->Publish(std::move(next));
    if (!published.durable()) {
      if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
        terminal_error_ = published.status().WithContext(
            "publish compaction MANIFEST: replacement may be visible; close and reopen "
            "the database");
        return *terminal_error_;
      }
      return published.status().WithContext("publish compaction MANIFEST");
    }

    obsolete_paths.reserve(selected_meta.size());
    obsolete_numbers.reserve(selected_meta.size());
    for (const auto& meta : selected_meta) {
      obsolete_paths.push_back(*path_ / internal::SstableFileName(meta.file_number));
      obsolete_numbers.push_back(meta.file_number);
    }
    tables_.erase(tables_.begin(), tables_.begin() + selected_meta.size());
    if (replacement_reader)
      tables_.insert(tables_.begin(), std::move(replacement_reader));
    if (compaction_covers_all_tables)
      snapshot_state_->SetLastFullCompactionRetention(retained_versions,
                                                      retained_bytes);
    compaction_metrics_->compactions.fetch_add(1, std::memory_order_relaxed);
    if (background)
      compaction_metrics_->background_compactions.fetch_add(1,
                                                            std::memory_order_relaxed);
    compaction_metrics_->compaction_input_tables.fetch_add(selected_meta.size(),
                                                           std::memory_order_relaxed);
    compaction_metrics_->compaction_output_tables.fetch_add(replacement ? 1 : 0,
                                                            std::memory_order_relaxed);
    compaction_metrics_->compaction_input_bytes.fetch_add(input_bytes,
                                                          std::memory_order_relaxed);
    compaction_metrics_->compaction_output_bytes.fetch_add(
        replacement ? replacement->file_size : 0, std::memory_order_relaxed);
    UpdateCompactionGauges();
  }
  for (const auto number : obsolete_numbers)
    block_cache_->EraseTable(number);
  bool removed_any = false;
  for (const auto& path : obsolete_paths)
    removed_any = BestEffortRemove(path) || removed_any;
  if (removed_any)
    BestEffortSyncDir();
  return Status::Ok();
}

bool DB::Impl::BestEffortRemove(const std::filesystem::path& path) noexcept {
  try {
    auto status = fs_->Remove(path);
    if (status.ok())
      return true;
  } catch (...) {
    RememberCleanup(path);
    return false;
  }

  RememberCleanup(path);
  return false;
}

void DB::Impl::BestEffortClose(internal::WalWriter* wal) noexcept {
  if (!wal)
    return;
  try {
    wal->Close().IgnoreError();
  } catch (...) {
    return;
  }
}

void DB::Impl::RememberCleanup(const std::filesystem::path& path) noexcept {
  try {
    if (std::find(pending_cleanup_.begin(), pending_cleanup_.end(), path) ==
        pending_cleanup_.end()) {
      pending_cleanup_.push_back(path);
    }
  } catch (...) {
    return;
  }
}

void DB::Impl::BestEffortSyncDir() noexcept {
  try {
    fs_->SyncDir(*path_).IgnoreError();
  } catch (...) {
    return;
  }
}

void DB::Impl::CleanupObsoleteFiles() noexcept {
  try {
    bool removed_any = false;
    auto pending = std::move(pending_cleanup_);
    pending_cleanup_.clear();
    for (const auto& path : pending)
      removed_any = BestEffortRemove(path) || removed_any;

    auto listed = fs_->ListDir(*path_);
    if (!listed.ok()) {
      if (removed_any)
        BestEffortSyncDir();
      return;
    }

    const auto& current = manifest_->current();
    for (const auto& path : listed.value()) {
      const auto name = path.filename().string();
      bool obsolete = name == "MANIFEST.tmp";
      if (const auto numbered = internal::ParseNumberedFileName(name)) {
        switch (numbered->type) {
        case internal::NumberedFileType::kWal:
          obsolete = numbered->number != current.active_wal_number &&
                     numbered->number != current.immutable_wal_number;
          break;
        case internal::NumberedFileType::kSstable:
          obsolete =
              std::none_of(current.live_tables.begin(), current.live_tables.end(),
                           [&](const internal::TableMeta& table) {
                             return table.file_number == numbered->number;
                           });
          break;
        case internal::NumberedFileType::kSstableTemp:
          obsolete = true;
          break;
        }
      }

      if (obsolete)
        removed_any = BestEffortRemove(path) || removed_any;
    }

    if (removed_any)
      BestEffortSyncDir();
  } catch (...) {
    // Cleanup cannot invalidate successfully recovered or committed state.
    return;
  }
}

Status DB::Impl::Close() {
  StopWriterQueue();
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  if (closed_)
    return Status::AlreadyClosed("database is closed");

  closing_ = true;
  compaction_requested_ = false;
  while (!background_error_ && !terminal_error_) {
    if (background_compaction_running_) {
      background_cv_.wait(lock, [&] {
        return !background_compaction_running_ || background_error_.has_value() ||
               terminal_error_.has_value();
      });
      continue;
    }
    if (immutable_memtable_) {
      flush_requested_ = true;
      background_cv_.notify_all();
      WaitForBackgroundFlush(lock);
      continue;
    }
    if (memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes) {
      auto rotated = RotateMemTable();
      if (!rotated.ok())
        background_error_ = rotated.WithContext("close database: rotate MemTable");
      continue;
    }
    break;
  }
  if (!wal_) {
    worker_stopping_ = true;
    background_cv_.notify_all();
    lock.unlock();
    if (background_worker_.joinable())
      background_worker_.join();
    lock.lock();
    closed_ = true;
    db_lock_.reset();
    return background_error_.value_or(Status::Ok());
  }

  if (options_.sync_on_write) {
    auto s = wal_->Sync();
    if (!s.ok()) {
      closing_ = false;
      background_cv_.notify_all();
      lock.unlock();
      ResumeWriterQueue();
      return s.WithContext("close database: sync WAL");
    }
  }

  worker_stopping_ = true;
  background_cv_.notify_all();
  lock.unlock();
  if (background_worker_.joinable())
    background_worker_.join();
  lock.lock();

  auto s = wal_->Close();
  closed_ = true;
  db_lock_.reset();
  if (!s.ok())
    return s.WithContext("close database: close WAL");
  return background_error_.value_or(Status::Ok());
}
DB::Impl::~Impl() {
  StopBackgroundWorker();
  if (!closed_ && wal_)
    BestEffortClose(wal_.get());
}
} // namespace tinylsm
