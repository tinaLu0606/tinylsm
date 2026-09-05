#include "lab/lab_session.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "db/diagnostic_snapshot.h"
#include "db/filename.h"
#include "util/bytewise_less.h"

namespace tinylsm::lab {
namespace {

struct DirectoryUsage {
  std::uintmax_t total = 0;
  std::uintmax_t manifest = 0;
  std::uintmax_t wal = 0;
  std::uintmax_t sstable = 0;
  std::uintmax_t temporary = 0;
};

DirectoryUsage DirectoryUsageFor(const std::filesystem::path& path) {
  std::error_code error;
  DirectoryUsage usage;
  for (std::filesystem::recursive_directory_iterator
           it(path, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       !error && it != end; it.increment(error)) {
    if (!it->is_regular_file(error))
      continue;
    const auto bytes = it->file_size(error);
    if (error)
      continue;
    usage.total += bytes;
    const auto name = it->path().filename().string();
    if (name == "MANIFEST")
      usage.manifest += bytes;
    else if (name.ends_with(".wal"))
      usage.wal += bytes;
    else if (name.ends_with(".sst"))
      usage.sstable += bytes;
    else
      usage.temporary += bytes;
  }
  return usage;
}

std::string JsonString(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (const unsigned char byte : value) {
    switch (byte) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (byte < 0x20) {
        out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<unsigned int>(byte) << std::dec;
      } else {
        out << static_cast<char>(byte);
      }
    }
  }
  out << '"';
  return out.str();
}

std::string FileKind(std::string_view name) {
  if (name == "MANIFEST")
    return "manifest";
  if (name.ends_with(".wal"))
    return "wal";
  if (name.ends_with(".sst"))
    return "sstable";
  return "temporary";
}

std::uint64_t ProcessRssBytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
    return 0;
  return info.resident_size;
#elif defined(__linux__)
  std::ifstream input("/proc/self/statm");
  std::uint64_t total = 0, resident = 0;
  if (!(input >> total >> resident))
    return 0;
  return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}

std::uint64_t Percentile(std::deque<std::uint64_t> samples, double percentile) {
  if (samples.empty())
    return 0;
  std::sort(samples.begin(), samples.end());
  const auto index = static_cast<std::size_t>(
      std::ceil(percentile * static_cast<double>(samples.size() - 1)));
  return samples[index];
}

} // namespace

std::string ToIso8601(std::chrono::system_clock::time_point time) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(time - seconds);
  const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &value);
#else
  gmtime_r(&value, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
      << std::setw(3) << millis.count() << 'Z';
  return out.str();
}

const char* OperationName(OperationKind kind) {
  switch (kind) {
  case OperationKind::kPut:
    return "put";
  case OperationKind::kGet:
    return "get";
  case OperationKind::kDelete:
    return "delete";
  case OperationKind::kScan:
    return "scan";
  case OperationKind::kCompact:
    return "compact";
  }
  return "unknown";
}

Result<State> LabSession::Open(std::filesystem::path path, Options options) {
  if (IsWorkloadActive())
    return Status::NotSupported(
        "stop the active workload before opening another session");
  std::scoped_lock lock(mutex_);
  if (path.empty())
    return Status::InvalidArgument("database path is empty");

  if (db_) {
    const auto status = db_->Close();
    if (!status.ok() && status.code() != StatusCode::kAlreadyClosed)
      return status.WithContext("close current Lab session");
    db_.reset();
  }

  auto opened = DB::Open(path, options);
  if (!opened.ok())
    return opened.status();
  db_ = std::move(opened.value());
  path_ = std::move(path);
  options_ = options;
  opened_at_ = ToIso8601(std::chrono::system_clock::now());
  event_log_.close();
  event_log_bytes_ = 0;
  next_event_log_number_ = 1;
  AddEventLocked({}, "success", "session.open", "Opened live TinyLSM session");
  return SnapshotLocked();
}

Result<State> LabSession::Close() {
  if (IsWorkloadActive())
    return Status::NotSupported("cancel or finish the active workload before closing");
  std::scoped_lock lock(mutex_);
  if (!db_)
    return SnapshotLocked();
  const auto status = db_->Close();
  if (!status.ok() && status.code() != StatusCode::kAlreadyClosed)
    return status;
  db_.reset();
  opened_at_.reset();
  AddEventLocked({}, "info", "session.close", "Closed live TinyLSM session");
  event_log_.close();
  return SnapshotLocked();
}

