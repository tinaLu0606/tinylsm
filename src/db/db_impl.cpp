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
          LoadMetric(wal_syncs),
          LoadMetric(memtable_rotations),
          LoadMetric(background_flushes),
          LoadMetric(background_flush_failures),
          LoadMetric(backpressure_waits),
          LoadMetric(backpressure_wait_nanoseconds),
          LoadMetric(background_queue_depth),
          LoadMetric(max_background_queue_depth),
          LoadMetric(immutable_memtable_bytes),
          LoadMetric(max_immutable_memtable_bytes)};
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
  if (options.memtable_bytes == 0 || options.sstable_block_bytes == 0)
    return Status::InvalidArgument("size options must be non-zero");
  if (!fs)
    return Status::InvalidArgument("filesystem is null");

  auto impl = std::unique_ptr<Impl>(new Impl());
  impl->options_ = options;
  impl->path_ = path;
  impl->fs_ = std::move(fs);
  impl->read_metrics_ = std::make_shared<internal::ReadMetricsState>();
  impl->write_metrics_ = std::make_shared<internal::WriteMetricsState>();
  impl->block_cache_ = std::make_shared<internal::BlockCache>(options.block_cache_bytes,
                                                              impl->read_metrics_);

  auto s = impl->EnsureDatabaseDirectory();
  if (!s.ok())
    return s;

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
  std::vector<std::unique_ptr<internal::SSTableReader>> opened;
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
    opened.push_back(std::move(reader.value()));
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
Status DB::Impl::Put(std::string_view k, std::string_view v) {
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  return WriteEntry(k, v, internal::ValueType::kValue, lock);
}
Status DB::Impl::Delete(std::string_view k) {
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  return WriteEntry(k, {}, internal::ValueType::kTombstone, lock);
}
Status DB::Impl::WriteEntry(std::string_view key, std::string_view value,
                            internal::ValueType type,
                            std::unique_lock<std::shared_mutex>& lock) {
  // A full second MemTable waits here until the one
  // immutable generation is durably published or reports its sticky failure.
  // This keeps both memory and the number of recoverable WALs bounded.
  auto prepared = PrepareForWrite(lock);
  if (!prepared.ok())
    return prepared;
  if (key.size() > options_.max_key_bytes || value.size() > options_.max_value_bytes)
    return Status::InvalidArgument("key or value exceeds configured limit");

  std::size_t projected = memtable_.ApproximateMemoryUsage();
  auto existing = memtable_.Get(key);
  if (existing.ok())
    projected -= sizeof(internal::InternalEntry) + existing.value().user_key.size() +
                 existing.value().value.size();
  const std::size_t added = sizeof(internal::InternalEntry) + key.size() + value.size();
  if (added > std::numeric_limits<std::size_t>::max() - projected)
    return Status::ResourceExhausted("memtable size accounting overflow");
  projected += added;

  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
    return Status::ResourceExhausted("sequence space is exhausted");

  internal::InternalEntry entry{std::string(key), next_sequence_++, type,
                                std::string(value)};

  if (wal_) {
    auto s = wal_->Append(entry);
    if (!s.ok())
      return s;
    if (options_.sync_on_write) {
      s = wal_->Sync();
      if (!s.ok())
        return s;
      write_metrics_->wal_syncs.fetch_add(1, std::memory_order_relaxed);
    }
  }

  auto s = memtable_.Apply(std::move(entry));
  if (!s.ok())
    return s;

  write_metrics_->writes.fetch_add(1, std::memory_order_relaxed);
  if (manifest_ && memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes &&
      !immutable_memtable_)
    return RotateMemTable();
  return Status::Ok();
}

