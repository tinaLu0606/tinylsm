#include "lab/lab_http_server.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lab/lab_session.h"

namespace tinylsm::lab {
namespace {
using Json = nlohmann::json;
using Request = httplib::Request;
using Response = httplib::Response;

constexpr std::size_t kMaxRequestBytes = 8U * 1024U * 1024U;

const char* StatusCodeName(StatusCode code) {
  switch (code) {
  case StatusCode::kOk:
    return "OK";
  case StatusCode::kNotFound:
    return "NOT_FOUND";
  case StatusCode::kInvalidArgument:
    return "INVALID_ARGUMENT";
  case StatusCode::kIOError:
    return "IO_ERROR";
  case StatusCode::kCorruption:
    return "CORRUPTION";
  case StatusCode::kResourceExhausted:
    return "RESOURCE_EXHAUSTED";
  case StatusCode::kAlreadyClosed:
    return "CLOSED";
  case StatusCode::kNotSupported:
    return "NOT_SUPPORTED";
  }
  return "INTERNAL_ERROR";
}

Json StatusJson(const Status& status, std::string_view operation_id = {},
                std::string_view phase = {}) {
  Json out{{"code", StatusCodeName(status.code())}, {"message", status.message()}};
  if (!operation_id.empty())
    out["operationId"] = operation_id;
  if (!phase.empty())
    out["phase"] = phase;
  return out;
}

void SendJson(Response& response, const Json& body, int status = 200) {
  response.status = status;
  response.set_content(body.dump(), "application/json");
}

void SendStatusError(Response& response, const Status& status, int http_status,
                     std::string_view phase = {}) {
  SendJson(response, Json{{"status", StatusJson(status, {}, phase)}}, http_status);
}

void SendException(Response& response, std::string message) {
  response.status = 500;
  response.set_content(Json{{"status",
                             {{"code", "INTERNAL_ERROR"},
                              {"message", std::move(message)},
                              {"phase", "http"}}}}
                           .dump(),
                       "application/json");
}

std::optional<Json> ParseBody(const Request& request, Response& response) {
  if (request.body.size() > kMaxRequestBytes) {
    SendStatusError(response,
                    Status::ResourceExhausted("request body exceeds Lab limit"), 413,
                    "http.parse");
    return std::nullopt;
  }
  try {
    return Json::parse(request.body);
  } catch (const std::exception& error) {
    SendStatusError(response, Status::InvalidArgument(error.what()), 400, "http.parse");
    return std::nullopt;
  }
}

std::optional<std::string> Base64Decode(std::string_view input) {
  static constexpr std::array<std::int16_t, 256> table = [] {
    std::array<std::int16_t, 256> out{};
    out.fill(-1);
    for (int index = 0; index < 26; ++index) {
      out[static_cast<unsigned char>('A' + index)] = index;
      out[static_cast<unsigned char>('a' + index)] = index + 26;
    }
    for (int index = 0; index < 10; ++index)
      out[static_cast<unsigned char>('0' + index)] = index + 52;
    out[static_cast<unsigned char>('+')] = 62;
    out[static_cast<unsigned char>('/')] = 63;
    return out;
  }();
  if (input.size() % 4 != 0)
    return std::nullopt;
  std::string out;
  out.reserve(input.size() / 4 * 3);
  for (std::size_t offset = 0; offset < input.size(); offset += 4) {
    const char first = input[offset];
    const char second = input[offset + 1];
    const char third = input[offset + 2];
    const char fourth = input[offset + 3];
    if (first == '=' || second == '=')
      return std::nullopt;
    const auto a = table[static_cast<unsigned char>(first)];
    const auto b = table[static_cast<unsigned char>(second)];
    const auto c = third == '=' ? 0 : table[static_cast<unsigned char>(third)];
    const auto d = fourth == '=' ? 0 : table[static_cast<unsigned char>(fourth)];
    if (a < 0 || b < 0 || c < 0 || d < 0 || (third == '=' && fourth != '=') ||
        ((third == '=' || fourth == '=') && offset + 4 != input.size())) {
      return std::nullopt;
    }
    out.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (third != '=')
      out.push_back(static_cast<char>((b << 4) | (c >> 2)));
    if (fourth != '=')
      out.push_back(static_cast<char>((c << 6) | d));
  }
  return out;
}

std::string Base64Encode(std::string_view input) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const auto first = static_cast<unsigned char>(input[offset]);
    const bool has_second = offset + 1 < input.size();
    const bool has_third = offset + 2 < input.size();
    const auto second = has_second ? static_cast<unsigned char>(input[offset + 1]) : 0;
    const auto third = has_third ? static_cast<unsigned char>(input[offset + 2]) : 0;
    out.push_back(alphabet[first >> 2]);
    out.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
    out.push_back(has_second ? alphabet[((second & 0x0FU) << 2U) | (third >> 6U)]
                             : '=');
    out.push_back(has_third ? alphabet[third & 0x3FU] : '=');
  }
  return out;
}

