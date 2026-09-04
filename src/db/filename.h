#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tinylsm::internal {

enum class NumberedFileType {
  kWal,
  kSstable,
  kSstableTemp,
};

struct NumberedFileName {
  std::uint64_t number;
  NumberedFileType type;

  bool operator==(const NumberedFileName&) const = default;
};

/// Formats a positive file number using at least six decimal digits.
std::string FormatNumberedFileName(std::uint64_t number, NumberedFileType type);

/// Parses only canonical numbered WAL, SSTable, and temporary SSTable names.
std::optional<NumberedFileName> ParseNumberedFileName(std::string_view name);

inline std::string WalFileName(std::uint64_t number) {
  return FormatNumberedFileName(number, NumberedFileType::kWal);
}

inline std::string SstableFileName(std::uint64_t number) {
  return FormatNumberedFileName(number, NumberedFileType::kSstable);
}

inline std::string SstableTempFileName(std::uint64_t number) {
  return FormatNumberedFileName(number, NumberedFileType::kSstableTemp);
}

} // namespace tinylsm::internal