Result<State> LabSession::Reopen(bool workload_owned) {
  if (!workload_owned && IsWorkloadActive())
    return Status::NotSupported("pause or finish the active workload before reopening");
  std::scoped_lock lock(mutex_);
  if (path_.empty())
    return Status::InvalidArgument("open a database session before reopening it");
  if (db_) {
    const auto status = db_->Close();
    if (!status.ok() && status.code() != StatusCode::kAlreadyClosed)
      return status.WithContext("close before reopen");
    db_.reset();
  }

  auto opened = DB::Open(path_, options_);
  if (!opened.ok())
    return opened.status().WithContext("reopen database");
  db_ = std::move(opened.value());
  opened_at_ = ToIso8601(std::chrono::system_clock::now());
  if (!workload_owned) {
    AddEventLocked({}, "success", "session.reopen",
                   "Reopened live TinyLSM session and recovered durable state");
  }
  return SnapshotLocked();
}

State LabSession::GetState() const {
  std::scoped_lock lock(mutex_);
  return SnapshotLocked();
}

OperationResult LabSession::Execute(OperationRequest request, bool retain_detail,
                                    bool workload_owned) {
  std::scoped_lock lock(mutex_);
  const auto started = std::chrono::steady_clock::now();
  OperationResult result;
  result.operation_id = NextOperationIdLocked();
  result.request = std::move(request);
  result.started_at = ToIso8601(std::chrono::system_clock::now());
  result.before = SnapshotLocked();
  if (retain_detail) {
    AddEventLocked(result.operation_id, "info", "operation.start",
                   std::string("Started ") + OperationName(result.request.kind));
  }

  const bool write_operation = result.request.kind == OperationKind::kPut ||
                               result.request.kind == OperationKind::kDelete ||
                               result.request.kind == OperationKind::kCompact;
  if (write_operation && !workload_owned && IsWorkloadActive()) {
    result.status =
        Status::NotSupported("direct writes are disabled while a workload is active");
  } else if (!db_) {
    result.status = Status::AlreadyClosed("open a database session first");
  } else {
    switch (result.request.kind) {
    case OperationKind::kPut:
      result.status = db_->Put(result.request.key, result.request.value);
      break;
    case OperationKind::kGet: {
      auto value = db_->Get(result.request.key);
      result.status = value.ok() ? Status::Ok() : value.status();
      if (value.ok())
        result.value = ValueEntry{result.request.key, std::move(value.value())};
      break;
    }
    case OperationKind::kDelete:
      result.status = db_->Delete(result.request.key);
      break;
    case OperationKind::kScan: {
      auto entries = db_->Scan(result.request.begin, result.request.end);
      result.status = entries.ok() ? Status::Ok() : entries.status();
      if (entries.ok()) {
        result.entries.reserve(entries.value().size());
        for (auto& entry : entries.value())
          result.entries.push_back({std::move(entry.key), std::move(entry.value)});
      }
      break;
    }
    case OperationKind::kCompact:
      result.status = db_->Compact();
      break;
    }
  }

  result.after = SnapshotLocked();
  result.duration_micros =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count());
  CountLocked(result.request.kind, result.status.ok());
  RecordMetricLocked(result.after, result.duration_micros);
  if (result.status.ok()) {
    if (result.request.kind != OperationKind::kCompact &&
        (result.before.active_wal != result.after.active_wal ||
         result.before.tables.size() != result.after.tables.size())) {
      ++metrics_.flushes;
      metrics_.total_flush_micros += result.duration_micros;
      if (retain_detail) {
        AddEventLocked(result.operation_id, "info", "flush.publish",
                       "Published a new Manifest and active WAL");
      }
    }
    if (result.request.kind == OperationKind::kCompact) {
      ++metrics_.compact_count;
      metrics_.total_compact_micros += result.duration_micros;
    }
  }
  if (result.status.ok() && retain_detail) {
    AddEventLocked(result.operation_id, "success", "operation.complete",
                   std::string("Completed ") + OperationName(result.request.kind),
                   std::nullopt, result.duration_micros);
  } else if (!result.status.ok() && retain_detail) {
    AddEventLocked(result.operation_id, "error", "operation.error",
                   result.status.ToString(), std::nullopt, result.duration_micros);
  }
  if (retain_detail) {
    operations_.push_front(result);
    if (operations_.size() > kMaxOperations)
      operations_.pop_back();
  }
  return result;
}

