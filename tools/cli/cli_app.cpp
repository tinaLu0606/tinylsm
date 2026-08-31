#include "cli/cli_app.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>

#include "tinylsm/db.h"

namespace tinylsm::cli {
namespace {

constexpr int kSuccess = 0;
constexpr int kFailure = 1;
constexpr int kUsageError = 2;
constexpr int kNotFound = 3;

enum class CommandType { kPut, kGet, kDelete, kScan };

struct ParsedCommand {
  std::filesystem::path db_path;
  CommandType type;
  std::string_view first;
  std::string_view second;
  bool json = false;
};

void PrintUsage(std::ostream& output) {
  output << "usage:\n"
            "  tinylsm_cli help\n"
            "  tinylsm_cli <db-path> put <key> <value>\n"
            "  tinylsm_cli <db-path> get <key> [--json]\n"
            "  tinylsm_cli <db-path> delete <key>\n"
            "  tinylsm_cli <db-path> scan [begin [end]] [--json]\n";
}

std::optional<ParsedCommand>
ParseArguments(std::span<const std::string_view> arguments) {
  if (arguments.size() < 2)
    return std::nullopt;

  ParsedCommand command;
  command.db_path = std::string(arguments[0]);
  const auto name = arguments[1];

  if (name == "put" && arguments.size() == 4) {
    command.type = CommandType::kPut;
    command.first = arguments[2];
    command.second = arguments[3];
    return command;
  }

  if (name == "get" && (arguments.size() == 3 || arguments.size() == 4)) {
    command.type = CommandType::kGet;
    command.first = arguments[2];
    command.json = arguments.size() == 4 && arguments[3] == "--json";
    if (arguments.size() == 3 || command.json)
      return command;
    return std::nullopt;
  }

  if (name == "delete" && arguments.size() == 3) {
    command.type = CommandType::kDelete;
    command.first = arguments[2];
    return command;
  }

  if (name == "scan") {
    auto operands = arguments.subspan(2);
    if (!operands.empty() && operands.back() == "--json") {
      command.json = true;
      operands = operands.first(operands.size() - 1);
    }
    if (operands.size() > 2)
      return std::nullopt;
    command.type = CommandType::kScan;
    if (!operands.empty())
      command.first = operands[0];
    if (operands.size() == 2)
      command.second = operands[1];
    return command;
  }

  return std::nullopt;
}

std::string Base64Encode(std::string_view bytes) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2) / 3) * 4);

  std::size_t offset = 0;
  while (offset + 3 <= bytes.size()) {
    const auto first = static_cast<std::uint8_t>(bytes[offset]);
    const auto second = static_cast<std::uint8_t>(bytes[offset + 1]);
    const auto third = static_cast<std::uint8_t>(bytes[offset + 2]);
    const std::uint32_t group = (static_cast<std::uint32_t>(first) << 16U) |
                                (static_cast<std::uint32_t>(second) << 8U) | third;
    encoded.push_back(alphabet[(group >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(group >> 12U) & 0x3fU]);
    encoded.push_back(alphabet[(group >> 6U) & 0x3fU]);
    encoded.push_back(alphabet[group & 0x3fU]);
    offset += 3;
  }

  const std::size_t remaining = bytes.size() - offset;
  if (remaining == 1) {
    const auto first = static_cast<std::uint8_t>(bytes[offset]);
    const std::uint32_t group = static_cast<std::uint32_t>(first) << 16U;
    encoded.push_back(alphabet[(group >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(group >> 12U) & 0x3fU]);
    encoded += "==";
  } else if (remaining == 2) {
    const auto first = static_cast<std::uint8_t>(bytes[offset]);
    const auto second = static_cast<std::uint8_t>(bytes[offset + 1]);
    const std::uint32_t group = (static_cast<std::uint32_t>(first) << 16U) |
                                (static_cast<std::uint32_t>(second) << 8U);
    encoded.push_back(alphabet[(group >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(group >> 12U) & 0x3fU]);
    encoded.push_back(alphabet[(group >> 6U) & 0x3fU]);
    encoded.push_back('=');
  }

  return encoded;
}

void WriteJsonEntry(std::ostream& output, std::string_view key,
                    std::string_view value) {
  output << "{\"key_base64\":\"" << Base64Encode(key) << "\",\"value_base64\":\""
         << Base64Encode(value) << "\"}\n";
}

int CloseAndReturn(DB& db, int command_result, std::ostream& error) {
  auto close = db.Close();
  if (!close.ok()) {
    error << close.ToString() << '\n';
    return kFailure;
  }
  return command_result;
}

int HandlePut(DB& db, const ParsedCommand& command, std::ostream& output,
              std::ostream& error) {
  auto status = db.Put(command.first, command.second);
  if (!status.ok()) {
    error << status.ToString() << '\n';
    return CloseAndReturn(db, kFailure, error);
  }

  const int result = CloseAndReturn(db, kSuccess, error);
  if (result == kSuccess)
    output << "OK\n";
  return result;
}

int HandleGet(DB& db, const ParsedCommand& command, std::ostream& output,
              std::ostream& error) {
  auto value = db.Get(command.first);
  if (!value.ok()) {
    error << value.status().ToString() << '\n';
    const int result =
        value.status().code() == StatusCode::kNotFound ? kNotFound : kFailure;
    return CloseAndReturn(db, result, error);
  }

  const int result = CloseAndReturn(db, kSuccess, error);
  if (result != kSuccess)
    return result;

  if (command.json)
    WriteJsonEntry(output, command.first, value.value());
  else
    output << value.value() << '\n';
  return kSuccess;
}

int HandleDelete(DB& db, const ParsedCommand& command, std::ostream& output,
                 std::ostream& error) {
  auto status = db.Delete(command.first);
  if (!status.ok()) {
    error << status.ToString() << '\n';
    return CloseAndReturn(db, kFailure, error);
  }

  const int result = CloseAndReturn(db, kSuccess, error);
  if (result == kSuccess)
    output << "OK\n";
  return result;
}

int HandleScan(DB& db, const ParsedCommand& command, std::ostream& output,
               std::ostream& error) {
  auto entries = db.Scan(command.first, command.second);
  if (!entries.ok()) {
    error << entries.status().ToString() << '\n';
    return CloseAndReturn(db, kFailure, error);
  }

  const int result = CloseAndReturn(db, kSuccess, error);
  if (result != kSuccess)
    return result;

  for (const auto& entry : entries.value()) {
    if (command.json)
      WriteJsonEntry(output, entry.key, entry.value);
    else
      output << entry.key << '\t' << entry.value << '\n';
  }
  return kSuccess;
}

} // namespace

int RunCli(std::span<const std::string_view> arguments, std::ostream& output,
           std::ostream& error) {
  if (arguments.size() == 1 &&
      (arguments[0] == "help" || arguments[0] == "-h" || arguments[0] == "--help")) {
    PrintUsage(output);
    return kSuccess;
  }

  auto parsed = ParseArguments(arguments);
  if (!parsed) {
    PrintUsage(error);
    return kUsageError;
  }

  auto opened = DB::Open(parsed->db_path);
  if (!opened.ok()) {
    error << opened.status().ToString() << '\n';
    return kFailure;
  }

  switch (parsed->type) {
  case CommandType::kPut:
    return HandlePut(*opened.value(), *parsed, output, error);
  case CommandType::kGet:
    return HandleGet(*opened.value(), *parsed, output, error);
  case CommandType::kDelete:
    return HandleDelete(*opened.value(), *parsed, output, error);
  case CommandType::kScan:
    return HandleScan(*opened.value(), *parsed, output, error);
  }

  error << "internal error: unhandled CLI command\n";
  return kFailure;
}

} // namespace tinylsm::cli
