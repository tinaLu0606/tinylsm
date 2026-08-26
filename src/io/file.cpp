#include "io/file.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace tinylsm::internal {
namespace {
Status Error(std::string_view op, const std::filesystem::path& path) {
  return Status::IOError(std::string(op) + " " + path.string() + ": " +
                         std::strerror(errno));
}
class PosixSequentialFile final : public SequentialFile {
public:
  PosixSequentialFile(int fd, std::filesystem::path path)
      : fd_(fd), path_(std::move(path)) {}
  ~PosixSequentialFile() override {
    if (fd_ >= 0)
      ::close(fd_);
  }
  Result<std::size_t> Read(std::span<std::byte> buffer) override {
    while (true) {
      const ssize_t n = ::read(fd_, buffer.data(), buffer.size());
      if (n >= 0)
        return static_cast<std::size_t>(n);
      if (errno != EINTR)
        return Error("read", path_);
    }
  }

private:
  int fd_;
  std::filesystem::path path_;
};
class PosixRandomAccessFile final : public RandomAccessFile {
public:
  PosixRandomAccessFile(int fd, std::filesystem::path path)
      : fd_(fd), path_(std::move(path)) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0)
      ::close(fd_);
  }
  Result<std::size_t> ReadAt(std::uint64_t offset,
                             std::span<std::byte> buffer) const override {
    while (true) {
      const ssize_t n =
          ::pread(fd_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
      if (n >= 0)
        return static_cast<std::size_t>(n);
      if (errno != EINTR)
        return Error("pread", path_);
    }
  }
  Result<std::uint64_t> Size() const override {
    struct stat st{};
    if (::fstat(fd_, &st) != 0)
      return Error("fstat", path_);
    return static_cast<std::uint64_t>(st.st_size);
  }

private:
  int fd_;
  std::filesystem::path path_;
};
class PosixWritableFile final : public WritableFile {
public:
  PosixWritableFile(int fd, std::filesystem::path path,
                    PosixWriteFunction write_function)
      : fd_(fd), path_(std::move(path)), write_function_(std::move(write_function)) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0)
      ::close(fd_);
  }
  Status Append(std::span<const std::byte> data) override {
    std::size_t done = 0;
    while (done < data.size()) {
      const std::ptrdiff_t n =
          write_function_(fd_, data.data() + done, data.size() - done);
      if (n > 0) {
        done += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      if (n == 0)
        errno = EIO;
      return Error("write", path_);
    }
    return Status::Ok();
  }
  Status Sync() override {
    if (::fsync(fd_) != 0)
      return Error("fsync", path_);
    return Status::Ok();
  }
  Status Close() override {
    if (fd_ < 0)
      return Status::Ok();
    const int fd = std::exchange(fd_, -1);
    if (::close(fd) != 0)
      return Error("close", path_);
    return Status::Ok();
  }

private:
  int fd_;
  std::filesystem::path path_;
  PosixWriteFunction write_function_;
};
class PosixFileSystem final : public FileSystem {
public:
  explicit PosixFileSystem(PosixWriteFunction write_function)
      : write_function_(std::move(write_function)) {}
  Result<std::unique_ptr<SequentialFile>>
  OpenSequential(const std::filesystem::path& p) override {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
      return Error("open", p);
    return std::unique_ptr<SequentialFile>(new PosixSequentialFile(fd, p));
  }
  Result<std::unique_ptr<RandomAccessFile>>
  OpenRandomAccess(const std::filesystem::path& p) override {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
      return Error("open", p);
    return std::unique_ptr<RandomAccessFile>(new PosixRandomAccessFile(fd, p));
  }
  Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path& p,
                                                     bool append) override {
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = ::open(p.c_str(), flags, 0644);
    if (fd < 0)
      return Error("open", p);
    return std::unique_ptr<WritableFile>(new PosixWritableFile(fd, p, write_function_));
  }
  Status CreateDir(const std::filesystem::path& p) override {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return ec ? Status::IOError("create directory " + p.string() + ": " + ec.message())
              : Status::Ok();
  }
  Result<std::vector<std::filesystem::path>>
  ListDir(const std::filesystem::path& p) override {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec))
      out.push_back(it->path());
    if (ec)
      return Status::IOError("list directory " + p.string() + ": " + ec.message());
    return out;
  }
  Status Rename(const std::filesystem::path& a,
                const std::filesystem::path& b) override {
    if (::rename(a.c_str(), b.c_str()) != 0)
      return Error("rename", a);
    return Status::Ok();
  }
  Status Remove(const std::filesystem::path& p) override {
    if (::unlink(p.c_str()) != 0 && errno != ENOENT)
      return Error("unlink", p);
    return Status::Ok();
  }
  Status Truncate(const std::filesystem::path& p, std::uint64_t n) override {
    if (::truncate(p.c_str(), static_cast<off_t>(n)) != 0)
      return Error("truncate", p);
    return Status::Ok();
  }
  bool FileExists(const std::filesystem::path& p) override {
    return ::access(p.c_str(), F_OK) == 0;
  }
  Status SyncDir(const std::filesystem::path& p) override {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
      return Error("open directory", p);
    if (::fsync(fd) != 0) {
      auto s = Error("fsync directory", p);
      ::close(fd);
      return s;
    }
    if (::close(fd) != 0)
      return Error("close directory", p);
    return Status::Ok();
  }

private:
  PosixWriteFunction write_function_;
};
} // namespace

std::unique_ptr<FileSystem> NewPosixFileSystem() {
  return std::make_unique<PosixFileSystem>(
      [](int fd, const void* data, std::size_t size) {
        return static_cast<std::ptrdiff_t>(::write(fd, data, size));
      });
}
std::unique_ptr<FileSystem>
NewPosixFileSystemForTesting(PosixWriteFunction write_function) {
  return std::make_unique<PosixFileSystem>(std::move(write_function));
}
Status ReadExactly(const RandomAccessFile& file, std::uint64_t offset,
                   std::span<std::byte> buffer) {
  std::size_t done = 0;
  while (done < buffer.size()) {
    auto read = file.ReadAt(offset + done, buffer.subspan(done));
    if (!read.ok())
      return read.status();
    if (read.value() == 0)
      return Status::Corruption("unexpected end of file");
    done += read.value();
  }
  return Status::Ok();
}
} // namespace tinylsm::internal
