#include "lab/recovery_lab.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "lab/storage_inspector.h"

namespace tinylsm::lab {
namespace {

constexpr std::string_view kWorkerKey = "__tinylsm_recovery_worker__";
constexpr std::string_view kWorkerValue = "durable-without-close";
constexpr std::string_view kPrefixKey = "__tinylsm_recovery_prefix__";
constexpr std::string_view kPrefixValue = "survives-truncated-tail";

const std::array<RecoveryScenario, 3> kScenarios{{
    {RecoveryScenarioId::kUncleanShutdown, "Unclean shutdown",
     "A dedicated worker writes a synced record and exits without DB::Close().",
     "The worker record is recovered from the active WAL.",
     "Terminate only the isolated worker process; the Lab Server remains running."},
    {RecoveryScenarioId::kTruncatedWal, "Truncated WAL tail",
     "The sandbox active WAL loses the final byte after two synced records.",
     "Recovery keeps the valid prefix and discards the incomplete tail.",
     "Resize only the sandbox active WAL by one byte."},
    {RecoveryScenarioId::kCrcCorruption, "Manifest CRC corruption",
     "One controlled byte in the sandbox MANIFEST is flipped.",
     "Opening the sandbox reports Corruption instead of accepting altered metadata.",
     "Flip one byte only in the sandbox MANIFEST."},
}};

const RecoveryScenario* FindScenario(RecoveryScenarioId id) {
  const auto it =
      std::find_if(kScenarios.begin(), kScenarios.end(),
                   [id](const auto& scenario) { return scenario.id == id; });
  return it == kScenarios.end() ? nullptr : &*it;
}

std::filesystem::path ActiveWalPath(const std::filesystem::path& sandbox) {
  const auto manifest = StorageInspector(sandbox).Manifest();
  if (!manifest.ok())
    return {};
  std::ostringstream name;
  name << std::setw(6) << std::setfill('0') << manifest.value().active_wal << ".wal";
  return sandbox / name.str();
}

} // namespace

std::optional<RecoveryScenarioId> ParseRecoveryScenarioId(std::string_view value) {
  if (value == "unclean-shutdown")
    return RecoveryScenarioId::kUncleanShutdown;
  if (value == "truncated-wal")
    return RecoveryScenarioId::kTruncatedWal;
  if (value == "crc-corruption")
    return RecoveryScenarioId::kCrcCorruption;
  return std::nullopt;
}

const char* RecoveryScenarioName(RecoveryScenarioId id) {
  switch (id) {
  case RecoveryScenarioId::kUncleanShutdown:
    return "unclean-shutdown";
  case RecoveryScenarioId::kTruncatedWal:
    return "truncated-wal";
  case RecoveryScenarioId::kCrcCorruption:
    return "crc-corruption";
  }
  return "unknown";
}

const char* RecoveryRunStatusName(RecoveryRunStatus status) {
  switch (status) {
  case RecoveryRunStatus::kPreview:
    return "preview";
  case RecoveryRunStatus::kRunning:
    return "running";
  case RecoveryRunStatus::kPassed:
    return "passed";
  case RecoveryRunStatus::kFailed:
    return "failed";
  }
  return "failed";
}

RecoveryLab::RecoveryLab(LabSession& session, std::filesystem::path worker_path)
    : session_(session), worker_path_(std::move(worker_path)) {
#if defined(__unix__) || defined(__APPLE__)
  sandbox_root_ = std::filesystem::temp_directory_path() /
                  ("tinylsm-lab-recovery-" + std::to_string(::getpid()));
#else
  sandbox_root_ = std::filesystem::temp_directory_path() / "tinylsm-lab-recovery";
#endif
}

RecoveryLab::~RecoveryLab() {
  std::error_code error;
  if (!sandbox_root_.empty())
    std::filesystem::remove_all(sandbox_root_, error);
}

std::vector<RecoveryScenario> RecoveryLab::Scenarios() const {
  return {kScenarios.begin(), kScenarios.end()};
}

Result<std::filesystem::path> RecoveryLab::CreateSandbox() {
  std::error_code error;
  std::filesystem::create_directories(sandbox_root_, error);
  if (error)
    return Status::IOError("create recovery sandbox root: " + error.message());
  const auto sandbox = sandbox_root_ / ("run-" + std::to_string(next_run_id_));
  if (std::filesystem::exists(sandbox))
    return Status::IOError("recovery sandbox already exists");
  return sandbox;
}

bool RecoveryLab::IsSandboxPath(const std::filesystem::path& path) const {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(sandbox_root_, error);
  if (error)
    return false;
  const auto candidate = std::filesystem::weakly_canonical(path, error);
  if (error || !std::filesystem::is_directory(candidate, error) || error)
    return false;
  const auto mismatch =
      std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end());
  return mismatch.first == root.end() && candidate != root;
}

