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

class DB final {
public:
  static Result<std::unique_ptr<DB>> OpenInMemory();
  static Result<std::unique_ptr<DB>> Open(const std::filesystem::path& db_path,
                                          Options options = {});

  ~DB();
  DB(DB&&) noexcept;
  DB& operator=(DB&&) noexcept;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(std::string_view key, std::string_view value);
  Result<std::string> Get(std::string_view key) const;
  Status Delete(std::string_view key);
  Result<std::vector<Entry>> Scan(std::string_view begin, std::string_view end) const;
  Status Close();

private:
  class Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace tinylsm
