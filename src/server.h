// server.h — the TCP server that ties networking + protocol + storage together.
//
// Phase 1 behavior: bind a port, then loop forever accepting ONE client at a
// time. Each client is served fully (read commands, reply) until it disconnects
// or sends QUIT, after which the server accepts the next client. Phase 2 will
// hand each accepted client to a worker thread so many can be served at once;
// the per-client logic in handle_client() is already isolated for that change.
#ifndef REDON_SERVER_H
#define REDON_SERVER_H

#include <cstdint>
#include <string>

#include "net.h"
#include "storage.h"

namespace redon {

class Server {
public:
    Server(std::string host, std::uint16_t port);

    // Set up the listening socket and run the accept loop. Blocks indefinitely.
    // Returns a non-zero exit code if the socket could not be set up; otherwise
    // it does not return under normal operation (stop with Ctrl+C).
    int run();

private:
    // Serve a single connected client until it disconnects or sends QUIT.
    void handle_client(net::socket_t client);

    std::string host_;
    std::uint16_t port_;
    Storage store_;  // the one shared database for every client
};

}  // namespace redon

#endif  // REDON_SERVER_H