void RecoveryLab::AddEvent(RecoveryRun& run, std::string level, std::string phase,
                           std::string summary) const {
  run.events.push_back({static_cast<std::uint64_t>(run.events.size() + 1), run.id,
                        ToIso8601(std::chrono::system_clock::now()), std::move(level),
                        std::move(phase), std::move(summary), std::nullopt,
                        std::nullopt});
}

Result<RecoveryRun> RecoveryLab::Preview(RecoveryScenarioId scenario_id) {
  const auto* scenario = FindScenario(scenario_id);
  if (!scenario)
    return Status::InvalidArgument("unknown recovery scenario");

  std::scoped_lock lock(mutex_);
  auto sandbox = CreateSandbox();
  if (!sandbox.ok())
    return sandbox.status();
  const auto source = session_.CopyDatabaseToSandbox(sandbox.value());
  if (!source.ok())
    return source.status();

  RecoveryRun run{"recovery-" + std::to_string(next_run_id_++),
                  scenario_id,
                  RecoveryRunStatus::kPreview,
                  std::move(sandbox.value()),
                  scenario->expected_outcome,
                  std::nullopt,
                  ToIso8601(std::chrono::system_clock::now()),
                  {}};
  AddEvent(run, "info", "recovery.preview", "Created a server-owned sandbox copy");
  session_.AddAuditEvent("info", "recovery.preview",
                         "Created a sandbox recovery preview",
                         run.sandbox_path.string());
  runs_.push_back(run);
  return run;
}

Result<RecoveryRun> RecoveryLab::Get(std::string_view id) const {
  std::scoped_lock lock(mutex_);
  const auto it = std::find_if(runs_.begin(), runs_.end(),
                               [id](const auto& run) { return run.id == id; });
  if (it == runs_.end())
    return Status::InvalidArgument("unknown recovery experiment");
  return *it;
}

std::vector<RecoveryRun> RecoveryLab::Runs() const {
  std::scoped_lock lock(mutex_);
  return runs_;
}

Result<RecoveryRun> RecoveryLab::Run(std::string_view id) {
  std::scoped_lock lock(mutex_);
  const auto it = std::find_if(runs_.begin(), runs_.end(),
                               [id](const auto& run) { return run.id == id; });
  if (it == runs_.end())
    return Status::InvalidArgument("unknown recovery experiment");
  RecoveryRun& run = *it;
  if (run.status != RecoveryRunStatus::kPreview)
    return Status::NotSupported("recovery experiment has already run");
  if (!IsSandboxPath(run.sandbox_path))
    return Status::InvalidArgument("recovery target is outside the sandbox root");

  const auto source = session_.GetState();
  if (!source.open)
    return Status::AlreadyClosed("open a database session before running recovery");
  run.status = RecoveryRunStatus::kRunning;
  AddEvent(run, "warning", "recovery.run", "Applying the previewed sandbox mutation");
  session_.AddAuditEvent("warning", "recovery.run",
                         "Running a previewed sandbox recovery experiment",
                         run.sandbox_path.string());

  Result<std::string> outcome = Status::NotSupported("unknown recovery scenario");
  switch (run.scenario_id) {
  case RecoveryScenarioId::kUncleanShutdown:
    outcome = RunUncleanShutdown(run, source.options);
    break;
  case RecoveryScenarioId::kTruncatedWal:
    outcome = RunTruncatedWal(run, source.options);
    break;
  case RecoveryScenarioId::kCrcCorruption:
    outcome = RunCrcCorruption(run, source.options);
    break;
  }
  if (!outcome.ok()) {
    run.status = RecoveryRunStatus::kFailed;
    run.actual_outcome = outcome.status().ToString();
    AddEvent(run, "error", "recovery.failed", *run.actual_outcome);
    session_.AddAuditEvent("error", "recovery.failed", *run.actual_outcome,
                           run.sandbox_path.string());
    return run;
  }
  run.status = RecoveryRunStatus::kPassed;
  run.actual_outcome = std::move(outcome.value());
  AddEvent(run, "success", "recovery.passed", *run.actual_outcome);
  session_.AddAuditEvent("success", "recovery.passed", *run.actual_outcome,
                         run.sandbox_path.string());
  return run;
}

Result<std::vector<RecoveryRun>> RecoveryLab::Reset() {
  std::scoped_lock lock(mutex_);
  for (const auto& run : runs_) {
    if (!IsSandboxPath(run.sandbox_path))
      return Status::InvalidArgument("refusing to reset a non-sandbox path");
  }
  std::error_code error;
  std::filesystem::remove_all(sandbox_root_, error);
  if (error)
    return Status::IOError("reset recovery sandboxes: " + error.message());
  runs_.clear();
  session_.AddAuditEvent("info", "recovery.reset",
                         "Removed all server-owned recovery sandboxes");
  return runs_;
}

