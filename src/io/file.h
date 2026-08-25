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

class SequentialFile {
public:
  virtual ~SequentialFile() = default;
  virtual Result<std::size_t> Read(std::span<std::byte> buffer) = 0;
};
class RandomAccessFile {
public:
  virtual ~RandomAccessFile() = default;
  virtual Result<std::size_t> ReadAt(std::uint64_t offset, std::span<std::byte> buffer) const = 0;
  virtual Result<std::uint64_t> Size() const = 0;
};
class WritableFile {
public:
  virtual ~WritableFile() = default;
  virtual Status Append(std::span<const std::byte> data) = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};
class FileSystem {
public:
  virtual ~FileSystem() = default;
  virtual Result<std::unique_ptr<SequentialFile>>
  OpenSequential(const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<RandomAccessFile>>
  OpenRandomAccess(const std::filesystem::path& path) = 0;
  virtual Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path& path,
                                                             bool append) = 0;
  virtual Status CreateDir(const std::filesystem::path& path) = 0;
  virtual Result<std::vector<std::filesystem::path>> ListDir(const std::filesystem::path& path) = 0;
  virtual Status Rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
  virtual Status Remove(const std::filesystem::path& path) = 0;
  virtual Status Truncate(const std::filesystem::path& path, std::uint64_t size) = 0;
  virtual bool FileExists(const std::filesystem::path& path) = 0;
  virtual Status SyncDir(const std::filesystem::path& path) = 0;
};

std::unique_ptr<FileSystem> NewPosixFileSystem();
using PosixWriteFunction = std::function<std::ptrdiff_t(int, const void*, std::size_t)>;
std::unique_ptr<FileSystem> NewPosixFileSystemForTesting(PosixWriteFunction write_function);
Status ReadExactly(const RandomAccessFile& file, std::uint64_t offset, std::span<std::byte> buffer);

} // namespace tinylsm::internal