Status DB::Impl::Write(const WriteBatch& batch) {
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  auto prepared = PrepareForWrite(lock);
  if (!prepared.ok())
    return prepared;
  if (batch.Empty())
    return Status::Ok();
  if (batch.Count() > internal::kMaxWalBatchOperations ||
      batch.Count() > std::numeric_limits<std::uint64_t>::max() - next_sequence_) {
    return Status::ResourceExhausted("batch exceeds sequence or operation limit");
  }

  std::vector<internal::InternalEntry> entries;
  entries.reserve(batch.Count());
  std::uint64_t sequence = next_sequence_;
  std::size_t added_bytes = 0;
  for (const auto& operation : batch.Operations()) {
    if (operation.key.size() > options_.max_key_bytes ||
        operation.value.size() > options_.max_value_bytes) {
      return Status::InvalidArgument("batch key or value exceeds configured limit");
    }
    const auto bytes =
        sizeof(internal::InternalEntry) + operation.key.size() + operation.value.size();
    if (bytes > std::numeric_limits<std::size_t>::max() - added_bytes)
      return Status::ResourceExhausted("batch size accounting overflow");
    added_bytes += bytes;

    const auto type = operation.type == WriteBatch::OperationType::kPut
                          ? internal::ValueType::kValue
                          : internal::ValueType::kTombstone;
    entries.push_back({operation.key, sequence++, type, operation.value});
  }
  if (added_bytes > internal::kMaxWalBatchBytes)
    return Status::InvalidArgument("batch exceeds encoded size limit");

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

  write_metrics_->writes.fetch_add(batch.Count(), std::memory_order_relaxed);
  write_metrics_->write_batches.fetch_add(1, std::memory_order_relaxed);
  if (manifest_ && memtable_.ApproximateMemoryUsage() >= options_.memtable_bytes &&
      !immutable_memtable_)
    return RotateMemTable();
  return Status::Ok();
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
                                   options_.sstable_block_bytes);
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

  tables_.push_back(std::move(verified.value()));
  immutable_memtable_.reset();
  write_metrics_->background_queue_depth.store(0, std::memory_order_relaxed);
  write_metrics_->immutable_memtable_bytes.store(0, std::memory_order_relaxed);
  lock.unlock();
  if (BestEffortRemove(*path_ / internal::WalFileName(immutable_wal_number)))
    BestEffortSyncDir();
  return Status::Ok();
}

