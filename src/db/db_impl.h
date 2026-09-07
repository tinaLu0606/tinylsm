#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "io/file.h"
#include "manifest/manifest_state.h"
#include "memtable/memtable.h"
#include "sstable/sstable_reader.h"
#include "tinylsm/db.h"
#include "wal/wal_writer.h"

namespace tinylsm {

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
  Status Compact();
  Status Close();
  ~Impl();

private:
  friend class internal::DBLabPeer;

  Impl() = default;

  Status EnsureDatabaseDirectory();
  Result<internal::ManifestSnapshot> LoadManifest();
  Result<bool> InspectInitialFiles();
  Result<internal::ManifestSnapshot> CreateInitialManifest();
  Status OpenManifestSSTables(const internal::ManifestSnapshot& snapshot);
  Status RecoverActiveWal(const internal::ManifestSnapshot& snapshot);
  void CleanupObsoleteFiles() noexcept;

  /// Performs the WAL-first write path and may synchronously trigger a flush.
  /// A flush error can occur after the record was accepted earlier.
  Status WriteEntry(std::string_view key, std::string_view value,
                    internal::ValueType type);

  /// Appends one SSTable and publishes a replacement WAL. Manifest publication
  /// is the commit point; later in-memory switching and cleanup cannot fail the write.
  Status FlushMemTable();
  bool BestEffortRemove(const std::filesystem::path& path) noexcept;
  void BestEffortClose(internal::WalWriter* wal) noexcept;
  void RememberCleanup(const std::filesystem::path& path) noexcept;
  void BestEffortSyncDir() noexcept;
  Status CheckOpen() const;

  Options options_;
  std::optional<std::filesystem::path> path_;
  std::unique_ptr<internal::FileSystem> fs_;
  internal::MemTable memtable_;
  std::unique_ptr<internal::WalWriter> wal_;
  /// Readers are kept in the Manifest's oldest-to-newest order.
  std::vector<std::unique_ptr<internal::SSTableReader>> tables_;
  std::unique_ptr<internal::ManifestState> manifest_;
  /// Set when the authoritative on-disk Manifest is uncertain. Close remains
  /// available, but every data operation fails until the caller reopens the DB.
  std::optional<Status> terminal_error_;
  std::vector<std::filesystem::path> pending_cleanup_;
  std::uint64_t next_sequence_ = 1;
  bool closed_ = false;
  mutable std::mutex mutex_;
};

} // namespace tinylsm