Result<std::string> RecoveryLab::RunUncleanShutdown(const RecoveryRun& run,
                                                    const Options& options) const {
#if defined(__unix__) || defined(__APPLE__)
  std::error_code error;
  if (!std::filesystem::is_regular_file(worker_path_, error) || error)
    return Status::NotSupported("recovery worker executable is unavailable");
  const pid_t child = ::fork();
  if (child < 0)
    return Status::IOError("fork recovery worker failed");
  if (child == 0) {
    ::execl(worker_path_.c_str(), worker_path_.c_str(), "--mode", "unclean", "--path",
            run.sandbox_path.c_str(), nullptr);
    std::_Exit(127);
  }
  int wait_status = 0;
  if (::waitpid(child, &wait_status, 0) != child)
    return Status::IOError("wait for recovery worker failed");
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)
    return Status::IOError("recovery worker did not exit successfully");
  Options recovery_options = options;
  recovery_options.create_if_missing = false;
  auto reopened = DB::Open(run.sandbox_path, recovery_options);
  if (!reopened.ok())
    return reopened.status().WithContext("recover worker sandbox");
  const auto value = reopened.value()->Get(kWorkerKey);
  const auto close = reopened.value()->Close();
  if (!value.ok())
    return value.status().WithContext("read worker recovery record");
  if (value.value() != kWorkerValue)
    return Status::Corruption("worker recovery record has an unexpected value");
  if (!close.ok())
    return close.WithContext("close worker recovery sandbox");
  return std::string(
      "Recovered the synced worker record after the child exited without Close().");
#else
  (void)run;
  (void)options;
  return Status::NotSupported(
      "unclean-shutdown recovery requires a POSIX worker process");
#endif
}

Result<std::string> RecoveryLab::RunTruncatedWal(const RecoveryRun& run,
                                                 const Options& options) const {
  Options recovery_options = options;
  recovery_options.create_if_missing = false;
  recovery_options.sync_on_write = true;
  {
    auto seeded = DB::Open(run.sandbox_path, recovery_options);
    if (!seeded.ok())
      return seeded.status().WithContext("open sandbox before WAL truncation");
    const auto first = seeded.value()->Put(kPrefixKey, kPrefixValue);
    const auto second =
        seeded.value()->Put("__tinylsm_recovery_tail__", "discardable-tail");
    const auto close = seeded.value()->Close();
    if (!first.ok())
      return first.WithContext("write durable WAL prefix");
    if (!second.ok())
      return second.WithContext("write WAL tail");
    if (!close.ok())
      return close.WithContext("close sandbox before WAL truncation");
  }
  const auto wal = ActiveWalPath(run.sandbox_path);
  if (wal.empty())
    return Status::Corruption("sandbox MANIFEST has no active WAL");
  std::error_code error;
  const auto size = std::filesystem::file_size(wal, error);
  if (error || size < 2)
    return Status::IOError("active WAL is too small to truncate safely");
  std::filesystem::resize_file(wal, size - 1, error);
  if (error)
    return Status::IOError("truncate sandbox WAL: " + error.message());
  auto reopened = DB::Open(run.sandbox_path, recovery_options);
  if (!reopened.ok())
    return reopened.status().WithContext("recover truncated WAL sandbox");
  const auto prefix = reopened.value()->Get(kPrefixKey);
  const auto close = reopened.value()->Close();
  if (!prefix.ok() || prefix.value() != kPrefixValue)
    return Status::Corruption("valid WAL prefix did not survive tail truncation");
  if (!close.ok())
    return close.WithContext("close truncated WAL sandbox");
  return std::string(
      "Recovered the valid WAL prefix and rejected the incomplete final record.");
}

Result<std::string> RecoveryLab::RunCrcCorruption(const RecoveryRun& run,
                                                  const Options& options) const {
  const auto manifest = run.sandbox_path / "MANIFEST";
  std::error_code error;
  const auto size = std::filesystem::file_size(manifest, error);
  if (error || size == 0)
    return Status::IOError("sandbox MANIFEST is unavailable for controlled corruption");
  std::fstream file(manifest, std::ios::binary | std::ios::in | std::ios::out);
  if (!file)
    return Status::IOError("open sandbox MANIFEST for controlled corruption failed");
  file.seekg(static_cast<std::streamoff>(size - 1));
  char value = 0;
  file.read(&value, 1);
  if (!file)
    return Status::IOError("read sandbox MANIFEST byte failed");
  value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x5AU);
  file.seekp(static_cast<std::streamoff>(size - 1));
  file.write(&value, 1);
  file.flush();
  if (!file)
    return Status::IOError("write sandbox MANIFEST byte failed");

  Options recovery_options = options;
  recovery_options.create_if_missing = false;
  auto opened = DB::Open(run.sandbox_path, recovery_options);
  if (opened.ok()) {
    (void)opened.value()->Close();
    return Status::Corruption("corrupted MANIFEST opened without reporting an error");
  }
  if (opened.status().code() != StatusCode::kCorruption)
    return opened.status().WithContext("open corrupted MANIFEST sandbox");
  return std::string(
      "Opening the corrupted sandbox MANIFEST returned Corruption as expected.");
}

} // namespace tinylsm::lab
