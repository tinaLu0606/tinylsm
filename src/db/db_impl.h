#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "db/snapshot_registry.h"
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
  std::atomic<std::uint64_t> logical_write_bytes{0};
  std::atomic<std::uint64_t> wal_syncs{0};
  std::atomic<std::uint64_t> memtable_rotations{0};
  std::atomic<std::uint64_t> background_flushes{0};
  std::atomic<std::uint64_t> background_flush_failures{0};
  std::atomic<std::uint64_t> backpressure_waits{0};
  std::atomic<std::uint64_t> backpressure_wait_nanoseconds{0};
  std::atomic<std::uint64_t> group_commits{0};
  std::atomic<std::uint64_t> grouped_write_requests{0};
  std::atomic<std::uint64_t> writer_queue_wait_nanoseconds{0};
  std::atomic<std::size_t> writer_queue_depth{0};
  std::atomic<std::size_t> max_writer_queue_depth{0};
  std::atomic<std::size_t> background_queue_depth{0};
  std::atomic<std::size_t> max_background_queue_depth{0};
  std::atomic<std::size_t> immutable_memtable_bytes{0};
  std::atomic<std::size_t> max_immutable_memtable_bytes{0};
};

class CompactionMetricsState {
public:
  [[nodiscard]] CompactionMetrics Snapshot() const noexcept;

  std::atomic<std::uint64_t> compactions{0};
  std::atomic<std::uint64_t> background_compactions{0};
  std::atomic<std::uint64_t> compaction_failures{0};
  std::atomic<std::uint64_t> compaction_input_tables{0};
  std::atomic<std::uint64_t> compaction_output_tables{0};
  std::atomic<std::uint64_t> compaction_input_bytes{0};
  std::atomic<std::uint64_t> compaction_output_bytes{0};
  std::atomic<std::uint64_t> flush_output_bytes{0};
  std::atomic<std::size_t> table_count{0};
  std::atomic<std::size_t> live_sstable_bytes{0};
  std::atomic<std::size_t> compaction_debt_tables{0};
  std::atomic<std::size_t> compaction_debt_bytes{0};
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
  Result<std::string> Get(std::string_view key, const Snapshot* snapshot) const;
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;
  Result<std::shared_ptr<const Snapshot>> GetSnapshot() const;
  Result<std::unique_ptr<Iterator>> NewIterator(std::string_view begin,
                                                std::string_view end,
                                                const Snapshot* snapshot) const;
  [[nodiscard]] ReadMetrics GetReadMetrics() const noexcept;
  [[nodiscard]] WriteMetrics GetWriteMetrics() const noexcept;
  [[nodiscard]] CompactionMetrics GetCompactionMetrics() const noexcept;
  [[nodiscard]] SnapshotMetrics GetSnapshotMetrics() const noexcept;
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

  struct WriterRequest;
  Status SubmitWrite(WriteBatch batch);
  Status ValidateWriteBatch(const WriteBatch& batch, std::size_t& queue_bytes) const;
  void DrainWriterQueue();
  Status ApplyWriteGroup(const std::vector<std::shared_ptr<WriterRequest>>& group);
  void StopWriterQueue() noexcept;
  void ResumeWriterQueue() noexcept;
  Status PrepareForWrite(std::unique_lock<std::shared_mutex>& lock);
  Status RotateMemTable();
  Status FlushImmutableMemTable();
  Status CompactTablePrefix(std::size_t input_count, bool background);
  void BackgroundWorkLoop() noexcept;
  Status StartBackgroundWorker();
  void StopBackgroundWorker() noexcept;
  void WaitForBackgroundFlush(std::unique_lock<std::shared_mutex>& lock);
  void WaitForBackgroundWork(std::unique_lock<std::shared_mutex>& lock);
  void UpdateCompactionGauges() noexcept;
  [[nodiscard]] bool NeedsBackgroundCompaction() const noexcept;

  bool BestEffortRemove(const std::filesystem::path& path) noexcept;
  void BestEffortClose(internal::WalWriter* wal) noexcept;
  void RememberCleanup(const std::filesystem::path& path) noexcept;
  void BestEffortSyncDir() noexcept;
  Status CheckOpen() const;

  Options options_;
  std::optional<std::filesystem::path> path_;
  std::unique_ptr<internal::FileSystem> fs_;
  /// Held for the process lifetime of an open handle; released explicitly in
  /// Close() so a synchronous reopen of the same path does not race against
  /// this object's eventual destruction.
  std::unique_ptr<internal::FileLock> db_lock_;
  internal::MemTable memtable_;
  std::unique_ptr<internal::MemTable> immutable_memtable_;
  std::unique_ptr<internal::WalWriter> wal_;
  /// Readers are kept in the Manifest's oldest-to-newest order.
  std::vector<std::shared_ptr<internal::SSTableReader>> tables_;
  std::unique_ptr<internal::ManifestState> manifest_;
  std::shared_ptr<internal::ReadMetricsState> read_metrics_;
  std::shared_ptr<internal::WriteMetricsState> write_metrics_;
  std::shared_ptr<internal::CompactionMetricsState> compaction_metrics_;
  std::shared_ptr<internal::BlockCache> block_cache_;
  std::shared_ptr<internal::SnapshotState> snapshot_state_;
  /// Set when the authoritative on-disk Manifest is uncertain. Close remains
  /// available, but every data operation fails until the caller reopens the DB.
  std::optional<Status> terminal_error_;
  std::optional<Status> background_error_;
  std::vector<std::filesystem::path> pending_cleanup_;
  std::uint64_t next_sequence_ = 1;
  bool closed_ = false;
  bool closing_ = false;
  bool flush_requested_ = false;
  bool compaction_requested_ = false;
  bool background_flush_running_ = false;
  bool background_compaction_running_ = false;
  bool worker_stopping_ = false;
  std::thread background_worker_;
  std::condition_variable_any background_cv_;
  mutable std::shared_mutex mutex_;
  std::mutex writer_mutex_;
  std::condition_variable writer_cv_;
  std::deque<std::shared_ptr<WriterRequest>> writer_queue_;
  std::size_t writer_queue_bytes_ = 0;
  bool writer_leader_ = false;
  bool writer_stopping_ = false;
};

} // namespace tinylsm
