#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "io/file.h"

namespace tinylsm::test {

enum class FaultOperation {
  kOpenRandomAccess,
  kReadAt,
  kOpenWritable,
  kAppend,
  kSync,
  kClose,
  kRename,
  kRemove,
  kFileExists,
  kSyncDir,
};

enum class FaultTiming {
  kBefore,
  kAfter,
};

class FaultPlan {
public:
  void Fail(FaultOperation operation, std::string path_suffix = {},
            std::size_t occurrence = 1, FaultTiming timing = FaultTiming::kBefore);
  void Throw(FaultOperation operation, std::string path_suffix = {},
             std::size_t occurrence = 1, FaultTiming timing = FaultTiming::kBefore);

  std::optional<Status> MaybeFail(FaultOperation operation,
                                  const std::filesystem::path& path,
                                  FaultTiming timing);

private:
  struct Rule {
    FaultOperation operation;
    std::string path_suffix;
    std::size_t occurrence;
    FaultTiming timing;
    bool throws;
    std::size_t matches = 0;
  };

  std::vector<Rule> rules_;
};

std::unique_ptr<internal::FileSystem>
NewFaultInjectionFileSystem(std::shared_ptr<FaultPlan> plan);

} // namespace tinylsm::test
