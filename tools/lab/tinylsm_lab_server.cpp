#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "lab/lab_http_server.h"
#include "lab/lab_session.h"

namespace {

bool ParsePort(std::string_view value, int* port) {
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), *port);
  return error == std::errc{} && end == value.data() + value.size() && *port > 0 &&
         *port <= 65535;
}

void Usage() {
  std::cerr << "Usage: tinylsm_lab_server [--port 8080] [--static-dir path]\n";
}

} // namespace

int main(int argc, char** argv) {
  int port = 8080;
  std::filesystem::path static_directory;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--port" && index + 1 < argc) {
      if (!ParsePort(argv[++index], &port)) {
        Usage();
        return 2;
      }
      continue;
    }
    if (argument == "--static-dir" && index + 1 < argc) {
      static_directory = argv[++index];
      continue;
    }
    Usage();
    return 2;
  }

  tinylsm::lab::LabSession session;
  const auto worker_path =
      std::filesystem::absolute(argv[0]).parent_path() / "tinylsm_lab_worker";
  tinylsm::lab::LabHttpServer server(session, static_directory, worker_path);
  std::cout << "TinyLSM Lab server listening on http://127.0.0.1:" << port << '\n';
  return server.Listen("127.0.0.1", port) ? 0 : 1;
}
