// net.h — a thin cross-platform wrapper over Berkeley/Winsock sockets.
//
// Windows and POSIX (Linux/macOS) expose almost the same socket API, but with a
// handful of differences that would otherwise litter the rest of the code with
// #ifdefs. We isolate every difference here:
//
//   * socket handle type      SOCKET (Windows)        vs  int (POSIX)
//   * "no socket" sentinel     INVALID_SOCKET          vs  -1
//   * closing a socket         closesocket()           vs  close()
//   * library startup          WSAStartup/WSACleanup   vs  (nothing)
//   * last error code          WSAGetLastError()       vs  errno
//
// Everything here is inline/header-only so any .cpp can include it.
#ifndef REDON_NET_H
#define REDON_NET_H

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// (CMakeLists links ws2_32; this pragma also pulls it in under MSVC directly.)
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace redon {
namespace net {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

// Close a socket handle, using whichever name the platform requires.
inline void close_socket(socket_t s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// The most recent socket error as an integer code.
inline int last_error() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

// A short human-readable description of the most recent socket error.
inline std::string last_error_str() {
    int code = last_error();
#if defined(_WIN32)
    // FormatMessage gives nice text but is verbose; the numeric code is plenty
    // for a learning project and is stable across locales.
    return "winsock error " + std::to_string(code);
#else
    return std::string(std::strerror(code)) + " (errno " +
           std::to_string(code) + ")";
#endif
}

// RAII guard that initializes the sockets library for the process. On Windows
// this calls WSAStartup in the constructor and WSACleanup in the destructor; on
// POSIX it does nothing. Create exactly one at the top of main() and keep it
// alive for the whole program:
//
//     redon::net::Init net_init;
//     if (!net_init.ok()) { /* handle error */ }
//
class Init {
public:
    Init() {
#if defined(_WIN32)
        WSADATA data;
        // Request Winsock 2.2.
        ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#else
        // Ignore SIGPIPE. On Linux/macOS, calling send() on a socket whose peer
        // has already closed would otherwise raise SIGPIPE, whose default action
        // terminates the whole process. With it ignored, send() instead returns
        // -1 with errno == EPIPE, which our send loops already treat as "the
        // connection broke". (Windows has no SIGPIPE; it reports the error via
        // the send() return value directly.)
        ::signal(SIGPIPE, SIG_IGN);
        ok_ = true;
#endif
    }

    ~Init() {
#if defined(_WIN32)
        if (ok_) {
            ::WSACleanup();
        }
#endif
    }

    // Non-copyable: there should only be one owner of the library lifetime.
    Init(const Init&) = delete;
    Init& operator=(const Init&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

}  // namespace net
}  // namespace redon

#endif  // REDON_NET_H
