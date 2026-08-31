#include <gtest/gtest.h>

#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>

#include "cli/cli_app.h"
#include "test_support/temp_dir.h"
#include "tinylsm/db.h"

namespace {

struct CliResult {
  int exit_code;
  std::string output;
  std::string error;
};

CliResult RunCli(std::initializer_list<std::string_view> arguments) {
  std::ostringstream output;
  std::ostringstream error;
  const int exit_code =
      tinylsm::cli::RunCli({arguments.begin(), arguments.size()}, output, error);
  return {exit_code, output.str(), error.str()};
}

} // namespace

TEST(CliTest, RejectsInvalidArgumentsBeforeOpeningDatabase) {
  tinylsm::test::TempDir dir;
  const std::string db_path = dir.path().string();

  const auto help = RunCli({"help"});
  EXPECT_EQ(help.exit_code, 0);
  EXPECT_NE(help.output.find("tinylsm_cli <db-path> put"), std::string::npos);
  EXPECT_TRUE(help.error.empty());

  const auto unknown = RunCli({db_path, "unknown"});
  EXPECT_EQ(unknown.exit_code, 2);
  EXPECT_TRUE(unknown.output.empty());
  EXPECT_NE(unknown.error.find("usage:"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(dir.path()));

  const auto invalid_json = RunCli({db_path, "put", "key", "value", "--json"});
  EXPECT_EQ(invalid_json.exit_code, 2);
  EXPECT_FALSE(std::filesystem::exists(dir.path()));
}

TEST(CliTest, SupportsPersistentPutGetScanAndDeleteCommands) {
  tinylsm::test::TempDir dir;
  const std::string db_path = dir.path().string();

  auto result = RunCli({db_path, "put", "alpha", "one"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "OK\n");
  EXPECT_TRUE(result.error.empty());

  result = RunCli({db_path, "put", "beta", "two"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "OK\n");

  result = RunCli({db_path, "get", "alpha"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "one\n");

  result = RunCli({db_path, "get", "alpha", "--json"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "{\"key_base64\":\"YWxwaGE=\",\"value_base64\":\"b25l\"}\n");

  result = RunCli({db_path, "scan"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "alpha\tone\nbeta\ttwo\n");

  result = RunCli({db_path, "scan", "beta"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "beta\ttwo\n");

  result = RunCli({db_path, "scan", "alpha", "beta"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "alpha\tone\n");

  result = RunCli({db_path, "scan", "--json"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "{\"key_base64\":\"YWxwaGE=\",\"value_base64\":\"b25l\"}\n"
                           "{\"key_base64\":\"YmV0YQ==\",\"value_base64\":\"dHdv\"}\n");

  result = RunCli({db_path, "delete", "alpha"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "OK\n");

  result = RunCli({db_path, "get", "alpha"});
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error.find("NotFound"), std::string::npos);
}

TEST(CliTest, JsonOutputPreservesArbitraryBytesWithBase64) {
  tinylsm::test::TempDir dir;
  const std::string db_path = dir.path().string();
  const std::string key("\0\xff", 2);
  const std::string value("\x01\x02\xfe", 3);

  auto db = tinylsm::DB::Open(dir.path());
  ASSERT_TRUE(db.ok()) << db.status().ToString();
  ASSERT_TRUE(db.value()->Put(key, value).ok());
  ASSERT_TRUE(db.value()->Close().ok());

  const auto result = RunCli({db_path, "scan", "--json"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_EQ(result.output, "{\"key_base64\":\"AP8=\",\"value_base64\":\"AQL+\"}\n");
  EXPECT_TRUE(result.error.empty());
}
