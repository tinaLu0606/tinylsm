#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "db/filename.h"

namespace {

using tinylsm::internal::NumberedFileName;
using tinylsm::internal::NumberedFileType;

TEST(FilenameTest, FormatsAndParsesCanonicalNumberedFiles) {
  EXPECT_EQ(tinylsm::internal::WalFileName(1), "000001.wal");
  EXPECT_EQ(tinylsm::internal::SstableFileName(999999), "999999.sst");
  EXPECT_EQ(tinylsm::internal::SstableTempFileName(1000000), "1000000.sst.tmp");
  EXPECT_EQ(
      tinylsm::internal::SstableFileName(std::numeric_limits<std::uint64_t>::max()),
      "18446744073709551615.sst");

  EXPECT_EQ(tinylsm::internal::ParseNumberedFileName("000001.wal"),
            (NumberedFileName{1, NumberedFileType::kWal}));
  EXPECT_EQ(tinylsm::internal::ParseNumberedFileName("999999.sst"),
            (NumberedFileName{999999, NumberedFileType::kSstable}));
  EXPECT_EQ(tinylsm::internal::ParseNumberedFileName("1000000.sst.tmp"),
            (NumberedFileName{1000000, NumberedFileType::kSstableTemp}));
}

TEST(FilenameTest, RejectsZeroOverflowAndNonCanonicalNames) {
  const std::vector<std::string> rejected{
      "000000.wal",
      "1.sst",
      "0000001.sst",
      "+000001.sst",
      " 000001.sst",
      "000001.sst ",
      "000001.sst.tmp.bak",
      "000001.wal.tmp",
      "18446744073709551616.sst",
      ".sst",
      "MANIFEST.tmp",
  };
  for (const auto& name : rejected) {
    SCOPED_TRACE(name);
    EXPECT_FALSE(tinylsm::internal::ParseNumberedFileName(name).has_value());
  }
}

} // namespace
