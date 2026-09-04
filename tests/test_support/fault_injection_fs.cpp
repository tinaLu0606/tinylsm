#include "test_support/fault_injection_fs.h"

#include <stdexcept>
#include <utility>

namespace tinylsm::test {
namespace {

class FaultRandomAccessFile final : public internal::RandomAccessFile {
public:
  FaultRandomAccessFile(std::filesystem::path path,
                        std::unique_ptr<internal::RandomAccessFile> inner,
                        std::shared_ptr<FaultPlan> plan)
      : path_(std::move(path)), inner_(std::move(inner)), plan_(std::move(plan)) {}

  Result<std::size_t> ReadAt(std::uint64_t offset,
                             std::span<std::byte> buffer) const override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kReadAt, path_, FaultTiming::kBefore))
      return *failure;
    auto read = inner_->ReadAt(offset, buffer);
    if (!read.ok())
      return read.status();
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kReadAt, path_, FaultTiming::kAfter))
      return *failure;
    return read.value();
  }

  Result<std::uint64_t> Size() const override { return inner_->Size(); }

private:
  std::filesystem::path path_;
  std::unique_ptr<internal::RandomAccessFile> inner_;
  std::shared_ptr<FaultPlan> plan_;
};

class FaultWritableFile final : public internal::WritableFile {
public:
  FaultWritableFile(std::filesystem::path path,
                    std::unique_ptr<internal::WritableFile> inner,
                    std::shared_ptr<FaultPlan> plan)
      : path_(std::move(path)), inner_(std::move(inner)), plan_(std::move(plan)) {}

  Status Append(std::span<const std::byte> data) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kAppend, path_, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->Append(data);
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kAppend, path_, FaultTiming::kAfter))
      return *failure;
    return status;
  }

  Status Sync() override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kSync, path_, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->Sync();
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kSync, path_, FaultTiming::kAfter))
      return *failure;
    return status;
  }

  Status Close() override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kClose, path_, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->Close();
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kClose, path_, FaultTiming::kAfter))
      return *failure;
    return status;
  }

private:
  std::filesystem::path path_;
  std::unique_ptr<internal::WritableFile> inner_;
  std::shared_ptr<FaultPlan> plan_;
};

class FaultInjectionFileSystem final : public internal::FileSystem {
public:
  FaultInjectionFileSystem(std::unique_ptr<internal::FileSystem> inner,
                           std::shared_ptr<FaultPlan> plan)
      : inner_(std::move(inner)), plan_(std::move(plan)) {}

  Result<std::unique_ptr<internal::SequentialFile>>
  OpenSequential(const std::filesystem::path& path) override {
    return inner_->OpenSequential(path);
  }

  Result<std::unique_ptr<internal::RandomAccessFile>>
  OpenRandomAccess(const std::filesystem::path& path) override {
    if (auto failure = plan_->MaybeFail(FaultOperation::kOpenRandomAccess, path,
                                        FaultTiming::kBefore))
      return *failure;
    auto file = inner_->OpenRandomAccess(path);
    if (!file.ok())
      return file.status();
    if (auto failure = plan_->MaybeFail(FaultOperation::kOpenRandomAccess, path,
                                        FaultTiming::kAfter))
      return *failure;
    return std::unique_ptr<internal::RandomAccessFile>(
        new FaultRandomAccessFile(path, std::move(file.value()), plan_));
  }

  Result<std::unique_ptr<internal::WritableFile>>
  OpenWritable(const std::filesystem::path& path, bool append) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kOpenWritable, path, FaultTiming::kBefore))
      return *failure;
    auto file = inner_->OpenWritable(path, append);
    if (!file.ok())
      return file.status();
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kOpenWritable, path, FaultTiming::kAfter))
      return *failure;
    return std::unique_ptr<internal::WritableFile>(
        new FaultWritableFile(path, std::move(file.value()), plan_));
  }

  Status CreateDir(const std::filesystem::path& path) override {
    return inner_->CreateDir(path);
  }

  Result<std::vector<std::filesystem::path>>
  ListDir(const std::filesystem::path& path) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kListDir, path, FaultTiming::kBefore))
      return *failure;
    auto listed = inner_->ListDir(path);
    if (!listed.ok())
      return listed.status();
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kListDir, path, FaultTiming::kAfter))
      return *failure;
    return std::move(listed.value());
  }

  Status Rename(const std::filesystem::path& from,
                const std::filesystem::path& to) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kRename, to, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->Rename(from, to);
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kRename, to, FaultTiming::kAfter))
      return *failure;
    return status;
  }

  Status Remove(const std::filesystem::path& path) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kRemove, path, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->Remove(path);
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kRemove, path, FaultTiming::kAfter))
      return *failure;
    return status;
  }

  Status Truncate(const std::filesystem::path& path, std::uint64_t size) override {
    return inner_->Truncate(path, size);
  }

  Result<bool> FileExists(const std::filesystem::path& path) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kFileExists, path, FaultTiming::kBefore))
      return *failure;
    auto exists = inner_->FileExists(path);
    if (!exists.ok())
      return exists.status();
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kFileExists, path, FaultTiming::kAfter))
      return *failure;
    return exists.value();
  }

  Status SyncDir(const std::filesystem::path& path) override {
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kSyncDir, path, FaultTiming::kBefore))
      return *failure;
    auto status = inner_->SyncDir(path);
    if (!status.ok())
      return status;
    if (auto failure =
            plan_->MaybeFail(FaultOperation::kSyncDir, path, FaultTiming::kAfter))
      return *failure;
    return status;
  }

private:
  std::unique_ptr<internal::FileSystem> inner_;
  std::shared_ptr<FaultPlan> plan_;
};

} // namespace

void FaultPlan::Fail(FaultOperation operation, std::string path_suffix,
                     std::size_t occurrence, FaultTiming timing) {
  rules_.push_back({operation, std::move(path_suffix), occurrence, timing, false, 0});
}

void FaultPlan::Throw(FaultOperation operation, std::string path_suffix,
                      std::size_t occurrence, FaultTiming timing) {
  rules_.push_back({operation, std::move(path_suffix), occurrence, timing, true, 0});
}

std::optional<Status> FaultPlan::MaybeFail(FaultOperation operation,
                                           const std::filesystem::path& path,
                                           FaultTiming timing) {
  const auto text = path.generic_string();
  for (auto& rule : rules_) {
    if (rule.operation != operation || rule.timing != timing ||
        !text.ends_with(rule.path_suffix))
      continue;
    ++rule.matches;
    if (rule.matches != rule.occurrence)
      continue;
    if (rule.throws)
      throw std::runtime_error("injected filesystem exception");
    return Status::IOError("injected filesystem failure for " + text);
  }
  return std::nullopt;
}

std::unique_ptr<internal::FileSystem>
NewFaultInjectionFileSystem(std::shared_ptr<FaultPlan> plan) {
  return std::make_unique<FaultInjectionFileSystem>(internal::NewPosixFileSystem(),
                                                    std::move(plan));
}

} // namespace tinylsm::test