std::string DisplayBytes(std::string_view bytes) {
  const bool printable =
      std::all_of(bytes.begin(), bytes.end(),
                  [](unsigned char value) { return value >= 32 && value != 127; });
  if (printable)
    return std::string(bytes);
  static constexpr char digits[] = "0123456789abcdef";
  std::string out{"0x"};
  out.reserve(2 + bytes.size() * 2);
  for (const unsigned char value : bytes) {
    out.push_back(digits[value >> 4]);
    out.push_back(digits[value & 0x0FU]);
  }
  return out;
}

Json EncodedJson(std::string_view bytes) {
  return {{"encoding", "base64"},
          {"data", Base64Encode(bytes)},
          {"byteLength", bytes.size()}};
}

Json ValueJson(const ValueEntry& value) {
  return {{"key", DisplayBytes(value.key)},
          {"value", DisplayBytes(value.value)},
          {"keyBase64", Base64Encode(value.key)},
          {"valueBase64", Base64Encode(value.value)}};
}

Json OptionsJson(const Options& options) {
  return {{"memtableBytes", options.memtable_bytes},
          {"createIfMissing", options.create_if_missing},
          {"syncOnWrite", options.sync_on_write},
          {"maxKeyBytes", options.max_key_bytes},
          {"maxValueBytes", options.max_value_bytes},
          {"sstableBlockBytes", options.sstable_block_bytes}};
}

Result<Options> ParseOptions(const Json& json) {
  try {
    Options options;
    options.memtable_bytes = json.at("memtableBytes").get<std::size_t>();
    options.create_if_missing = json.at("createIfMissing").get<bool>();
    options.sync_on_write = json.at("syncOnWrite").get<bool>();
    options.max_key_bytes = json.at("maxKeyBytes").get<std::uint32_t>();
    options.max_value_bytes = json.at("maxValueBytes").get<std::uint32_t>();
    options.sstable_block_bytes = json.at("sstableBlockBytes").get<std::size_t>();
    if (options.memtable_bytes == 0 || options.sstable_block_bytes == 0)
      return Status::InvalidArgument("size options must be positive");
    return options;
  } catch (const std::exception& error) {
    return Status::InvalidArgument(std::string("invalid session options: ") +
                                   error.what());
  }
}

Result<std::string> ParseBytes(const Json& json, std::string_view field,
                               bool required) {
  if (!json.contains(field)) {
    if (required)
      return Status::InvalidArgument(std::string("missing ") + std::string(field));
    return std::string();
  }
  try {
    const auto& value = json.at(field);
    if (value.at("encoding").get<std::string>() != "base64")
      return Status::InvalidArgument(std::string(field) + " must use base64");
    const auto decoded = Base64Decode(value.at("data").get<std::string>());
    if (!decoded)
      return Status::InvalidArgument(std::string(field) + " is not valid base64");
    return *decoded;
  } catch (const std::exception& error) {
    return Status::InvalidArgument(std::string("invalid ") + std::string(field) + ": " +
                                   error.what());
  }
}

Result<OperationRequest> ParseOperation(OperationKind kind, const Json& json) {
  OperationRequest out{.kind = kind};
  auto key = ParseBytes(json, "key",
                        kind == OperationKind::kPut || kind == OperationKind::kGet ||
                            kind == OperationKind::kDelete);
  if (!key.ok())
    return key.status();
  out.key = std::move(key.value());
  auto value = ParseBytes(json, "value", kind == OperationKind::kPut);
  if (!value.ok())
    return value.status();
  out.value = std::move(value.value());
  auto begin = ParseBytes(json, "begin", false);
  if (!begin.ok())
    return begin.status();
  out.begin = std::move(begin.value());
  auto end = ParseBytes(json, "end", false);
  if (!end.ok())
    return end.status();
  out.end = std::move(end.value());
  return out;
}

Json StateJson(const State& state) {
  Json tables = Json::array();
  for (const auto& table : state.tables) {
    std::ostringstream name;
    name << std::setw(6) << std::setfill('0') << table.file_number << ".sst";
    tables.push_back({{"fileNumber", table.file_number},
                      {"name", name.str()},
                      {"fileSize", table.file_size},
                      {"smallestKey", DisplayBytes(table.smallest_key)},
                      {"largestKey", DisplayBytes(table.largest_key)},
                      {"minSequence", table.min_sequence},
                      {"maxSequence", table.max_sequence}});
  }
  Json out{{"source", "live"},
           {"connection", state.open ? "open" : "closed"},
           {"path", state.path},
           {"options", OptionsJson(state.options)},
           {"memtableBytes", state.memtable_bytes},
           {"memtableEntries", state.memtable_entries},
           {"nextSequence", state.next_sequence},
           {"activeWal", state.active_wal},
           {"lastSequence", state.last_sequence},
           {"tables", std::move(tables)},
           {"directoryBytes", state.directory_bytes},
           {"pendingCleanup", state.pending_cleanup},
           {"features",
            {{"compact", true},
             {"storageInspection", true},
             {"resourceMetrics", false},
             {"recoveryExperiments", false}}}};
  if (state.opened_at)
    out["openedAt"] = *state.opened_at;
  if (state.terminal_error)
    out["terminalError"] = StatusJson(*state.terminal_error);
  return out;
}

