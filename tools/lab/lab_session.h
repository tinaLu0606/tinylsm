#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "tinylsm/db.h"
#include "tinylsm/options.h"
#include "tinylsm/result.h"
#include "tinylsm/status.h"

namespace tinylsm::lab {

enum class OperationKind { kPut, kGet, kDelete, kScan, kCompact };

struct OperationRequest {
  OperationKind kind;
  std::string key;
  std::string value;
  std::string begin;
  std::string end;
};

struct ValueEntry {
  std::string key;
  std::string value;
};

struct TableInfo {
  std::uint64_t file_number = 0;
  std::uint64_t file_size = 0;
  std::string smallest_key;
  std::string largest_key;
  std::uint64_t min_sequence = 0;
  std::uint64_t max_sequence = 0;
};

struct State {
  std::string path;
  Options options;
  bool open = false;
  std::optional<std::string> opened_at;
  std::size_t memtable_bytes = 0;
  std::size_t memtable_entries = 0;
  std::uint64_t next_sequence = 1;
  std::uint64_t active_wal = 0;
  std::uint64_t last_sequence = 0;
  std::vector<TableInfo> tables;
  std::uintmax_t directory_bytes = 0;
  std::size_t pending_cleanup = 0;
  std::optional<Status> terminal_error;
};

struct OperationResult {
  std::string operation_id;
  OperationRequest request;
  Status status;
  std::uint64_t duration_micros = 0;
  std::string started_at;
  std::optional<ValueEntry> value;
  std::vector<ValueEntry> entries;
  State before;
  State after;
};

struct Event {
  std::uint64_t id = 0;
  std::string operation_id;
  std::string timestamp;
  std::string level;
  std::string phase;
  std::string summary;
  std::optional<std::string> file;
  std::optional<std::uint64_t> duration_micros;
};

struct StorageFile {
  std::string name;
  std::string kind;
  std::string state;
  std::uintmax_t size = 0;
  std::string modified_at;
  std::optional<std::string> referenced_by;
};

struct Metrics {
  std::uint64_t puts = 0;
  std::uint64_t gets = 0;
  std::uint64_t deletes = 0;
  std::uint64_t scans = 0;
  std::uint64_t compactions = 0;
  std::uint64_t errors = 0;
  std::uint64_t flushes = 0;
  std::uint64_t compact_count = 0;
  std::uint64_t total_flush_micros = 0;
  std::uint64_t total_compact_micros = 0;
};

/// Owns one TinyLSM handle and serializes every command that touches it.
/// Snapshot and event copies never leak DB-owned references to HTTP threads.
class LabSession {
public:
  static constexpr std::size_t kMaxEvents = 10'000;
  static constexpr std::size_t kMaxOperations = 10'000;

  Result<State> Open(std::filesystem::path path, Options options);
  Result<State> Close();
  Result<State> Reopen();
  [[nodiscard]] State GetState() const;
  OperationResult Execute(OperationRequest request);
  [[nodiscard]] std::vector<StorageFile> GetStorageFiles() const;
  [[nodiscard]] Metrics GetMetrics() const;
  [[nodiscard]] std::vector<OperationResult> GetOperations() const;
  [[nodiscard]] std::vector<Event> GetEvents() const;
  [[nodiscard]] std::vector<Event> EventsAfter(std::uint64_t id) const;
  void WaitForEventsAfter(std::uint64_t id, std::chrono::milliseconds timeout) const;

private:
  [[nodiscard]] State SnapshotLocked() const;
  void AddEventLocked(std::string operation_id, std::string level, std::string phase,
                      std::string summary,
                      std::optional<std::string> file = std::nullopt,
                      std::optional<std::uint64_t> duration = std::nullopt);
  void CountLocked(OperationKind kind, bool success);
  [[nodiscard]] std::string NextOperationIdLocked();

  mutable std::mutex mutex_;
  mutable std::condition_variable events_changed_;
  std::unique_ptr<DB> db_;
  std::filesystem::path path_;
  Options options_;
  std::optional<std::string> opened_at_;
  std::deque<OperationResult> operations_;
  std::deque<Event> events_;
  Metrics metrics_;
  std::uint64_t next_operation_id_ = 1;
  std::uint64_t next_event_id_ = 1;
};

[[nodiscard]] std::string ToIso8601(std::chrono::system_clock::time_point time);
[[nodiscard]] const char* OperationName(OperationKind kind);

} // namespace tinylsm::lab
