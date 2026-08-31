#pragma once

#include <filesystem>

namespace tinylsm::test {

class TempDir {
public:
  TempDir();
  ~TempDir();

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace tinylsm::test
