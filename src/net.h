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

#include <cstdint>
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
#include <sys/time.h>
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

// Open a blocking TCP connection to host:port. Returns kInvalidSocket on
// failure (including a peer that isn't listening — connect fails fast then).
inline socket_t connect_tcp(const std::string& host, std::uint16_t port) {
    socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kInvalidSocket) {
        return kInvalidSocket;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(sock);
        return kInvalidSocket;
    }
    return sock;
}

// Make recv() on this socket give up after `seconds` of silence instead of
// blocking forever. This is how we reap idle/slow clients (Redis's `timeout`
// directive) so a stalled connection can't tie up a worker thread for good.
// seconds <= 0 leaves the socket blocking (no timeout).
inline void set_recv_timeout(socket_t sock, int seconds) {
    if (seconds <= 0) {
        return;
    }
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(seconds) * 1000u;  // Windows wants milliseconds
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
}

// Make send() on this socket give up after `seconds` instead of blocking
// forever when the peer stops reading (a stalled follower). seconds <= 0 leaves
// it blocking.
inline void set_send_timeout(socket_t sock, int seconds) {
    if (seconds <= 0) {
        return;
    }
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(seconds) * 1000u;
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
}

// Set BOTH the send and receive timeouts on a socket, in milliseconds — used
// for the short, frequent Raft RPCs which need sub-second deadlines.
inline void set_io_timeout_ms(socket_t sock, int ms) {
    if (ms <= 0) {
        return;
    }
#if defined(_WIN32)
    DWORD t = static_cast<DWORD>(ms);
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&t), sizeof(t));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&t), sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
}

// Enable TCP keepalive (Redis enables this by default) so a peer that silently
// vanishes — machine crash, cable pull — is eventually detected by the OS and
// the connection torn down, instead of lingering forever.
inline void set_keepalive(socket_t sock) {
    int on = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
                 reinterpret_cast<const char*>(&on), sizeof(on));
}

// True if `code` (from last_error()) means "recv timed out" — set by
// set_recv_timeout() — as opposed to a genuine connection error.
inline bool is_timeout_error(int code) {
#if defined(_WIN32)
    return code == WSAETIMEDOUT;
#else
    return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

// RAII owner of a socket handle: closes it automatically when destroyed, so a
// socket can't leak down an error path or while an exception unwinds the stack.
// Ownership can be handed off with release() (the guard then closes nothing).
//
//     SocketCloser guard(sock);   // guard now owns sock
//     ... code that might return early or throw ...
//     guard.release();            // success: someone else owns sock now
//
class SocketCloser {
public:
    explicit SocketCloser(socket_t sock = kInvalidSocket) : sock_(sock) {}

    ~SocketCloser() {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
        }
    }

    SocketCloser(const SocketCloser&) = delete;
    SocketCloser& operator=(const SocketCloser&) = delete;

    socket_t get() const { return sock_; }

    // Give up ownership without closing; returns the handle for the new owner.
    socket_t release() {
        socket_t s = sock_;
        sock_ = kInvalidSocket;
        return s;
    }

private:
    socket_t sock_;
};

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
