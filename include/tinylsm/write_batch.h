#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tinylsm {

/// A sequence of writes committed through DB::Write() as one WAL record.
class WriteBatch {
public:
  enum class OperationType { kPut, kDelete };

  struct Operation {
    OperationType type;
    std::string key;
    std::string value;
  };

  void Put(std::string_view key, std::string_view value) {
    operations_.push_back({OperationType::kPut, std::string(key), std::string(value)});
  }

  void Delete(std::string_view key) {
    operations_.push_back({OperationType::kDelete, std::string(key), {}});
  }

  void Clear() noexcept { operations_.clear(); }
  [[nodiscard]] bool Empty() const noexcept { return operations_.empty(); }
  [[nodiscard]] std::size_t Count() const noexcept { return operations_.size(); }
  [[nodiscard]] const std::vector<Operation>& Operations() const noexcept {
    return operations_;
  }

private:
  std::vector<Operation> operations_;
};

} // namespace tinylsm