std::vector<StorageFile> LabSession::GetStorageFiles() const {
  std::scoped_lock lock(mutex_);
  std::vector<StorageFile> files;
  if (path_.empty())
    return files;

  const State state = SnapshotLocked();
  std::error_code error;
  for (std::filesystem::directory_iterator it(path_, error), end; !error && it != end;
       it.increment(error)) {
    const auto name = it->path().filename().string();
    StorageFile file;
    file.name = name;
    file.kind = FileKind(name);
    file.size = it->is_regular_file(error) ? it->file_size(error) : 0;
    const auto modified = it->last_write_time(error);
    if (!error) {
      const auto system_time =
          std::chrono::time_point_cast<std::chrono::system_clock::duration>(
              modified - std::filesystem::file_time_type::clock::now() +
              std::chrono::system_clock::now());
      file.modified_at = ToIso8601(system_time);
    }
    file.state = "orphan";
    if (name == "MANIFEST")
      file.state = "live";
    std::ostringstream active_wal_name;
    active_wal_name << std::setw(6) << std::setfill('0') << state.active_wal << ".wal";
    if (file.kind == "wal" && name == active_wal_name.str()) {
      file.state = "active";
      file.referenced_by = "MANIFEST";
    }
    for (const auto& table : state.tables) {
      std::ostringstream expected;
      expected << std::setw(6) << std::setfill('0') << table.file_number << ".sst";
      if (name == expected.str()) {
        file.state = "live";
        file.referenced_by = "MANIFEST";
      }
    }
    if (name.ends_with(".tmp"))
      file.state = "temporary";
    files.push_back(std::move(file));
  }
  std::sort(files.begin(), files.end(),
            [](const auto& left, const auto& right) { return left.name < right.name; });
  return files;
}

Result<ManifestInspection> LabSession::InspectManifest() const {
  std::scoped_lock lock(mutex_);
  if (path_.empty())
    return Status::AlreadyClosed("open a database session before inspecting storage");
  return StorageInspector(path_).Manifest();
}

Result<PagedResult<WalRecordPageItem>> LabSession::InspectWal(std::string_view name,
                                                              std::uint64_t cursor,
                                                              std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  if (path_.empty())
    return Status::AlreadyClosed("open a database session before inspecting storage");
  return StorageInspector(path_).WalRecords(name, cursor, limit);
}

Result<PagedResult<SstableBlockPageItem>>
LabSession::InspectSstable(std::string_view name, std::uint64_t cursor,
                           std::size_t limit) const {
  std::scoped_lock lock(mutex_);
  if (path_.empty())
    return Status::AlreadyClosed("open a database session before inspecting storage");
  return StorageInspector(path_).SstableBlocks(name, cursor, limit);
}

Result<std::string> LabSession::InspectFileBytes(std::string_view name,
                                                 std::uint64_t offset,
                                                 std::size_t length) const {
  std::scoped_lock lock(mutex_);
  if (path_.empty())
    return Status::AlreadyClosed("open a database session before inspecting storage");
  return StorageInspector(path_).FileBytes(name, offset, length);
}

Result<RecoverySource>
LabSession::CopyDatabaseToSandbox(const std::filesystem::path& sandbox_path) const {
  std::scoped_lock lock(mutex_);
  if (!db_ || path_.empty())
    return Status::AlreadyClosed(
        "open a database session before creating a recovery sandbox");
  if (sandbox_path.empty() || std::filesystem::exists(sandbox_path))
    return Status::InvalidArgument("recovery sandbox path must be a new directory");

  std::error_code error;
  std::filesystem::create_directories(sandbox_path, error);
  if (error)
    return Status::IOError("create recovery sandbox: " + error.message());
  for (std::filesystem::directory_iterator it(path_, error), end; !error && it != end;
       it.increment(error)) {
    const auto status = it->symlink_status(error);
    if (error)
      break;
    if (std::filesystem::is_symlink(status))
      return Status::InvalidArgument("recovery source contains a symbolic link");
    if (!std::filesystem::is_regular_file(status))
      continue;
    const auto name = it->path().filename().string();
    if (name != "MANIFEST" && !internal::ParseNumberedFileName(name))
      continue;
    std::filesystem::copy_file(it->path(), sandbox_path / name,
                               std::filesystem::copy_options::none, error);
    if (error)
      return Status::IOError("copy recovery database file: " + error.message());
  }
  if (error)
    return Status::IOError("list recovery database files: " + error.message());
  return RecoverySource{options_, path_};
}

