#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tinylsm/options.h"
#include "tinylsm/read_metrics.h"
#include "tinylsm/result.h"
#include "tinylsm/types.h"
#include "tinylsm/write_batch.h"
#include "tinylsm/write_metrics.h"

namespace tinylsm {

namespace internal {
class DBTestPeer;
class DBLabPeer;
} // namespace internal

/// A movable, non-copyable key-value database handle.
///
/// Keys and values are arbitrary byte strings. Public operations on one DB
/// instance may be called from multiple threads: Get(), Scan(), and internal
/// diagnostic snapshots share a read lock; Put(), Delete(), Write(), Compact(),
/// and Close() coordinate state with the exclusive lock. A long Scan can therefore
/// delay a writer. Flush I/O runs on one background worker after MemTable rotation.
/// Moving or destroying an instance still requires exclusive ownership.
/// Expected database failures use Status/Result. Standard-library and dependency
/// exceptions, including std::bad_alloc, may propagate through this interface.
class DB final {
public:
  /// Opens or recovers a persistent database at `db_path`.
  ///
  /// When the path does not contain a database, `options.create_if_missing`
  /// controls whether a new one is created. The returned Result contains a DB
  /// on success and a Status describing an invalid option, I/O failure, or
  /// corrupted persistent state on failure.
  static Result<std::unique_ptr<DB>> Open(const std::filesystem::path& db_path,
                                          Options options = {});

  /// Releases owned resources. Prefer Close() when close errors must be observed.
  ~DB();

  /// Transfers ownership of an open or closed database handle.
  DB(DB&&) noexcept;
  DB& operator=(DB&&) noexcept;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  /// Associates `key` with `value`, replacing the key's previous value.
  ///
  /// Empty values are valid. For a persistent database, acknowledgement follows
  /// the durability policy selected by Options::sync_on_write. An error Status or
  /// propagated exception may occur after the WAL or MemTable has accepted the
  /// write, so callers must not assume that a failed Put left the key unchanged.
  /// A persistence error can make the current handle unusable; in that state all
  /// later data operations fail until the caller closes and reopens the DB.
  Status Put(std::string_view key, std::string_view value);

  /// Returns the current value for `key`.
  ///
  /// A missing or deleted key returns a Result with StatusCode::kNotFound. An
  /// empty stored value is returned successfully and is distinct from NotFound.
  Result<std::string> Get(std::string_view key) const;

  /// Deletes `key` by recording a tombstone.
  ///
  /// As with Put(), an error Status or propagated exception may occur after the
  /// delete was accepted by an earlier stage of the write path. A persistence
  /// error can require closing and reopening the DB before further operations.
  Status Delete(std::string_view key);

  /// Atomically appends and applies every operation in the batch.
  ///
  /// A complete batch is recovered in full after a crash. An incomplete final
  /// WAL batch is discarded in full. Operations retain insertion order, so the
  /// last operation for a repeated key wins. An empty batch is a successful
  /// no-op.
  Status Write(const WriteBatch& batch);

  /// Returns live entries in byte-wise key order over the range [begin, end).
  ///
  /// An empty `end` means the range is unbounded above. Deleted entries are not
  /// returned. A non-empty end less than begin returns kInvalidArgument.
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;

  /// Returns cumulative read-path and lock counters without resetting them.
  /// Metrics remain available after Close().
  [[nodiscard]] ReadMetrics GetReadMetrics() const noexcept;

  /// Returns cumulative write/flush/backpressure counters without resetting them.
  /// Metrics remain available after Close().
  [[nodiscard]] WriteMetrics GetWriteMetrics() const noexcept;

  /// Rewrites all currently published SSTables into zero or one replacement.
  ///
  /// Compaction is synchronous and does not flush or modify the MemTable or
  /// active WAL. Because it covers every published table, obsolete versions
  /// and tombstones are removed from the replacement.
  Status Compact();

  /// Closes writable resources, syncing the WAL first when sync_on_write is enabled.
  ///
  /// A sync failure leaves the DB open so the caller may retry. Once the
  /// underlying close is attempted, the DB is closed even if close reports an error.
  /// A successful Close returns OK even if an earlier operation required reopen.
  Status Close();

private:
  friend class internal::DBTestPeer;
  friend class internal::DBLabPeer;
  class Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace tinylsm