void DB::Impl::BackgroundFlushLoop() noexcept {
  std::unique_lock lock(mutex_);
  while (true) {
    background_cv_.wait(lock, [&] { return worker_stopping_ || flush_requested_; });
    if (worker_stopping_)
      break;
    if (!immutable_memtable_) {
      flush_requested_ = false;
      continue;
    }
    flush_requested_ = false;
    background_flush_running_ = true;
    lock.unlock();

    Status status;
    try {
      status = FlushImmutableMemTable();
    } catch (const std::exception& error) {
      status =
          Status::IOError(std::string("background flush exception: ") + error.what());
    } catch (...) {
      status = Status::IOError("background flush raised an unknown exception");
    }

    lock.lock();
    background_flush_running_ = false;
    if (!status.ok()) {
      if (!terminal_error_)
        background_error_ = status.WithContext("background immutable flush");
      write_metrics_->background_flush_failures.fetch_add(1, std::memory_order_relaxed);
    } else {
      write_metrics_->background_flushes.fetch_add(1, std::memory_order_relaxed);
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
  }

  try {
    background_worker_ = std::thread(&Impl::BackgroundFlushLoop, this);
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

Result<std::string> DB::Impl::Get(std::string_view key) const {
  const auto lock_started = LockClock::now();
  std::shared_lock lock(mutex_);
  RecordReadLockWait(read_metrics_, lock_started);
  auto s = CheckOpen();
  if (!s.ok())
    return s;

  auto mem = memtable_.Get(key);
  if (mem.ok())
    return mem.value().type == internal::ValueType::kTombstone
               ? Result<std::string>(Status::NotFound("key was deleted"))
               : Result<std::string>(mem.value().value);
  if (mem.status().code() != StatusCode::kNotFound)
    return mem.status();

  if (immutable_memtable_) {
    auto immutable = immutable_memtable_->Get(key);
    if (immutable.ok())
      return immutable.value().type == internal::ValueType::kTombstone
                 ? Result<std::string>(Status::NotFound("key was deleted"))
                 : Result<std::string>(immutable.value().value);
    if (immutable.status().code() != StatusCode::kNotFound)
      return immutable.status();
  }

  if (manifest_ && manifest_->current().live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  const internal::BytewiseLess less;
  for (std::size_t i = tables_.size(); i > 0; --i) {
    const auto& meta = manifest_->current().live_tables[i - 1];
    if (less(key, meta.smallest_key) || less(meta.largest_key, key))
      continue;
    auto disk = tables_[i - 1]->Get(key);
    if (!disk.ok()) {
      if (disk.status().code() == StatusCode::kNotFound) {
        continue;
      }
      return disk.status();
    }
    if (disk.value().type == internal::ValueType::kTombstone)
      return Status::NotFound("key was deleted");
    return disk.value().value;
  }

  return Status::NotFound("key is absent");
}
Result<std::vector<Entry>> DB::Impl::Scan(std::string_view begin,
                                          std::string_view end) const {
  const auto lock_started = LockClock::now();
  std::shared_lock lock(mutex_);
  RecordReadLockWait(read_metrics_, lock_started);
  auto s = CheckOpen();
  if (!s.ok())
    return s;
  const internal::BytewiseLess less;
  if (!end.empty() && less(end, begin))
    return Status::InvalidArgument("scan begin is greater than end");
  if (manifest_ && manifest_->current().live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  std::vector<std::unique_ptr<internal::InternalIterator>> inputs;
  inputs.reserve(tables_.size() + 2);

  for (std::size_t i = 0; i < tables_.size(); ++i) {
    const auto& meta = manifest_->current().live_tables[i];
    if ((!end.empty() && !less(meta.smallest_key, end)) ||
        less(meta.largest_key, begin))
      continue;
    auto disk = tables_[i]->NewIterator(begin, end);
    if (!disk.ok())
      return disk.status();
    inputs.push_back(std::move(disk.value()));
  }

  auto memory = memtable_.NewIterator(begin, end);
  if (!memory.ok())
    return memory.status();
  inputs.push_back(std::move(memory.value()));

  if (immutable_memtable_) {
    auto immutable = immutable_memtable_->NewIterator(begin, end);
    if (!immutable.ok())
      return immutable.status();
    inputs.push_back(std::move(immutable.value()));
  }

  auto merged = internal::NewMergingIterator(std::move(inputs));
  if (!merged.ok())
    return merged.status();

  std::vector<Entry> out;
  while (merged.value()->Valid()) {
    const auto& entry = merged.value()->entry();
    if (entry.type == internal::ValueType::kValue)
      out.push_back({entry.user_key, entry.value});
    auto next = merged.value()->Next();
    if (!next.ok())
      return next;
  }
  if (!merged.value()->status().ok())
    return merged.value()->status();
  return out;
}

ReadMetrics DB::Impl::GetReadMetrics() const noexcept {
  return read_metrics_ ? read_metrics_->Snapshot() : ReadMetrics{};
}

WriteMetrics DB::Impl::GetWriteMetrics() const noexcept {
  return write_metrics_ ? write_metrics_->Snapshot() : WriteMetrics{};
}

Status DB::Impl::Compact() {
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  auto open = CheckOpen();
  if (!open.ok())
    return open;

  WaitForBackgroundFlush(lock);
  open = CheckOpen();
  if (!open.ok())
    return open;

  const auto& current = manifest_->current();
  if (current.live_tables.size() != tables_.size())
    return Status::Corruption("Manifest and Reader table sets differ");

  CleanupObsoleteFiles();
  if (tables_.empty())
    return Status::Ok();

  std::vector<std::filesystem::path> old_table_paths;
  std::vector<std::uint64_t> old_table_numbers;
  old_table_paths.reserve(current.live_tables.size());
  old_table_numbers.reserve(current.live_tables.size());
  for (const auto& table : current.live_tables) {
    old_table_paths.push_back(*path_ / internal::SstableFileName(table.file_number));
    old_table_numbers.push_back(table.file_number);
  }

  std::vector<std::unique_ptr<internal::InternalIterator>> inputs;
  inputs.reserve(tables_.size());
  for (const auto& table : tables_) {
    auto iterator = table->NewIterator({}, {});
    if (!iterator.ok())
      return iterator.status().WithContext("create compaction input iterator");
    inputs.push_back(std::move(iterator.value()));
  }
  auto merged = internal::NewMergingIterator(std::move(inputs));
  if (!merged.ok())
    return merged.status().WithContext("create compaction merge iterator");

  const std::uint64_t replacement_number = current.next_file_number;
  const auto temp = *path_ / internal::SstableTempFileName(replacement_number);
  const auto final = *path_ / internal::SstableFileName(replacement_number);
  std::unique_ptr<internal::SSTableBuilder> builder;

  while (merged.value()->Valid()) {
    const auto& entry = merged.value()->entry();
    if (entry.type == internal::ValueType::kValue) {
      if (!builder) {
        if (replacement_number == std::numeric_limits<std::uint64_t>::max())
          return Status::ResourceExhausted("file number space is exhausted");
        auto file = fs_->OpenWritable(temp, false);
        if (!file.ok())
          return file.status().WithContext("create temporary compacted SSTable");
        builder = std::make_unique<internal::SSTableBuilder>(
            std::move(file.value()), options_.sstable_block_bytes);
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
  merged.value().reset();

  internal::ManifestSnapshot next = current;
  next.live_tables.clear();
  std::vector<std::unique_ptr<internal::SSTableReader>> replacement_tables;

  if (builder) {
    auto built = builder->Finish();
    if (!built.ok())
      return built.status().WithContext("finish temporary compacted SSTable");
    builder.reset();

    auto verify_file = fs_->OpenRandomAccess(temp);
    if (!verify_file.ok())
      return verify_file.status().WithContext(
          "open temporary compacted SSTable for validation");
    auto verified =
        internal::SSTableReader::Open(std::move(verify_file.value()),
                                      replacement_number, block_cache_, read_metrics_);
    if (!verified.ok())
      return verified.status().WithContext("validate temporary compacted SSTable");
    auto properties = verified.value()->ValidateAndGetProperties();
    if (!properties.ok()) {
      return properties.status().WithContext(
          "validate temporary compacted SSTable data");
    }

    internal::TableMeta replacement{
        replacement_number,         built.value().file_size,
        built.value().smallest_key, built.value().largest_key,
        built.value().min_sequence, built.value().max_sequence};
    if (!Matches(replacement, properties.value()))
      return Status::Corruption("compacted SSTable metadata does not match file");

    auto status = fs_->Rename(temp, final);
    if (!status.ok())
      return status.WithContext("publish compacted SSTable filename");
    status = fs_->SyncDir(*path_);
    if (!status.ok())
      return status.WithContext("sync database directory after compaction rename");

    next.next_file_number = replacement_number + 1;
    next.live_tables.push_back(std::move(replacement));
    replacement_tables.reserve(1);
    replacement_tables.push_back(std::move(verified.value()));
  }

  // This is the compaction commit point. All input iteration, replacement
  // validation, and in-memory allocations are complete before publication.
  auto published = manifest_->Publish(std::move(next));
  if (!published.durable()) {
    if (published.state() == internal::ManifestPublishState::kVisibleNotDurable) {
      terminal_error_ = published.status().WithContext(
          "publish compaction MANIFEST: replacement may be visible; close and "
          "reopen the database");
      return *terminal_error_;
    }
    return published.status().WithContext("publish compaction MANIFEST");
  }

  // Publication made the replacement authoritative. The swap is noexcept;
  // closing old readers and removing their files are best-effort cleanup.
  tables_.swap(replacement_tables);
  replacement_tables.clear();
  for (const auto old_table_number : old_table_numbers)
    block_cache_->EraseTable(old_table_number);
  bool removed_any = false;
  for (const auto& old_table : old_table_paths)
    removed_any = BestEffortRemove(old_table) || removed_any;
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
  const auto lock_started = LockClock::now();
  std::unique_lock lock(mutex_);
  RecordWriteLockWait(read_metrics_, lock_started);
  if (closed_)
    return Status::AlreadyClosed("database is closed");

  closing_ = true;
  while (!background_error_ && !terminal_error_) {
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
    return background_error_.value_or(Status::Ok());
  }

  if (options_.sync_on_write) {
    auto s = wal_->Sync();
    if (!s.ok()) {
      closing_ = false;
      background_cv_.notify_all();
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