Json RequestJson(const OperationRequest& request) {
  Json out{{"kind", OperationName(request.kind)}};
  if (request.kind == OperationKind::kPut || request.kind == OperationKind::kGet ||
      request.kind == OperationKind::kDelete)
    out["key"] = EncodedJson(request.key);
  if (request.kind == OperationKind::kPut)
    out["value"] = EncodedJson(request.value);
  if (request.kind == OperationKind::kScan) {
    out["begin"] = EncodedJson(request.begin);
    out["end"] = EncodedJson(request.end);
  }
  return out;
}

Json OperationJson(const OperationResult& result) {
  Json out{{"operationId", result.operation_id},
           {"request", RequestJson(result.request)},
           {"status", StatusJson(result.status, result.operation_id)},
           {"durationMicros", result.duration_micros},
           {"startedAt", result.started_at},
           {"before", StateJson(result.before)},
           {"after", StateJson(result.after)}};
  if (result.value)
    out["value"] = ValueJson(*result.value);
  if (!result.entries.empty() || result.request.kind == OperationKind::kScan) {
    out["entries"] = Json::array();
    for (const auto& entry : result.entries)
      out["entries"].push_back(ValueJson(entry));
  }
  return out;
}

Json EventJson(const Event& event) {
  Json out{{"id", std::to_string(event.id)}, {"operationId", event.operation_id},
           {"timestamp", event.timestamp},   {"level", event.level},
           {"phase", event.phase},           {"summary", event.summary}};
  if (event.file)
    out["file"] = *event.file;
  if (event.duration_micros)
    out["durationMicros"] = *event.duration_micros;
  return out;
}

Json MetricsJson(const Metrics& metrics) {
  return {{"source", "live"},
          {"measuredAt", ToIso8601(std::chrono::system_clock::now())},
          {"operationCounts",
           {{"put", metrics.puts},
            {"get", metrics.gets},
            {"delete", metrics.deletes},
            {"scan", metrics.scans},
            {"compact", metrics.compactions}}},
          {"errorCount", metrics.errors},
          {"flushCount", metrics.flushes},
          {"compactionCount", metrics.compact_count},
          {"averageFlushMicros",
           metrics.flushes == 0 ? 0 : metrics.total_flush_micros / metrics.flushes},
          {"averageCompactionMicros",
           metrics.compact_count == 0
               ? 0
               : metrics.total_compact_micros / metrics.compact_count},
          {"points", Json::array()},
          {"processMetricsAvailable", false}};
}

std::optional<std::uint64_t> EventCursor(const Request& request) {
  std::string value;
  if (request.has_header("Last-Event-ID"))
    value = request.get_header_value("Last-Event-ID");
  else if (request.has_param("after"))
    value = request.get_param_value("after");
  if (value.empty())
    return 0;
  std::uint64_t out = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), out);
  if (error != std::errc{} || end != value.data() + value.size())
    return std::nullopt;
  return out;
}

void HandleOperation(LabSession& session, OperationKind kind, const Request& request,
                     Response& response) {
  const auto body = ParseBody(request, response);
  if (!body)
    return;
  auto operation = ParseOperation(kind, *body);
  if (!operation.ok()) {
    SendStatusError(response, operation.status(), 400, "http.parse");
    return;
  }
  SendJson(response, OperationJson(session.Execute(std::move(operation.value()))));
}

} // namespace

