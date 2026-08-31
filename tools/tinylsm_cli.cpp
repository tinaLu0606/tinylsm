#include <cstdio>
#include <exception>
#include <iostream>
#include <new>

#include "tinylsm/db.h"

namespace {
int Run(int argc, char** argv) {
  if (argc != 4 || std::string_view(argv[2]) != "get") {
    std::cerr << "usage: tinylsm_cli <db-path> get <key>\n";
    return 2;
  }
  auto db = tinylsm::DB::Open(argv[1]);
  if (!db.ok()) {
    std::cerr << db.status().ToString() << '\n';
    return 1;
  }
  auto value = db.value()->Get(argv[3]);
  if (!value.ok()) {
    std::cerr << value.status().ToString() << '\n';
    return value.status().code() == tinylsm::StatusCode::kNotFound ? 3 : 1;
  }
  std::cout << value.value() << '\n';
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const tinylsm::BadResultAccess& error) {
    std::cerr << "internal result access error: " << error.what() << '\n';
  } catch (const std::bad_alloc&) {
    std::fputs("tinylsm_cli: out of memory\n", stderr);
  } catch (const std::exception& error) {
    std::cerr << "tinylsm_cli: unexpected exception: " << error.what() << '\n';
  } catch (...) {
    std::fputs("tinylsm_cli: unknown exception\n", stderr);
  }
  return 1;
}
