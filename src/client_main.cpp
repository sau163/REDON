// client_main.cpp — entry point for the `redon-cli` executable.
//
// A minimal interactive client so you don't need telnet/netcat installed. It
// connects to a running redon-server, then reads commands from your keyboard,
// sends each one, and prints the server's reply.
//
// Usage:
//   redon-cli                     # connect to 127.0.0.1:6380
//   redon-cli <port>              # connect to 127.0.0.1:<port>
//   redon-cli <host> <port>       # connect to <host>:<port>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "net.h"

namespace {

constexpr std::uint16_t kDefaultPort = 6380;
const char kDefaultHost[] = "127.0.0.1";

// A server reply line is small. Refuse to buffer without bound if a broken or
// hostile server streams data with no newline terminator (mirrors the server's
// own kMaxLineLength guard so neither side can be memory-exhausted by the peer).
constexpr std::size_t kMaxLineLength = 64 * 1024;  // 64 KB

// Cap a single send() so casting (len - sent) to int can't overflow on a very
// large payload; oversized data is simply sent across multiple calls.
constexpr std::size_t kMaxSendChunk = std::size_t(1) << 30;  // 1 GiB

bool parse_port(const std::string& text, std::uint16_t* out) {
    try {
        std::size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        if (consumed != text.size() || value < 1 || value > 65535) {
            return false;
        }
        *out = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Send the whole buffer (a single send() may transmit only part of it).
bool send_all(redon::net::socket_t sock, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        std::size_t remaining = len - sent;
        int to_send = remaining > kMaxSendChunk
                          ? static_cast<int>(kMaxSendChunk)
                          : static_cast<int>(remaining);
        int n = ::send(sock, data + sent, to_send, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Read one '\n'-terminated line from the socket, buffering any extra bytes in
// `inbuf` for the next call. Returns false if the connection closed first.
bool recv_line(redon::net::socket_t sock, std::string* inbuf,
               std::string* line_out) {
    std::size_t newline;
    while ((newline = inbuf->find('\n')) == std::string::npos) {
        char chunk[4096];
        int n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n <= 0) {
            return false;  // closed or error
        }
        inbuf->append(chunk, static_cast<std::size_t>(n));
        if (inbuf->size() > kMaxLineLength) {
            return false;  // reply line too long: treat as a broken peer
        }
    }
    *line_out = inbuf->substr(0, newline);
    inbuf->erase(0, newline + 1);
    if (!line_out->empty() && line_out->back() == '\r') {
        line_out->pop_back();  // tolerate CRLF replies
    }
    return true;
}

// Uppercase the first whitespace-delimited token of `line` (used to spot QUIT).
std::string first_word_upper(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() &&
           std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    std::string word;
    while (i < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[i]))) {
        word.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(line[i]))));
        ++i;
    }
    return word;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = kDefaultHost;
    std::uint16_t port = kDefaultPort;

    if (argc == 2) {
        if (!parse_port(argv[1], &port)) {
            std::cerr << "error: invalid port '" << argv[1] << "'\n";
            return 1;
        }
    } else if (argc == 3) {
        host = argv[1];
        if (!parse_port(argv[2], &port)) {
            std::cerr << "error: invalid port '" << argv[2] << "'\n";
            return 1;
        }
    } else if (argc > 3) {
        std::cerr << "usage: redon-cli [host] [port]\n";
        return 1;
    }

    redon::net::Init net_init;
    if (!net_init.ok()) {
        std::cerr << "error: failed to initialize sockets library\n";
        return 1;
    }

    // Create the socket and connect to the server.
    redon::net::socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == redon::net::kInvalidSocket) {
        std::cerr << "error: could not create socket: "
                  << redon::net::last_error_str() << "\n";
        return 1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "error: invalid host '" << host << "'\n";
        redon::net::close_socket(sock);
        return 1;
    }
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        std::cerr << "error: could not connect to " << host << ":" << port
                  << ": " << redon::net::last_error_str() << "\n"
                  << "Is redon-server running?\n";
        redon::net::close_socket(sock);
        return 1;
    }

    std::cout << "Connected to Redon at " << host << ":" << port << ".\n";
    std::cout << "Type commands (SET/GET/DEL/EXISTS/PING), or QUIT to exit.\n";

    std::string inbuf;  // bytes received from server, not yet consumed
    std::string line;
    for (;;) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;  // end of input (Ctrl+Z then Enter on Windows, Ctrl+D on *nix)
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;  // don't bother the server with blank lines
        }

        std::string out = line + "\n";
        if (!send_all(sock, out.data(), out.size())) {
            std::cerr << "error: connection lost while sending\n";
            break;
        }

        std::string reply;
        if (!recv_line(sock, &inbuf, &reply)) {
            std::cout << "(server closed the connection)\n";
            break;
        }
        std::cout << reply << "\n";

        const std::string verb = first_word_upper(line);
        if (verb == "QUIT" || verb == "EXIT") {
            break;
        }
    }

    redon::net::close_socket(sock);
    return 0;
}
