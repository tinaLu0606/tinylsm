#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lab/storage_inspector.h"
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
  std::deque<std::uint64_t> recent_latencies;
  struct Point {
    std::string timestamp;
    std::uint64_t operations_per_second = 0;
    std::uint64_t p50_micros = 0;
    std::uint64_t p95_micros = 0;
    std::uint64_t p99_micros = 0;
    std::uint64_t rss_bytes = 0;
    double cpu_percent = 0;
    std::uintmax_t directory_bytes = 0;
    std::uintmax_t manifest_bytes = 0;
    std::uintmax_t wal_bytes = 0;
    std::uintmax_t sstable_bytes = 0;
    std::uintmax_t temporary_bytes = 0;
    std::size_t memtable_bytes = 0;
  };
  std::deque<Point> points;
  std::clock_t previous_cpu = 0;
  std::chrono::steady_clock::time_point previous_wall{};
  std::uint64_t previous_operations = 0;
};

enum class WorkloadDistribution { kSequential, kUniform, kHotspot };
enum class WorkloadStatus { kIdle, kRunning, kPaused, kCompleted, kCancelled, kFailed };

struct WorkloadConfig {
  std::uint64_t operation_count = 0;
  std::uint64_t seed = 0;
  std::uint64_t key_space = 0;
  std::size_t value_bytes = 0;
  std::uint32_t put_ratio = 0;
  std::uint32_t get_ratio = 0;
  std::uint32_t delete_ratio = 0;
  std::uint32_t operations_per_second = 0;
  WorkloadDistribution distribution = WorkloadDistribution::kUniform;
  std::uint64_t reopen_every = 0;
};

struct WorkloadMismatch {
  std::uint64_t operation = 0;
  std::string key;
  std::string expected;
  std::string actual;
};

struct WorkloadRun {
  std::string id = "workload-idle";
  WorkloadStatus status = WorkloadStatus::kIdle;
  WorkloadConfig config;
  std::uint64_t completed_operations = 0;
  std::optional<std::string> started_at;
  std::optional<std::string> finished_at;
  std::optional<WorkloadMismatch> mismatch;
  bool performance_mode = false;
};

struct RecoverySource {
  Options options;
  std::filesystem::path source_path;
};

/// Owns one TinyLSM handle and serializes every command that touches it.
/// Snapshot and event copies never leak DB-owned references to HTTP threads.
class LabSession {
public:
  static constexpr std::size_t kMaxEvents = 10'000;
  static constexpr std::size_t kMaxOperations = 10'000;
  static constexpr std::size_t kMaxJsonlLogBytes = 64U * 1024U;
  ~LabSession();

  Result<State> Open(std::filesystem::path path, Options options);
  Result<State> Close();
  Result<State> Reopen(bool workload_owned = false);
  [[nodiscard]] State GetState() const;
  OperationResult Execute(OperationRequest request, bool retain_detail = true,
                          bool workload_owned = false);
  [[nodiscard]] std::vector<StorageFile> GetStorageFiles() const;
  Result<ManifestInspection> InspectManifest() const;
  Result<PagedResult<WalRecordPageItem>>
  InspectWal(std::string_view name, std::uint64_t cursor, std::size_t limit) const;
  Result<PagedResult<SstableBlockPageItem>>
  InspectSstable(std::string_view name, std::uint64_t cursor, std::size_t limit) const;
  Result<std::string> InspectFileBytes(std::string_view name, std::uint64_t offset,
                                       std::size_t length) const;
  Result<RecoverySource>
  CopyDatabaseToSandbox(const std::filesystem::path& sandbox_path) const;
  void AddAuditEvent(std::string level, std::string phase, std::string summary,
                     std::optional<std::string> file = std::nullopt);
  [[nodiscard]] Metrics GetMetrics() const;
  [[nodiscard]] std::vector<OperationResult> GetOperations() const;
  [[nodiscard]] std::vector<Event> GetEvents() const;
  [[nodiscard]] std::vector<Event> EventsAfter(std::uint64_t id) const;
  void WaitForEventsAfter(std::uint64_t id, std::chrono::milliseconds timeout) const;
  Result<WorkloadRun> StartWorkload(WorkloadConfig config);
  [[nodiscard]] WorkloadRun GetWorkload() const;
  Result<WorkloadRun> PauseWorkload();
  Result<WorkloadRun> ResumeWorkload();
  Result<WorkloadRun> CancelWorkload();

private:
  [[nodiscard]] State SnapshotLocked() const;
  void AddEventLocked(std::string operation_id, std::string level, std::string phase,
                      std::string summary,
                      std::optional<std::string> file = std::nullopt,
                      std::optional<std::uint64_t> duration = std::nullopt);
  void CountLocked(OperationKind kind, bool success);
  void RecordMetricLocked(const State& state, std::uint64_t latency_micros);
  void AppendEventLogLocked(const Event& event);
  void OpenNextEventLogLocked();
  [[nodiscard]] bool IsWorkloadActive() const;
  Result<State> ReopenForWorkload();
  void RunWorkload(WorkloadConfig config, bool retain_detail);
  void StopWorkload();
  [[nodiscard]] std::string NextOperationIdLocked();

  mutable std::mutex mutex_;
  mutable std::condition_variable events_changed_;
  std::unique_ptr<DB> db_;
  std::filesystem::path path_;
  Options options_;
  std::optional<std::string> opened_at_;
  std::deque<OperationResult> operations_;
  std::deque<Event> events_;
  std::ofstream event_log_;
  std::uintmax_t event_log_bytes_ = 0;
  std::uint64_t next_event_log_number_ = 1;
  Metrics metrics_;
  std::uint64_t next_operation_id_ = 1;
  std::uint64_t next_event_id_ = 1;
  mutable std::mutex workload_mutex_;
  std::condition_variable workload_changed_;
  std::thread workload_thread_;
  WorkloadRun workload_;
  bool workload_cancel_requested_ = false;
  std::uint64_t next_workload_id_ = 1;
};

[[nodiscard]] std::string ToIso8601(std::chrono::system_clock::time_point time);
[[nodiscard]] const char* OperationName(OperationKind kind);

} // namespace tinylsm::lab