void LabSession::AddAuditEvent(std::string level, std::string phase,
                               std::string summary, std::optional<std::string> file) {
  std::scoped_lock lock(mutex_);
  AddEventLocked({}, std::move(level), std::move(phase), std::move(summary),
                 std::move(file));
}

Metrics LabSession::GetMetrics() const {
  std::scoped_lock lock(mutex_);
  return metrics_;
}

std::vector<OperationResult> LabSession::GetOperations() const {
  std::scoped_lock lock(mutex_);
  return {operations_.begin(), operations_.end()};
}

std::vector<Event> LabSession::GetEvents() const {
  std::scoped_lock lock(mutex_);
  return {events_.rbegin(), events_.rend()};
}

std::vector<Event> LabSession::EventsAfter(std::uint64_t id) const {
  std::scoped_lock lock(mutex_);
  std::vector<Event> out;
  for (const auto& event : events_) {
    if (event.id > id)
      out.push_back(event);
  }
  return out;
}

void LabSession::WaitForEventsAfter(std::uint64_t id,
                                    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  events_changed_.wait_for(lock, timeout,
                           [&] { return !events_.empty() && events_.back().id > id; });
}

State LabSession::SnapshotLocked() const {
  State state;
  state.path = path_.string();
  state.options = options_;
  state.open = db_ != nullptr;
  state.opened_at = opened_at_;
  state.directory_bytes = path_.empty() ? 0 : DirectoryUsageFor(path_).total;
  if (!db_)
    return state;

  auto diagnostic = internal::DBLabPeer::Snapshot(*db_);
  if (!diagnostic.ok()) {
    state.terminal_error = diagnostic.status();
    return state;
  }
  const auto& snapshot = diagnostic.value();
  state.options = snapshot.options;
  state.memtable_bytes = snapshot.memtable_bytes;
  state.memtable_entries = snapshot.memtable_entries;
  state.next_sequence = snapshot.next_sequence;
  state.active_wal = snapshot.active_wal_number;
  state.last_sequence = snapshot.last_sequence;
  state.pending_cleanup = snapshot.pending_cleanup;
  state.terminal_error = snapshot.terminal_error;
  state.tables.reserve(snapshot.live_tables.size());
  for (const auto& table : snapshot.live_tables) {
    state.tables.push_back({table.file_number, table.file_size, table.smallest_key,
                            table.largest_key, table.min_sequence, table.max_sequence});
  }
  return state;
}

void LabSession::AddEventLocked(std::string operation_id, std::string level,
                                std::string phase, std::string summary,
                                std::optional<std::string> file,
                                std::optional<std::uint64_t> duration) {
  events_.push_back({next_event_id_++, std::move(operation_id),
                     ToIso8601(std::chrono::system_clock::now()), std::move(level),
                     std::move(phase), std::move(summary), std::move(file), duration});
  if (events_.size() > kMaxEvents)
    events_.pop_front();
  AppendEventLogLocked(events_.back());
  events_changed_.notify_all();
}

void LabSession::CountLocked(OperationKind kind, bool success) {
  switch (kind) {
  case OperationKind::kPut:
    ++metrics_.puts;
    break;
  case OperationKind::kGet:
    ++metrics_.gets;
    break;
  case OperationKind::kDelete:
    ++metrics_.deletes;
    break;
  case OperationKind::kScan:
    ++metrics_.scans;
    break;
  case OperationKind::kCompact:
    ++metrics_.compactions;
    break;
  }
  if (!success)
    ++metrics_.errors;
}

