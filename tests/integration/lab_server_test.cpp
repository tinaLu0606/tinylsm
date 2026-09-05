#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lab/lab_http_server.h"
#include "lab/lab_session.h"
#include "test_support/temp_dir.h"

namespace {
using tinylsm::lab::LabHttpServer;
using tinylsm::lab::LabSession;
using tinylsm::lab::OperationKind;
using tinylsm::lab::OperationRequest;
using tinylsm::test::TempDir;
using Json = nlohmann::json;

Json OptionsJson() {
  return {{"memtableBytes", 512},  {"createIfMissing", true},
          {"syncOnWrite", true},   {"maxKeyBytes", 4096},
          {"maxValueBytes", 4096}, {"sstableBlockBytes", 128}};
}

class RunningLabServer {
public:
  explicit RunningLabServer(LabSession& session) : server_(session) {
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

  const auto stream = client.Get("/api/events/stream?after=0");
  ASSERT_TRUE(stream);
  EXPECT_EQ(stream->get_header_value("Content-Type"), "text/event-stream");
  EXPECT_NE(stream->body.find("event: lab-event"), std::string::npos);
  EXPECT_NE(stream->body.find("operation.complete"), std::string::npos);
}

} // namespace
