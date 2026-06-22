// web.h — a tiny HTTP gateway that puts a browser UI in front of Redon.
//
// Browsers can't speak raw TCP, so they can't talk to the Redon server
// directly. redon-web is a small HTTP server that:
//   * serves a single-page web app at  GET /        (an embedded HTML page), and
//   * bridges                          POST /cmd     -> one Redon TCP command,
// so anyone with a browser can use Redon without a terminal or `redon-cli`.
//
// It deliberately reuses the same socket primitives as the rest of the project
// (net.h) and the same one-line-request / one-line-reply protocol the server
// speaks. It depends only on net.h + threads, not on the storage engine.
#ifndef REDON_WEB_H
#define REDON_WEB_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace redon {

class WebGateway {
public:
    WebGateway(std::string bind_host, std::uint16_t web_port,
               std::string redon_host, std::uint16_t redon_port, int workers);
    ~WebGateway();

    WebGateway(const WebGateway&) = delete;
    WebGateway& operator=(const WebGateway&) = delete;

    // Bind + listen and spawn the worker threads. Returns false if the port is
    // already in use (so main() can print a clear message and exit).
    bool start();

    // Block the calling thread until the gateway stops (joins the workers).
    void wait();

    // Stop accepting and unblock the workers (closes the listen socket).
    void stop();

private:
    void worker_loop();
    void handle_connection(net::socket_t client);
    // Build a full HTTP response for one parsed request.
    std::string route(const std::string& method, const std::string& path,
                      const std::string& body);
    // Send `line` to the Redon server and read one reply line. Returns false if
    // the server is unreachable or the request fails.
    bool forward_to_redon(const std::string& line, std::string* reply);

    std::string bind_host_;
    std::uint16_t web_port_;
    std::string redon_host_;
    std::uint16_t redon_port_;
    int workers_;
    net::socket_t listen_ = net::kInvalidSocket;
    std::atomic<bool> running_{false};
    std::vector<std::thread> threads_;
};

}  // namespace redon

#endif  // REDON_WEB_H
