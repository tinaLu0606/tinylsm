#include <iostream>

#include "tinylsm/db.h"

int main(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[2]) != "get") {
    std::cerr << "usage: tinylsm_cli <db-path> get <key>\n";
    return 2;
  }
  auto db = tinylsm::DB::Open(argv[1]);
  if (!db.ok()) {
    std::cerr << db.status().message() << '\n';
    return 1;
  }
  auto value = db.value()->Get(argv[3]);
  if (!value.ok()) {
    std::cerr << value.status().message() << '\n';
    return value.status().code() == tinylsm::StatusCode::kNotFound ? 3 : 1;
  }
  std::cout << value.value() << '\n';
  return 0;
}
