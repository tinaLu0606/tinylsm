#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lab/lab_http_server.h"
#include "lab/lab_session.h"
#include "lab/recovery_lab.h"
#include "test_support/temp_dir.h"

namespace {
using tinylsm::lab::LabHttpServer;
using tinylsm::lab::LabSession;
using tinylsm::lab::OperationKind;
using tinylsm::lab::OperationRequest;
using tinylsm::lab::RecoveryLab;
using tinylsm::lab::RecoveryRunStatus;
using tinylsm::lab::RecoveryScenarioId;
using tinylsm::test::TempDir;
using Json = nlohmann::json;

Json OptionsJson() {
  return {{"memtableBytes", 512},  {"createIfMissing", true},
          {"syncOnWrite", true},   {"maxKeyBytes", 4096},
          {"maxValueBytes", 4096}, {"sstableBlockBytes", 128}};
}

bool WaitForWorkload(tinylsm::lab::LabSession& session) {
  for (int attempt = 0; attempt < 1'000; ++attempt) {
    const auto workload = session.GetWorkload();
    if (workload.status == tinylsm::lab::WorkloadStatus::kCompleted ||
        workload.status == tinylsm::lab::WorkloadStatus::kCancelled ||
        workload.status == tinylsm::lab::WorkloadStatus::kFailed)
      return workload.status != tinylsm::lab::WorkloadStatus::kFailed;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

class RunningLabServer {
public:
  explicit RunningLabServer(LabSession& session,
                            std::filesystem::path static_directory = {})
      : server_(session, std::move(static_directory), TINYLSM_LAB_WORKER_PATH) {
    port_ = server_.BindLoopbackAnyPort();
    EXPECT_GT(port_, 0);
    if (port_ > 0) {
      thread_ = std::thread([this] { (void)server_.ListenAfterBind(); });
      server_ready_ = true;
    }
  }
  ~RunningLabServer() {
    if (server_ready_) {
      server_.Stop();
      thread_.join();
    }
  }
  [[nodiscard]] int port() const { return port_; }

private:
  LabHttpServer server_;
  int port_ = 0;
  bool server_ready_ = false;
  std::thread thread_;
};

TEST(LabSessionTest, SerializesOperationsAndCopiesDiagnosticState) {
  TempDir dir;
  LabSession session;
  auto opened =
      session.Open(dir.path() / "live", tinylsm::Options{.memtable_bytes = 512,
                                                         .max_key_bytes = 4096,
                                                         .max_value_bytes = 4096,
                                                         .sstable_block_bytes = 128});
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  EXPECT_EQ(opened.value().active_wal, 1U);
  EXPECT_TRUE(opened.value().tables.empty());

  const auto put =
      session.Execute({.kind = OperationKind::kPut, .key = "alpha", .value = "one"});
  ASSERT_TRUE(put.status.ok()) << put.status.ToString();
  EXPECT_EQ(put.before.next_sequence, 1U);
  EXPECT_EQ(put.after.next_sequence, 2U);
  EXPECT_EQ(put.after.memtable_entries, 1U);

  const auto get = session.Execute({.kind = OperationKind::kGet, .key = "alpha"});
  ASSERT_TRUE(get.status.ok()) << get.status.ToString();
  ASSERT_TRUE(get.value.has_value());
  EXPECT_EQ(get.value->value, "one");
  const auto events = session.EventsAfter(0);
  EXPECT_GE(events.size(), 4U);
  EXPECT_LT(events.front().id, events.back().id);
  EXPECT_EQ(session.GetOperations().front().operation_id, get.operation_id);
}

TEST(LabStorageInspectorTest, PagesCanonicalWalAndSstableBytesWithoutEscapingDatabase) {
  TempDir dir;
  LabSession session;
  auto opened =
      session.Open(dir.path() / "live", tinylsm::Options{.memtable_bytes = 128,
                                                         .max_key_bytes = 4096,
                                                         .max_value_bytes = 4096,
                                                         .sstable_block_bytes = 32});
  ASSERT_TRUE(opened.ok()) << opened.status().ToString();
  ASSERT_TRUE(
      session
          .Execute(
              {.kind = OperationKind::kPut, .key = "alpha", .value = "first-value"})
          .status.ok());
  ASSERT_TRUE(
      session
          .Execute(
              {.kind = OperationKind::kPut, .key = "beta", .value = "second-value"})
          .status.ok());
  const auto state = session.GetState();
  ASSERT_FALSE(state.tables.empty());
  ASSERT_TRUE(
      session.Execute({.kind = OperationKind::kPut, .key = "live", .value = "wal"})
          .status.ok());

  const auto manifest = session.InspectManifest();
  ASSERT_TRUE(manifest.ok()) << manifest.status().ToString();
  EXPECT_EQ(manifest.value().active_wal, state.active_wal);
  const auto wal = session.InspectWal("000003.wal", 0, 1);
  ASSERT_TRUE(wal.ok()) << wal.status().ToString();
  ASSERT_EQ(wal.value().items.size(), 1U);
  ASSERT_TRUE(wal.value().items.front().entry.has_value());
  EXPECT_EQ(wal.value().items.front().entry->key, "live");

  const auto table = session.InspectSstable("000002.sst", 0, 1);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_FALSE(table.value().items.empty());
  EXPECT_FALSE(table.value().items.front().entries.empty());
  EXPECT_FALSE(session.InspectFileBytes("../MANIFEST", 0, 1).ok());
  EXPECT_FALSE(session.InspectFileBytes("MANIFEST", 0, 0).ok());
}

TEST(LabWorkloadTest, ReplaysFixedSeedWithTheSameOperationSequenceAndReferenceResult) {
  TempDir first_dir;
  TempDir second_dir;
  tinylsm::lab::WorkloadConfig config{.operation_count = 30,
                                      .seed = 77,
                                      .key_space = 10,
                                      .value_bytes = 12,
                                      .put_ratio = 50,
                                      .get_ratio = 30,
                                      .delete_ratio = 20,
                                      .operations_per_second = 200,
                                      .distribution =
                                          tinylsm::lab::WorkloadDistribution::kHotspot,
                                      .reopen_every = 7};
  const auto run = [&](const std::filesystem::path& path) {
    LabSession session;
    const auto opened = session.Open(path, tinylsm::Options{.memtable_bytes = 128,
                                                            .max_key_bytes = 4096,
                                                            .max_value_bytes = 4096,
                                                            .sstable_block_bytes = 64});
    EXPECT_TRUE(opened.ok());
    if (!opened.ok())
      return std::vector<tinylsm::lab::OperationResult>{};
    EXPECT_TRUE(session
                    .Execute({.kind = OperationKind::kPut,
                              .key = "preexisting",
                              .value = "retained"})
                    .status.ok());
    const auto started = session.StartWorkload(config);
    EXPECT_TRUE(started.ok());
    if (!started.ok())
      return std::vector<tinylsm::lab::OperationResult>{};
    bool finished = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
      const auto workload = session.GetWorkload();
      if (workload.status == tinylsm::lab::WorkloadStatus::kCompleted ||
          workload.status == tinylsm::lab::WorkloadStatus::kFailed) {
        EXPECT_EQ(workload.status, tinylsm::lab::WorkloadStatus::kCompleted);
        EXPECT_FALSE(workload.mismatch.has_value());
        finished = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(finished);
    return session.GetOperations();
  };
  const auto first = run(first_dir.path() / "first");
  const auto second = run(second_dir.path() / "second");
  ASSERT_EQ(first.size(), second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    EXPECT_EQ(first[index].request.kind, second[index].request.kind);
    EXPECT_EQ(first[index].request.key, second[index].request.key);
    EXPECT_EQ(first[index].request.value, second[index].request.value);
    EXPECT_EQ(first[index].status.code(), second[index].status.code());
  }
}

TEST(LabSessionTest,
     RotatesDetailedJsonlEventsAndSkipsPerOperationLogsInPerformanceMode) {
  TempDir detailed_dir;
  LabSession detailed;
  ASSERT_TRUE(detailed.Open(detailed_dir.path() / "live", tinylsm::Options{}).ok());
  for (int index = 0; index < 350; ++index) {
    const auto result = detailed.Execute(
        {.kind = OperationKind::kGet, .key = "missing-" + std::to_string(index)});
    EXPECT_EQ(result.status.code(), tinylsm::StatusCode::kNotFound);
  }
  std::size_t logs = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(detailed_dir.path() / "live")) {
    if (entry.path().extension() != ".jsonl")
      continue;
    ++logs;
    std::ifstream input(entry.path());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    EXPECT_TRUE(Json::parse(line).contains("phase"));
  }
  EXPECT_GE(logs, 2U);

  TempDir performance_dir;
  LabSession performance;
  ASSERT_TRUE(
      performance.Open(performance_dir.path() / "live", tinylsm::Options{}).ok());
  const auto started = performance.StartWorkload(
      {.operation_count = 20,
       .seed = 9,
       .key_space = 5,
       .value_bytes = 8,
       .put_ratio = 50,
       .get_ratio = 30,
       .delete_ratio = 20,
       .operations_per_second = 500,
       .distribution = tinylsm::lab::WorkloadDistribution::kUniform});
  ASSERT_TRUE(started.ok()) << started.status().ToString();
  EXPECT_TRUE(WaitForWorkload(performance));
  EXPECT_TRUE(performance.GetOperations().empty());
  for (const auto& entry :
       std::filesystem::directory_iterator(performance_dir.path() / "live")) {
    if (entry.path().extension() != ".jsonl")
      continue;
    std::ifstream input(entry.path());
    std::string line;
    while (std::getline(input, line))
      EXPECT_EQ(Json::parse(line).at("phase"), "session.open");
  }
}

TEST(LabWorkloadTest,
     RejectsExternalWritesWhileTheDeterministicRunnerOwnsTheWritePath) {
  TempDir dir;
  LabSession session;
  ASSERT_TRUE(session.Open(dir.path() / "live", tinylsm::Options{}).ok());
  const auto started = session.StartWorkload(
      {.operation_count = 200,
       .seed = 3,
       .key_space = 8,
       .value_bytes = 16,
       .put_ratio = 60,
       .get_ratio = 20,
       .delete_ratio = 20,
       .operations_per_second = 20,
       .distribution = tinylsm::lab::WorkloadDistribution::kSequential});
  ASSERT_TRUE(started.ok()) << started.status().ToString();
  const auto external = session.Execute(
      {.kind = OperationKind::kPut, .key = "outside", .value = "blocked"});
  EXPECT_EQ(external.status.code(), tinylsm::StatusCode::kNotSupported);
  ASSERT_TRUE(session.CancelWorkload().ok());
  EXPECT_TRUE(WaitForWorkload(session));
}

TEST(RecoveryLabTest, RunsAllScenariosOnlyInsideServerOwnedSandboxes) {
  TempDir dir;
  LabSession session;
  ASSERT_TRUE(session.Open(dir.path() / "live", tinylsm::Options{}).ok());
  ASSERT_TRUE(
      session
          .Execute({.kind = OperationKind::kPut, .key = "source", .value = "preserved"})
          .status.ok());
  RecoveryLab recovery(session, TINYLSM_LAB_WORKER_PATH);
  for (const auto scenario :
       {RecoveryScenarioId::kUncleanShutdown, RecoveryScenarioId::kTruncatedWal,
        RecoveryScenarioId::kCrcCorruption}) {
    const auto preview = recovery.Preview(scenario);
    ASSERT_TRUE(preview.ok()) << preview.status().ToString();
    ASSERT_EQ(preview.value().status, RecoveryRunStatus::kPreview);
    ASSERT_TRUE(std::filesystem::exists(preview.value().sandbox_path));
    const auto run = recovery.Run(preview.value().id);
    ASSERT_TRUE(run.ok()) << run.status().ToString();
    EXPECT_EQ(run.value().status, RecoveryRunStatus::kPassed)
        << run.value().actual_outcome.value_or("missing outcome");
    ASSERT_TRUE(run.value().actual_outcome.has_value());
    EXPECT_FALSE(run.value().events.empty());
  }
  EXPECT_FALSE(recovery.Run("../../../outside").ok());
  const auto sandboxes = recovery.Runs();
  ASSERT_EQ(sandboxes.size(), 3U);
  const auto reset = recovery.Reset();
  ASSERT_TRUE(reset.ok()) << reset.status().ToString();
  for (const auto& run : sandboxes)
    EXPECT_FALSE(std::filesystem::exists(run.sandbox_path));
}

TEST(RecoveryLabTest, RejectsSandboxPathThatResolvesOutsideTheServerRoot) {
  TempDir dir;
  LabSession session;
  ASSERT_TRUE(session.Open(dir.path() / "live", tinylsm::Options{}).ok());
  RecoveryLab recovery(session, TINYLSM_LAB_WORKER_PATH);
  const auto preview = recovery.Preview(RecoveryScenarioId::kTruncatedWal);
  ASSERT_TRUE(preview.ok()) << preview.status().ToString();

  const auto outside = dir.path() / "outside";
  std::filesystem::create_directories(outside);
  std::error_code error;
  std::filesystem::remove_all(preview.value().sandbox_path, error);
  ASSERT_FALSE(error) << error.message();
  std::filesystem::create_directory_symlink(outside, preview.value().sandbox_path,
                                            error);
  ASSERT_FALSE(error) << error.message();

  const auto run = recovery.Run(preview.value().id);
  ASSERT_FALSE(run.ok());
  EXPECT_EQ(run.status().code(), tinylsm::StatusCode::kInvalidArgument);
  EXPECT_TRUE(std::filesystem::is_empty(outside));
}

TEST(LabServerHttpTest, ServesLiveOperationsStateStorageAndReconnectableEvents) {
  TempDir dir;
  LabSession session;
  RunningLabServer server(session);
  ASSERT_GT(server.port(), 0);
  httplib::Client client("127.0.0.1", server.port());
  client.set_connection_timeout(std::chrono::seconds(2));

  const auto open = client.Post(
      "/api/session/open",
      Json{{"path", (dir.path() / "live").string()}, {"options", OptionsJson()}}.dump(),
      "application/json");
  ASSERT_TRUE(open);
  ASSERT_EQ(open->status, 200);
  EXPECT_EQ(Json::parse(open->body).at("source"), "live");

  const auto put =
      client.Post("/api/operations/put",
                  Json{{"key", {{"encoding", "base64"}, {"data", "YWxwaGE="}}},
                       {"value", {{"encoding", "base64"}, {"data", "b25l"}}}}
                      .dump(),
                  "application/json");
  ASSERT_TRUE(put);
  ASSERT_EQ(put->status, 200);
  EXPECT_EQ(Json::parse(put->body).at("status").at("code"), "OK");

  const auto get =
      client.Post("/api/operations/get",
                  Json{{"key", {{"encoding", "base64"}, {"data", "YWxwaGE="}}}}.dump(),
                  "application/json");
  ASSERT_TRUE(get);
  const Json get_json = Json::parse(get->body);
  EXPECT_EQ(get_json.at("value").at("valueBase64"), "b25l");

  const auto state = client.Get("/api/session/state");
  ASSERT_TRUE(state);
  EXPECT_EQ(Json::parse(state->body).at("memtableEntries"), 1);
  const auto storage = client.Get("/api/storage/files");
  ASSERT_TRUE(storage);
  EXPECT_FALSE(Json::parse(storage->body).at("files").empty());
  const auto manifest = client.Get("/api/storage/manifest");
  ASSERT_TRUE(manifest);
  EXPECT_EQ(Json::parse(manifest->body).at("crc"), "ok");
  const auto wal = client.Get("/api/storage/wal/000001.wal/records?cursor=0&limit=1");
  ASSERT_TRUE(wal);
  EXPECT_EQ(Json::parse(wal->body).at("records").size(), 1U);
  const auto bytes = client.Get("/api/storage/file/MANIFEST/bytes?offset=0&length=1");
  ASSERT_TRUE(bytes);
  EXPECT_EQ(Json::parse(bytes->body).at("length"), 1);
  const auto metrics = client.Get("/api/metrics");
  ASSERT_TRUE(metrics);
  const Json metrics_json = Json::parse(metrics->body);
  EXPECT_TRUE(metrics_json.at("processMetricsAvailable"));
  ASSERT_FALSE(metrics_json.at("points").empty());
  EXPECT_TRUE(metrics_json.at("points").back().contains("walBytes"));

  const Json workload_config{{"operationCount", 100},
                             {"seed", 5},
                             {"keySpace", 10},
                             {"valueBytes", 12},
                             {"putRatio", 50},
                             {"getRatio", 30},
                             {"deleteRatio", 20},
                             {"operationsPerSecond", 20},
                             {"distribution", "uniform"},
                             {"reopenEvery", 0}};
  const auto workload =
      client.Post("/api/workloads", workload_config.dump(), "application/json");
  ASSERT_TRUE(workload);
  ASSERT_EQ(workload->status, 200);
  const auto workload_id = Json::parse(workload->body).at("id").get<std::string>();
  const auto pause =
      client.Post("/api/workloads/" + workload_id + "/pause", "", "application/json");
  ASSERT_TRUE(pause);
  EXPECT_EQ(Json::parse(pause->body).at("status"), "paused");
  const auto resume =
      client.Post("/api/workloads/" + workload_id + "/resume", "", "application/json");
  ASSERT_TRUE(resume);
  const auto cancel =
      client.Post("/api/workloads/" + workload_id + "/cancel", "", "application/json");
  ASSERT_TRUE(cancel);
  EXPECT_TRUE(WaitForWorkload(session));
  const auto unknown =
      client.Post("/api/workloads/not-this-run/cancel", "", "application/json");
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->status, 404);

  const auto events = client.Get("/api/events?limit=1");
  ASSERT_TRUE(events);
  const Json events_json = Json::parse(events->body);
  EXPECT_EQ(events_json.at("events").size(), 1U);
  EXPECT_TRUE(events_json.at("hasMore"));

  const auto scenarios = client.Get("/api/recovery/scenarios");
  ASSERT_TRUE(scenarios);
  EXPECT_EQ(Json::parse(scenarios->body).size(), 3U);
  const auto preview =
      client.Post("/api/recovery/experiments",
                  Json{{"scenarioId", "unclean-shutdown"}}.dump(), "application/json");
  ASSERT_TRUE(preview);
  ASSERT_EQ(preview->status, 200);
  const auto recovery_id = Json::parse(preview->body).at("id").get<std::string>();
  const auto recovery_run = client.Post(
      "/api/recovery/experiments/" + recovery_id + "/run", "", "application/json");
  ASSERT_TRUE(recovery_run);
  EXPECT_EQ(Json::parse(recovery_run->body).at("status"), "passed");
  const auto recovery_reset =
      client.Post("/api/recovery/reset", "", "application/json");
  ASSERT_TRUE(recovery_reset);
  EXPECT_EQ(Json::parse(recovery_reset->body), Json::array());
  const auto source_after_recovery =
      client.Post("/api/operations/get",
                  Json{{"key", {{"encoding", "base64"}, {"data", "YWxwaGE="}}}}.dump(),
                  "application/json");
  ASSERT_TRUE(source_after_recovery);
  EXPECT_EQ(Json::parse(source_after_recovery->body).at("value").at("valueBase64"),
            "b25l");
  const auto invalid_preview =
      client.Post("/api/recovery/experiments",
                  Json{{"scenarioId", "../outside"}}.dump(), "application/json");
  ASSERT_TRUE(invalid_preview);
  EXPECT_EQ(invalid_preview->status, 400);

  const auto stream = client.Get("/api/events/stream?after=0");
  ASSERT_TRUE(stream);
  EXPECT_EQ(stream->get_header_value("Content-Type"), "text/event-stream");
  EXPECT_NE(stream->body.find("event: lab-event"), std::string::npos);
  EXPECT_NE(stream->body.find("operation.complete"), std::string::npos);
}

TEST(LabServerStaticTest, ServesPackagedUiFilesAndRejectsOversizedRecoveryRequests) {
  TempDir dir;
  const auto static_directory = dir.path() / "static";
  std::filesystem::create_directories(static_directory);
  {
    std::ofstream index(static_directory / "index.html");
    index << "<html><body>TinyLSM Lab</body></html>";
  }
  LabSession session;
  RunningLabServer server(session, static_directory);
  ASSERT_GT(server.port(), 0);
  httplib::Client client("127.0.0.1", server.port());
  const auto page = client.Get("/index.html");
  ASSERT_TRUE(page);
  EXPECT_EQ(page->status, 200);
  EXPECT_NE(page->body.find("TinyLSM Lab"), std::string::npos);

  const std::string oversized(8U * 1024U * 1024U + 1U, 'x');
  const auto rejected =
      client.Post("/api/recovery/experiments", oversized, "application/json");
  ASSERT_TRUE(rejected);
  EXPECT_GE(rejected->status, 400);
}

} // namespace
