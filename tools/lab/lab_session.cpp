#include "lab/lab_session.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include "db/diagnostic_snapshot.h"

namespace tinylsm::lab {
namespace {

std::uintmax_t DirectoryBytes(const std::filesystem::path& path) {
  std::error_code error;
  std::uintmax_t bytes = 0;
  for (std::filesystem::recursive_directory_iterator
           it(path, std::filesystem::directory_options::skip_permission_denied, error),
       end;
       !error && it != end; it.increment(error)) {
    if (it->is_regular_file(error))
      bytes += it->file_size(error);
  }
  return bytes;
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
  AddEventLocked({}, "success", "session.open", "Opened live TinyLSM session");
  return SnapshotLocked();
}

Result<State> LabSession::Close() {
  std::scoped_lock lock(mutex_);
  if (!db_)
    return SnapshotLocked();
  const auto status = db_->Close();
  if (!status.ok() && status.code() != StatusCode::kAlreadyClosed)
    return status;
  db_.reset();
  opened_at_.reset();
  AddEventLocked({}, "info", "session.close", "Closed live TinyLSM session");
  return SnapshotLocked();
}

Result<State> LabSession::Reopen() {
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
  AddEventLocked({}, "success", "session.reopen",
                 "Reopened live TinyLSM session and recovered durable state");
  return SnapshotLocked();
}

State LabSession::GetState() const {
  std::scoped_lock lock(mutex_);
  return SnapshotLocked();
}

OperationResult LabSession::Execute(OperationRequest request) {
  std::scoped_lock lock(mutex_);
  const auto started = std::chrono::steady_clock::now();
  OperationResult result;
  result.operation_id = NextOperationIdLocked();
  result.request = std::move(request);
  result.started_at = ToIso8601(std::chrono::system_clock::now());
  result.before = SnapshotLocked();
  AddEventLocked(result.operation_id, "info", "operation.start",
                 std::string("Started ") + OperationName(result.request.kind));

  if (!db_) {
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
  if (result.status.ok()) {
    AddEventLocked(result.operation_id, "success", "operation.complete",
                   std::string("Completed ") + OperationName(result.request.kind),
                   std::nullopt, result.duration_micros);
    if (result.before.active_wal != result.after.active_wal ||
        result.before.tables.size() != result.after.tables.size()) {
      ++metrics_.flushes;
      AddEventLocked(result.operation_id, "info", "flush.publish",
                     "Published a new Manifest and active WAL");
    }
    if (result.request.kind == OperationKind::kCompact) {
      ++metrics_.compact_count;
      metrics_.total_compact_micros += result.duration_micros;
    }
  } else {
    AddEventLocked(result.operation_id, "error", "operation.error",
                   result.status.ToString(), std::nullopt, result.duration_micros);
  }
  operations_.push_front(result);
  if (operations_.size() > kMaxOperations)
    operations_.pop_back();
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
  state.directory_bytes = path_.empty() ? 0 : DirectoryBytes(path_);
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

std::string LabSession::NextOperationIdLocked() {
  return "op-" + std::to_string(next_operation_id_++);
}

} // namespace tinylsm::lab
