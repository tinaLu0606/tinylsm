#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "lab/lab_session.h"
#include "tinylsm/result.h"

namespace tinylsm::lab {

enum class RecoveryScenarioId {
  kUncleanShutdown,
  kTruncatedWal,
  kCrcCorruption,
};

enum class RecoveryRunStatus { kPreview, kRunning, kPassed, kFailed };

struct RecoveryScenario {
  RecoveryScenarioId id;
  std::string name;
  std::string description;
  std::string expected_outcome;
  std::string mutation;
};

struct RecoveryRun {
  std::string id;
  RecoveryScenarioId scenario_id;
  RecoveryRunStatus status = RecoveryRunStatus::kPreview;
  std::filesystem::path sandbox_path;
  std::string expected_outcome;
  std::optional<std::string> actual_outcome;
  std::string created_at;
  std::vector<Event> events;
};

/// Owns server-created sandbox copies and never mutates the live session path.
class RecoveryLab {
public:
  RecoveryLab(LabSession& session, std::filesystem::path worker_path);
  ~RecoveryLab();

  RecoveryLab(const RecoveryLab&) = delete;
  RecoveryLab& operator=(const RecoveryLab&) = delete;

  [[nodiscard]] std::vector<RecoveryScenario> Scenarios() const;
  Result<RecoveryRun> Preview(RecoveryScenarioId scenario);
  Result<RecoveryRun> Run(std::string_view id);
  Result<RecoveryRun> Get(std::string_view id) const;
  [[nodiscard]] std::vector<RecoveryRun> Runs() const;
  Result<std::vector<RecoveryRun>> Reset();

private:
  Result<std::filesystem::path> CreateSandbox();
  [[nodiscard]] bool IsSandboxPath(const std::filesystem::path& path) const;
  void AddEvent(RecoveryRun& run, std::string level, std::string phase,
                std::string summary) const;
  Result<std::string> RunUncleanShutdown(const RecoveryRun& run,
                                         const Options& options) const;
  Result<std::string> RunTruncatedWal(const RecoveryRun& run,
                                      const Options& options) const;
  Result<std::string> RunCrcCorruption(const RecoveryRun& run,
                                       const Options& options) const;

  LabSession& session_;
  std::filesystem::path worker_path_;
  std::filesystem::path sandbox_root_;
  std::vector<RecoveryRun> runs_;
  std::uint64_t next_run_id_ = 1;
  mutable std::mutex mutex_;
};

[[nodiscard]] std::optional<RecoveryScenarioId>
ParseRecoveryScenarioId(std::string_view value);
[[nodiscard]] const char* RecoveryScenarioName(RecoveryScenarioId id);
[[nodiscard]] const char* RecoveryRunStatusName(RecoveryRunStatus status);

} // namespace tinylsm::lab