void LabSession::RecordMetricLocked(const State& state, std::uint64_t latency_micros) {
  constexpr std::size_t kMaxLatencySamples = 4'096;
  constexpr std::size_t kMaxMetricPoints = 600;
  metrics_.recent_latencies.push_back(latency_micros);
  if (metrics_.recent_latencies.size() > kMaxLatencySamples)
    metrics_.recent_latencies.pop_front();

  const auto now = std::chrono::steady_clock::now();
  if (!metrics_.points.empty() &&
      now - metrics_.previous_wall < std::chrono::seconds(1)) {
    return;
  }
  const auto cpu = std::clock();
  const auto operations = metrics_.puts + metrics_.gets + metrics_.deletes +
                          metrics_.scans + metrics_.compactions;
  double cpu_percent = 0;
  std::uint64_t operations_per_second = 0;
  if (metrics_.previous_wall.time_since_epoch().count() != 0) {
    const auto elapsed =
        std::chrono::duration<double>(now - metrics_.previous_wall).count();
    if (elapsed > 0) {
      cpu_percent = 100.0 * static_cast<double>(cpu - metrics_.previous_cpu) /
                    (static_cast<double>(CLOCKS_PER_SEC) * elapsed);
      operations_per_second = static_cast<std::uint64_t>(
          static_cast<double>(operations - metrics_.previous_operations) / elapsed);
    }
  }
  metrics_.previous_wall = now;
  metrics_.previous_cpu = cpu;
  metrics_.previous_operations = operations;
  const auto usage = path_.empty() ? DirectoryUsage{} : DirectoryUsageFor(path_);
  metrics_.points.push_back(
      {ToIso8601(std::chrono::system_clock::now()), operations_per_second,
       Percentile(metrics_.recent_latencies, 0.50),
       Percentile(metrics_.recent_latencies, 0.95),
       Percentile(metrics_.recent_latencies, 0.99), ProcessRssBytes(), cpu_percent,
       usage.total, usage.manifest, usage.wal, usage.sstable, usage.temporary,
       state.memtable_bytes});
  if (metrics_.points.size() > kMaxMetricPoints)
    metrics_.points.pop_front();
}

void LabSession::AppendEventLogLocked(const Event& event) {
  if (path_.empty())
    return;
  if (!event_log_.is_open() || event_log_bytes_ >= kMaxJsonlLogBytes)
    OpenNextEventLogLocked();
  if (!event_log_)
    return;
  std::ostringstream line;
  line << "{\"id\":" << event.id << ",\"timestamp\":" << JsonString(event.timestamp)
       << ",\"operationId\":" << JsonString(event.operation_id)
       << ",\"level\":" << JsonString(event.level)
       << ",\"phase\":" << JsonString(event.phase)
       << ",\"summary\":" << JsonString(event.summary);
  if (event.file)
    line << ",\"file\":" << JsonString(*event.file);
  if (event.duration_micros)
    line << ",\"durationMicros\":" << *event.duration_micros;
  line << "}\n";
  const std::string encoded = line.str();
  event_log_ << encoded;
  event_log_.flush();
  if (event_log_)
    event_log_bytes_ += encoded.size();
}

void LabSession::OpenNextEventLogLocked() {
  event_log_.close();
  std::error_code error;
  std::filesystem::create_directories(path_, error);
  if (error)
    return;
  for (;
       std::filesystem::exists(
           path_ / ("lab-events-" + std::to_string(next_event_log_number_) + ".jsonl"),
           error) &&
       !error;
       ++next_event_log_number_) {
  }
  const auto log_path =
      path_ / ("lab-events-" + std::to_string(next_event_log_number_++) + ".jsonl");
  event_log_.open(log_path, std::ios::app);
  if (!event_log_)
    return;
  event_log_bytes_ = std::filesystem::file_size(log_path, error);
  if (error)
    event_log_bytes_ = 0;
}

bool LabSession::IsWorkloadActive() const {
  std::scoped_lock lock(workload_mutex_);
  return workload_.status == WorkloadStatus::kRunning ||
         workload_.status == WorkloadStatus::kPaused;
}

Result<State> LabSession::ReopenForWorkload() { return Reopen(true); }

std::string LabSession::NextOperationIdLocked() {
  return "op-" + std::to_string(next_operation_id_++);
}

LabSession::~LabSession() { StopWorkload(); }

Result<WorkloadRun> LabSession::StartWorkload(WorkloadConfig config) {
  if (config.operation_count == 0 || config.key_space == 0 ||
      config.operations_per_second == 0 ||
      config.put_ratio + config.get_ratio + config.delete_ratio != 100) {
    return Status::InvalidArgument("workload configuration is invalid");
  }
  if (!GetState().open)
    return Status::AlreadyClosed("open a database before starting a workload");

  std::unique_lock lock(workload_mutex_);
  if (workload_.status == WorkloadStatus::kRunning ||
      workload_.status == WorkloadStatus::kPaused) {
    return Status::NotSupported("a workload is already active");
  }
  if (workload_thread_.joinable()) {
    lock.unlock();
    workload_thread_.join();
    lock.lock();
  }
  const std::string id = "workload-" + std::to_string(next_workload_id_++);
  const bool performance_mode = config.operations_per_second >= 500;
  workload_ = {id,
               WorkloadStatus::kRunning,
               config,
               0,
               ToIso8601(std::chrono::system_clock::now()),
               std::nullopt,
               std::nullopt,
               performance_mode};
  workload_cancel_requested_ = false;
  workload_thread_ = std::thread(
      [this, config, performance_mode] { RunWorkload(config, !performance_mode); });
  return workload_;
}

