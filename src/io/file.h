#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tinylsm/result.h"

namespace tinylsm::internal {

/// A forward-only byte stream. Read may return fewer bytes than requested;
/// returning zero bytes indicates EOF.
class SequentialFile {
public:
  virtual ~SequentialFile() = default;
  virtual Result<std::size_t> Read(std::span<std::byte> buffer) = 0;
};

/// A byte source that supports positional reads without changing shared state.
class RandomAccessFile {
public:
  virtual ~RandomAccessFile() = default;
  virtual Result<std::size_t> ReadAt(std::uint64_t offset,
                                     std::span<std::byte> buffer) const = 0;
  virtual Result<std::uint64_t> Size() const = 0;
};

/// An append-only byte sink with explicit durability and close operations.
class WritableFile {
public:
  virtual ~WritableFile() = default;

  /// Appends the complete span or returns an error; short OS writes are retried.
  virtual Status Append(std::span<const std::byte> data) = 0;

  /// Requests durable persistence of previously appended bytes.
  virtual Status Sync() = 0;

  /// Explicitly closes the handle so close errors can be observed.
  virtual Status Close() = 0;
};

/// A held advisory lock on a database's LOCK file. Releasing it (destroying
/// the handle) is the only supported way to unlock; there is no explicit
/// Unlock() to avoid a use-after-unlock state.
class FileLock {
public:
  virtual ~FileLock() = default;
};

/// Filesystem operations used by storage modules. This layer understands paths
/// and bytes only; it does not interpret WAL, SSTable, or Manifest contents.
class FileSystem {
public:
  virtual ~FileSystem() = default;
  virtual Result<std::unique_ptr<SequentialFile>>
  OpenSequential(const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<RandomAccessFile>>
  OpenRandomAccess(const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<WritableFile>>
  OpenWritable(const std::filesystem::path& path, bool append) = 0;
  virtual Status CreateDir(const std::filesystem::path& path) = 0;
  virtual Result<std::vector<std::filesystem::path>>
  ListDir(const std::filesystem::path& path) = 0;
  /// Atomically replaces `to` with `from` when supported by the filesystem.
  /// Call SyncDir() separately when the directory entry must be durable.
  virtual Status Rename(const std::filesystem::path& from,
                        const std::filesystem::path& to) = 0;
  virtual Status Remove(const std::filesystem::path& path) = 0;
  virtual Status Truncate(const std::filesystem::path& path, std::uint64_t size) = 0;
  /// Returns false only when the path is absent; inspection failures are errors.
  virtual Result<bool> FileExists(const std::filesystem::path& path) = 0;
  /// Persists directory-entry changes such as Rename() and Remove().
  virtual Status SyncDir(const std::filesystem::path& path) = 0;
  /// Creates `path` if missing and takes an exclusive, non-blocking advisory
  /// lock on it. Returns an IOError when another handle already holds the
  /// lock, including a second handle in this same process.
  virtual Result<std::unique_ptr<FileLock>>
  LockFile(const std::filesystem::path& path) = 0;
};

std::unique_ptr<FileSystem> NewPosixFileSystem();
using PosixWriteFunction = std::function<std::ptrdiff_t(int, const void*, std::size_t)>;
std::unique_ptr<FileSystem>
NewPosixFileSystemForTesting(PosixWriteFunction write_function);
/// Fills `buffer` or returns an error, treating premature EOF as corruption.
Status ReadExactly(const RandomAccessFile& file, std::uint64_t offset,
                   std::span<std::byte> buffer);

} // namespace tinylsm::internal
