#pragma once

#include <memory>

namespace zks::server {

class ServerRuntime;

struct HttpServerOptions {
    bool enable_test_ui = false;
};

class HttpServer {
  public:
    HttpServer(std::shared_ptr<ServerRuntime> runtime, HttpServerOptions options = {});

    int run();

  private:
    std::shared_ptr<ServerRuntime> runtime_;
    HttpServerOptions options_;
};

} // namespace zks::server