WorkloadRun LabSession::GetWorkload() const {
  std::scoped_lock lock(workload_mutex_);
  return workload_;
}

Result<WorkloadRun> LabSession::PauseWorkload() {
  std::scoped_lock lock(workload_mutex_);
  if (workload_.status != WorkloadStatus::kRunning)
    return Status::NotSupported("workload is not running");
  workload_.status = WorkloadStatus::kPaused;
  workload_changed_.notify_all();
  return workload_;
}

Result<WorkloadRun> LabSession::ResumeWorkload() {
  std::scoped_lock lock(workload_mutex_);
  if (workload_.status != WorkloadStatus::kPaused)
    return Status::NotSupported("workload is not paused");
  workload_.status = WorkloadStatus::kRunning;
  workload_changed_.notify_all();
  return workload_;
}

Result<WorkloadRun> LabSession::CancelWorkload() {
  std::scoped_lock lock(workload_mutex_);
  if (workload_.status != WorkloadStatus::kRunning &&
      workload_.status != WorkloadStatus::kPaused) {
    return Status::NotSupported("workload is not active");
  }
  workload_cancel_requested_ = true;
  workload_changed_.notify_all();
  return workload_;
}

void LabSession::StopWorkload() {
  {
    std::scoped_lock lock(workload_mutex_);
    workload_cancel_requested_ = true;
    workload_changed_.notify_all();
  }
  if (workload_thread_.joinable())
    workload_thread_.join();
}

