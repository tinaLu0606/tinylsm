#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "io/file.h"
#include "manifest/manifest_state.h"
#include "memtable/memtable.h"
#include "sstable/sstable_reader.h"
#include "tinylsm/db.h"
#include "wal/wal_writer.h"

namespace tinylsm {

namespace internal {

class WriteMetricsState {
public:
  [[nodiscard]] WriteMetrics Snapshot() const noexcept;

  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> write_batches{0};
  std::atomic<std::uint64_t> wal_syncs{0};
  std::atomic<std::uint64_t> memtable_rotations{0};
  std::atomic<std::uint64_t> background_flushes{0};
  std::atomic<std::uint64_t> background_flush_failures{0};
  std::atomic<std::uint64_t> backpressure_waits{0};
  std::atomic<std::uint64_t> backpressure_wait_nanoseconds{0};
  std::atomic<std::size_t> background_queue_depth{0};
  std::atomic<std::size_t> max_background_queue_depth{0};
  std::atomic<std::size_t> immutable_memtable_bytes{0};
  std::atomic<std::size_t> max_immutable_memtable_bytes{0};
};

} // namespace internal

/// Coordinates storage modules and owns the ordering and crash-consistency
/// rules that do not belong to any individual file format.
class DB::Impl {
public:
  static Result<std::unique_ptr<Impl>> Open(const std::filesystem::path& path,
                                            Options options);
  static Result<std::unique_ptr<Impl>> Open(const std::filesystem::path& path,
                                            Options options,
                                            std::unique_ptr<internal::FileSystem> fs);
  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  Status Write(const WriteBatch& batch);
  Result<std::string> Get(std::string_view key) const;
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;
  [[nodiscard]] ReadMetrics GetReadMetrics() const noexcept;
  [[nodiscard]] WriteMetrics GetWriteMetrics() const noexcept;
  Status Compact();
  Status Close();
  ~Impl();

private:
  friend class internal::DBTestPeer;
  friend class internal::DBLabPeer;

  Impl() = default;

  Status EnsureDatabaseDirectory();
  Result<internal::ManifestSnapshot> LoadManifest();
  Result<bool> InspectInitialFiles();
  Result<internal::ManifestSnapshot> CreateInitialManifest();
  Status OpenManifestSSTables(const internal::ManifestSnapshot& snapshot);
  Status RecoverWals(const internal::ManifestSnapshot& snapshot);
  Status RecoverWal(std::uint64_t wal_number, std::uint64_t floor,
                    internal::MemTable& target, std::uint64_t* recovered_max);
  void CleanupObsoleteFiles() noexcept;

  /// Performs the WAL-first write path and rotates a full active MemTable.
  /// A background flush error becomes sticky after the record was accepted.
  Status WriteEntry(std::string_view key, std::string_view value,
                    internal::ValueType type,
                    std::unique_lock<std::shared_mutex>& lock);
  Status PrepareForWrite(std::unique_lock<std::shared_mutex>& lock);
  Status RotateMemTable();
  Status FlushImmutableMemTable();
  void BackgroundFlushLoop() noexcept;
  Status StartBackgroundWorker();
  void StopBackgroundWorker() noexcept;
  void WaitForBackgroundFlush(std::unique_lock<std::shared_mutex>& lock);

  bool BestEffortRemove(const std::filesystem::path& path) noexcept;
  void BestEffortClose(internal::WalWriter* wal) noexcept;
  void RememberCleanup(const std::filesystem::path& path) noexcept;
  void BestEffortSyncDir() noexcept;
  Status CheckOpen() const;

  Options options_;
  std::optional<std::filesystem::path> path_;
  std::unique_ptr<internal::FileSystem> fs_;
  internal::MemTable memtable_;
  std::unique_ptr<internal::MemTable> immutable_memtable_;
  std::unique_ptr<internal::WalWriter> wal_;
  /// Readers are kept in the Manifest's oldest-to-newest order.
  std::vector<std::unique_ptr<internal::SSTableReader>> tables_;
  std::unique_ptr<internal::ManifestState> manifest_;
  std::shared_ptr<internal::ReadMetricsState> read_metrics_;
  std::shared_ptr<internal::WriteMetricsState> write_metrics_;
  std::shared_ptr<internal::BlockCache> block_cache_;
  /// Set when the authoritative on-disk Manifest is uncertain. Close remains
  /// available, but every data operation fails until the caller reopens the DB.
  std::optional<Status> terminal_error_;
  std::optional<Status> background_error_;
  std::vector<std::filesystem::path> pending_cleanup_;
  std::uint64_t next_sequence_ = 1;
  bool closed_ = false;
  bool closing_ = false;
  bool flush_requested_ = false;
  bool background_flush_running_ = false;
  bool worker_stopping_ = false;
  std::thread background_worker_;
  std::condition_variable_any background_cv_;
  mutable std::shared_mutex mutex_;
};

} // namespace tinylsm
