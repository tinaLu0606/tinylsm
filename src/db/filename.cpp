#include "db/filename.h"

#include <cassert>
#include <charconv>
#include <system_error>

namespace tinylsm::internal {
namespace {

std::string_view Suffix(NumberedFileType type) {
  switch (type) {
  case NumberedFileType::kWal:
    return ".wal";
  case NumberedFileType::kSstable:
    return ".sst";
  case NumberedFileType::kSstableTemp:
    return ".sst.tmp";
  }
  return {};
}

} // namespace

std::string FormatNumberedFileName(std::uint64_t number, NumberedFileType type) {
  assert(number > 0);
  auto digits = std::to_string(number);
  std::string result;
  if (digits.size() < 6)
    result.append(6 - digits.size(), '0');
  result += digits;
  result += Suffix(type);
  return result;
}

std::optional<NumberedFileName> ParseNumberedFileName(std::string_view name) {
  NumberedFileType type;
  std::string_view suffix;
  if (name.ends_with(Suffix(NumberedFileType::kSstableTemp))) {
    type = NumberedFileType::kSstableTemp;
    suffix = Suffix(type);
  } else if (name.ends_with(Suffix(NumberedFileType::kSstable))) {
    type = NumberedFileType::kSstable;
    suffix = Suffix(type);
  } else if (name.ends_with(Suffix(NumberedFileType::kWal))) {
    type = NumberedFileType::kWal;
    suffix = Suffix(type);
  } else {
    return std::nullopt;
  }

  const auto digits = name.substr(0, name.size() - suffix.size());
  if (digits.empty())
    return std::nullopt;
  std::uint64_t number = 0;
  const auto [end, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (error != std::errc{} || end != digits.data() + digits.size() || number == 0)
    return std::nullopt;

  if (FormatNumberedFileName(number, type) != name)
    return std::nullopt;
  return NumberedFileName{number, type};
}

} // namespace tinylsm::internal