LabHttpServer::LabHttpServer(LabSession& session,
                             std::filesystem::path static_directory)
    : session_(session), static_directory_(std::move(static_directory)),
      server_(std::make_unique<httplib::Server>()) {
  server_->set_payload_max_length(kMaxRequestBytes);
  server_->set_exception_handler(
      [](const auto&, auto& response, std::exception_ptr error) {
        try {
          std::rethrow_exception(error);
        } catch (const std::exception& exception) {
          SendException(response, exception.what());
        } catch (...) {
          SendException(response, "unknown C++ exception at HTTP boundary");
        }
      });

  server_->Post(
      "/api/session/open", [this](const Request& request, Response& response) {
        const auto body = ParseBody(request, response);
        if (!body)
          return;
        try {
          auto options = ParseOptions(body->at("options"));
          if (!options.ok()) {
            SendStatusError(response, options.status(), 400, "session.open");
            return;
          }
          const auto state =
              session_.Open(body->at("path").get<std::string>(), options.value());
          if (!state.ok()) {
            SendStatusError(response, state.status(), 422, "session.open");
            return;
          }
          SendJson(response, StateJson(state.value()));
        } catch (const std::exception& error) {
          SendStatusError(response, Status::InvalidArgument(error.what()), 400,
                          "session.open");
        }
      });
  server_->Post("/api/session/close", [this](const Request&, Response& response) {
    const auto state = session_.Close();
    if (!state.ok()) {
      SendStatusError(response, state.status(), 422, "session.close");
      return;
    }
    SendJson(response, StateJson(state.value()));
  });
  server_->Post("/api/session/reopen", [this](const Request&, Response& response) {
    const auto state = session_.Reopen();
    if (!state.ok()) {
      SendStatusError(response, state.status(), 422, "session.reopen");
      return;
    }
    SendJson(response, StateJson(state.value()));
  });
  server_->Get("/api/session/state", [this](const Request&, Response& response) {
    SendJson(response, StateJson(session_.GetState()));
  });

  server_->Post("/api/operations/put",
                [this](const Request& request, Response& response) {
                  HandleOperation(session_, OperationKind::kPut, request, response);
                });
  server_->Post("/api/operations/get",
                [this](const Request& request, Response& response) {
                  HandleOperation(session_, OperationKind::kGet, request, response);
                });
  server_->Post("/api/operations/delete",
                [this](const Request& request, Response& response) {
                  HandleOperation(session_, OperationKind::kDelete, request, response);
                });
  server_->Post("/api/operations/scan",
                [this](const Request& request, Response& response) {
                  HandleOperation(session_, OperationKind::kScan, request, response);
                });
  server_->Post("/api/operations/compact",
                [this](const Request& request, Response& response) {
                  HandleOperation(session_, OperationKind::kCompact, request, response);
                });
  server_->Get("/api/operations", [this](const Request&, Response& response) {
    Json operations = Json::array();
    for (const auto& operation : session_.GetOperations())
      operations.push_back(OperationJson(operation));
    SendJson(response, operations);
  });

  server_->Get("/api/storage/files", [this](const Request&, Response& response) {
    Json files = Json::array();
    std::uintmax_t total = 0;
    for (const auto& file : session_.GetStorageFiles()) {
      total += file.size;
      Json item{{"name", file.name},
                {"kind", file.kind},
                {"state", file.state},
                {"size", file.size},
                {"modifiedAt", file.modified_at},
                {"hexPreview", ""}};
      if (file.referenced_by)
        item["referencedBy"] = *file.referenced_by;
      files.push_back(std::move(item));
    }
    SendJson(response, {{"source", "live"},
                        {"files", std::move(files)},
                        {"totalBytes", total},
                        {"hasMore", false}});
  });
  server_->Get("/api/metrics", [this](const Request&, Response& response) {
    SendJson(response, MetricsJson(session_.GetMetrics()));
  });
  server_->Get("/api/events", [this](const Request&, Response& response) {
    Json events = Json::array();
    for (const auto& event : session_.GetEvents())
      events.push_back(EventJson(event));
    SendJson(response, events);
  });
  server_->Get(
      "/api/events/stream", [this](const Request& request, Response& response) {
        const auto cursor = EventCursor(request);
        if (!cursor) {
          SendStatusError(response, Status::InvalidArgument("invalid SSE event cursor"),
                          400, "events.stream");
          return;
        }
        std::ostringstream stream;
        stream << "retry: 500\n\n";
        for (const auto& event : session_.EventsAfter(*cursor)) {
          stream << "id: " << event.id << "\n"
                 << "event: lab-event\n"
                 << "data: " << EventJson(event).dump() << "\n\n";
        }
        response.set_header("Cache-Control", "no-cache");
        response.set_header("X-Accel-Buffering", "no");
        response.set_content(stream.str(), "text/event-stream");
      });

  if (!static_directory_.empty() && std::filesystem::is_directory(static_directory_))
    server_->set_mount_point("/", static_directory_.string());
}

LabHttpServer::~LabHttpServer() = default;

bool LabHttpServer::Listen(const std::string& host, int port) {
  if (host != "127.0.0.1" && host != "::1")
    return false;
  return server_->listen(host, port);
}

int LabHttpServer::BindLoopbackAnyPort() {
  return server_->bind_to_any_port("127.0.0.1");
}

bool LabHttpServer::ListenAfterBind() { return server_->listen_after_bind(); }

void LabHttpServer::Stop() { server_->stop(); }

} // namespace tinylsm::lab
