// web_main.cpp — entry point for redon-web, the browser gateway.
//
// Usage:
//   redon-web [web_port=8080] [redon_host=127.0.0.1] [redon_port=6380] [workers=8]
//
// Then open http://127.0.0.1:8080 in a browser. The gateway forwards each
// command you run to the Redon TCP server at redon_host:redon_port.
//
// The listen address defaults to 127.0.0.1 (loopback only). Set the environment
// variable REDON_WEB_HOST=0.0.0.0 to expose it on the network (this is what the
// Docker image does) — note the gateway is unauthenticated, so only do that on a
// network you trust.

// std::getenv is flagged C4996 ("unsafe") by MSVC; our use is read-only and the
// result is consumed immediately, so the warning doesn't apply here.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "net.h"
#include "web.h"

namespace {

bool parse_port(const std::string& s, std::uint16_t* out) {
    try {
        long v = std::stol(s);
        if (v < 1 || v > 65535) {
            return false;
        }
        *out = static_cast<std::uint16_t>(v);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "fatal: could not initialize sockets\n";
        return 1;
    }

    std::uint16_t web_port = 8080;
    std::string redon_host = "127.0.0.1";
    std::uint16_t redon_port = 6380;
    int workers = 8;

    if (argc > 1 && !parse_port(argv[1], &web_port)) {
        std::cerr << "invalid web port: " << argv[1] << "\n";
        return 1;
    }
    if (argc > 2) {
        redon_host = argv[2];
    }
    if (argc > 3 && !parse_port(argv[3], &redon_port)) {
        std::cerr << "invalid redon port: " << argv[3] << "\n";
        return 1;
    }
    if (argc > 4) {
        try {
            workers = std::stoi(argv[4]);
        } catch (const std::exception&) {
            std::cerr << "invalid worker count: " << argv[4] << "\n";
            return 1;
        }
    }

    std::string bind_host = "127.0.0.1";
    if (const char* env = std::getenv("REDON_WEB_HOST")) {
        if (env[0] != '\0') {
            bind_host = env;
        }
    }

    redon::WebGateway gateway(bind_host, web_port, redon_host, redon_port, workers);
    if (!gateway.start()) {
        std::cerr << "fatal: could not bind " << bind_host << ":" << web_port
                  << " (is the port already in use?)\n";
        return 1;
    }

    std::cout << "Redon web UI:  http://" << bind_host << ":" << web_port << "\n"
              << "  bridging to Redon server at " << redon_host << ":"
              << redon_port << "\n"
              << "  (Ctrl-C to stop)\n";
    std::cout.flush();

    gateway.wait();
    return 0;
}
