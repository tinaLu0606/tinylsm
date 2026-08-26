#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tinylsm/options.h"
#include "tinylsm/result.h"
#include "tinylsm/types.h"

namespace tinylsm {

/// A movable, non-copyable key-value database handle.
///
/// Keys and values are arbitrary byte strings. DB instances are not thread-safe;
/// callers must provide external synchronization when sharing an instance.
class DB final {
public:
  /// Creates a process-local database with no persistent storage or recovery.
  static Result<std::unique_ptr<DB>> OpenInMemory();

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
  /// the durability policy selected by Options::sync_on_write. An error may be
  /// reported after the WAL or MemTable has accepted the write, so callers must
  /// not assume that a failed Put left the key unchanged.
  Status Put(std::string_view key, std::string_view value);

  /// Returns the current value for `key`.
  ///
  /// A missing or deleted key returns a Result with StatusCode::kNotFound. An
  /// empty stored value is returned successfully and is distinct from NotFound.
  Result<std::string> Get(std::string_view key) const;

  /// Deletes `key` by recording a tombstone.
  ///
  /// As with Put(), an error may be reported after the delete was accepted by an
  /// earlier stage of the write path.
  Status Delete(std::string_view key);

  /// Returns live entries in byte-wise key order over the range [begin, end).
  ///
  /// An empty `end` means the range is unbounded above. Deleted entries are not
  /// returned. A non-empty end less than begin returns kInvalidArgument.
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;

  /// Closes writable resources, syncing the WAL first when sync_on_write is enabled.
  ///
  /// The DB is considered closed after this call even if closing returns an
  /// error. Later operations, including another Close(), return kAlreadyClosed.
  Status Close();

private:
  class Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace tinylsm
