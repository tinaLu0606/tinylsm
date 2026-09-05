#include <cstdlib>
#include <filesystem>
#include <string_view>

#include "tinylsm/db.h"

namespace {

constexpr std::string_view kWorkerKey = "__tinylsm_recovery_worker__";
constexpr std::string_view kWorkerValue = "durable-without-close";

bool IsUncleanInvocation(int argc, char** argv) {
  return argc == 5 && std::string_view(argv[1]) == "--mode" &&
         std::string_view(argv[2]) == "unclean" &&
         std::string_view(argv[3]) == "--path";
}

} // namespace

int main(int argc, char** argv) {
  if (!IsUncleanInvocation(argc, argv))
    return 2;
  tinylsm::Options options;
  options.create_if_missing = false;
  options.sync_on_write = true;
  auto db = tinylsm::DB::Open(std::filesystem::path(argv[4]), options);
  if (!db.ok())
    return 1;
  if (!db.value()->Put(kWorkerKey, kWorkerValue).ok())
    return 1;

  // Deliberately bypass DB and C++ destructors to model process termination.
  std::_Exit(0);
}
