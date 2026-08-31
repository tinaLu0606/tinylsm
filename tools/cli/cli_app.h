#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace tinylsm::cli {

int RunCli(std::span<const std::string_view> arguments, std::ostream& output,
           std::ostream& error);

} // namespace tinylsm::cli
