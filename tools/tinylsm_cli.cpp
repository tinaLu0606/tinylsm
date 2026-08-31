#include <cstdio>
#include <exception>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

#include "cli/cli_app.h"
#include "tinylsm/db.h"

int main(int argc, char** argv) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
      arguments.emplace_back(argv[index]);
    return tinylsm::cli::RunCli(arguments, std::cout, std::cerr);
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