void LabSession::RunWorkload(WorkloadConfig config, bool retain_detail) {
  std::map<std::string, std::string, internal::BytewiseLess> reference;
  std::optional<WorkloadMismatch> initial_failure;
  {
    std::scoped_lock lock(mutex_);
    if (!db_) {
      initial_failure = {0, "<initial-scan>", "open database", "database is closed"};
    } else {
      const auto initial = db_->Scan({}, {});
      if (!initial.ok()) {
        initial_failure = {0, "<initial-scan>", "readable database",
                           initial.status().ToString()};
      } else {
        for (const auto& entry : initial.value())
          reference[entry.key] = entry.value;
      }
    }
  }
  if (initial_failure) {
    {
      std::scoped_lock lock(workload_mutex_);
      workload_.status = WorkloadStatus::kFailed;
      workload_.mismatch = std::move(initial_failure);
      workload_.finished_at = ToIso8601(std::chrono::system_clock::now());
    }
    std::scoped_lock lock(mutex_);
    AddEventLocked({}, "error", "workload.mismatch",
                   "Could not initialize the reference model from TinyLSM");
    return;
  }
  std::uint64_t random = config.seed == 0 ? 1 : config.seed;
  const auto next_random = [&random] {
    random = random * 6364136223846793005ULL + 1442695040888963407ULL;
    return random;
  };
  const auto key_for = [&](std::uint64_t operation) {
    std::uint64_t index = operation % config.key_space;
    if (config.distribution == WorkloadDistribution::kUniform)
      index = next_random() % config.key_space;
    if (config.distribution == WorkloadDistribution::kHotspot) {
      const auto hot_keys = std::max<std::uint64_t>(1, config.key_space / 5);
      index = next_random() % 100 < 80 ? next_random() % hot_keys
                                       : next_random() % config.key_space;
    }
    std::ostringstream out;
    out << "key-" << std::setw(8) << std::setfill('0') << index;
    return out.str();
  };
  const auto value_for = [&](std::uint64_t operation) {
    std::string value = "value-" + std::to_string(operation);
    value.resize(config.value_bytes, static_cast<char>('a' + operation % 26));
    return value;
  };
  const auto interval =
      std::chrono::microseconds(1'000'000 / config.operations_per_second);
  auto deadline = std::chrono::steady_clock::now();

  for (std::uint64_t operation = 0; operation < config.operation_count; ++operation) {
    {
      std::unique_lock lock(workload_mutex_);
      workload_changed_.wait(lock, [&] {
        return workload_cancel_requested_ ||
               workload_.status != WorkloadStatus::kPaused;
      });
      if (workload_cancel_requested_)
        break;
    }
    const auto key = key_for(operation);
    const auto selector = next_random() % 100;
    OperationRequest request{.kind = OperationKind::kGet, .key = key};
    if (selector < config.put_ratio) {
      request.kind = OperationKind::kPut;
      request.value = value_for(operation);
      reference[key] = request.value;
    } else if (selector < config.put_ratio + config.get_ratio) {
      request.kind = OperationKind::kGet;
    } else {
      request.kind = OperationKind::kDelete;
      reference.erase(key);
    }
    const auto actual = Execute(request, retain_detail, true);
    std::optional<WorkloadMismatch> mismatch;
    if (request.kind == OperationKind::kGet) {
      const auto expected = reference.find(key);
      if (expected == reference.end() &&
          actual.status.code() != StatusCode::kNotFound) {
        mismatch = {operation, key, "not found", actual.status.ToString()};
      } else if (expected != reference.end() &&
                 (!actual.status.ok() || !actual.value ||
                  actual.value->value != expected->second)) {
        mismatch = {operation, key,
                    expected == reference.end() ? "not found" : expected->second,
                    actual.value ? actual.value->value : "not found"};
      }
    } else if (!actual.status.ok()) {
      mismatch = {operation, key, "successful operation", actual.status.ToString()};
    }
    if (!mismatch && config.reopen_every != 0 &&
        (operation + 1) % config.reopen_every == 0) {
      const auto reopened = ReopenForWorkload();
      if (!reopened.ok())
        mismatch = {operation, key, "successful reopen", reopened.status().ToString()};
    }
    {
      std::scoped_lock lock(workload_mutex_);
      workload_.completed_operations = operation + 1;
      if (mismatch) {
        workload_.mismatch = std::move(mismatch);
        workload_.status = WorkloadStatus::kFailed;
      }
    }
    if (mismatch) {
      std::scoped_lock lock(mutex_);
      AddEventLocked({}, "error", "workload.mismatch",
                     "Captured the first deterministic reference-model mismatch");
      break;
    }
    deadline += interval;
    std::unique_lock lock(workload_mutex_);
    workload_changed_.wait_until(lock, deadline, [&] {
      return workload_cancel_requested_ || workload_.status == WorkloadStatus::kPaused;
    });
    if (workload_cancel_requested_)
      break;
  }
  bool verify_reference = false;
  {
    std::scoped_lock lock(workload_mutex_);
    verify_reference =
        !workload_cancel_requested_ && workload_.status == WorkloadStatus::kRunning;
  }
  if (verify_reference) {
    const auto scan = Execute({.kind = OperationKind::kScan}, retain_detail, true);
    std::optional<WorkloadMismatch> mismatch;
    if (!scan.status.ok() || scan.entries.size() != reference.size()) {
      mismatch = {config.operation_count, "<full-scan>",
                  std::to_string(reference.size()) + " reference entries",
                  scan.status.ok()
                      ? std::to_string(scan.entries.size()) + " engine entries"
                      : scan.status.ToString()};
    } else {
      auto expected = reference.begin();
      for (const auto& entry : scan.entries) {
        if (entry.key != expected->first || entry.value != expected->second) {
          mismatch = {config.operation_count, entry.key, expected->second, entry.value};
          break;
        }
        ++expected;
      }
    }
    if (mismatch) {
      {
        std::scoped_lock lock(workload_mutex_);
        workload_.mismatch = std::move(mismatch);
        workload_.status = WorkloadStatus::kFailed;
      }
      std::scoped_lock lock(mutex_);
      AddEventLocked({}, "error", "workload.mismatch",
                     "Final full Scan differs from the reference model");
    }
  }
  std::scoped_lock lock(workload_mutex_);
  if (workload_.status == WorkloadStatus::kRunning ||
      workload_.status == WorkloadStatus::kPaused) {
    workload_.status = workload_cancel_requested_ ? WorkloadStatus::kCancelled
                                                  : WorkloadStatus::kCompleted;
  }
  workload_.finished_at = ToIso8601(std::chrono::system_clock::now());
  workload_changed_.notify_all();
}

} // namespace tinylsm::lab
