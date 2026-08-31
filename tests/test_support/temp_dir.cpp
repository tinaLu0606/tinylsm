#include "test_support/temp_dir.h"

#include <chrono>
#include <string>
#include <unistd.h>

namespace tinylsm::test {

TempDir::TempDir() {
  path_ = std::filesystem::temp_directory_path() /
          ("tinylsm-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

} // namespace tinylsm::test
