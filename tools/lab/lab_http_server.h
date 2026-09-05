#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace tinylsm::lab {

class LabSession;

/// Loopback-only HTTP adapter around a serialized LabSession.
class LabHttpServer {
public:
  explicit LabHttpServer(LabSession& session,
                         std::filesystem::path static_directory = {});
  ~LabHttpServer();

  LabHttpServer(const LabHttpServer&) = delete;
  LabHttpServer& operator=(const LabHttpServer&) = delete;

  [[nodiscard]] bool Listen(const std::string& host, int port);
  [[nodiscard]] int BindLoopbackAnyPort();
  [[nodiscard]] bool ListenAfterBind();
  void Stop();

private:
  LabSession& session_;
  std::filesystem::path static_directory_;
  std::unique_ptr<httplib::Server> server_;
};

} // namespace tinylsm::lab
