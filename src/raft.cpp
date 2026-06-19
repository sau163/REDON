// raft.cpp — implementation of Raft leader election.
#include "raft.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <utility>

#include "net.h"

namespace redon {
namespace {

constexpr int kTickMs = 20;       // how often the timer thread wakes up
constexpr int kMaxLine = 1024;    // RPC replies are tiny

bool split_host_port(const std::string& addr, std::string* host,
                     std::uint16_t* port) {
    std::size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= addr.size()) {
        return false;
    }
    *host = addr.substr(0, colon);
    try {
        int p = std::stoi(addr.substr(colon + 1));
        if (p < 1 || p > 65535) {
            return false;
        }
        *port = static_cast<std::uint16_t>(p);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool send_all(net::socket_t sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(sock, data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_line(net::socket_t sock, std::string* line) {
    line->clear();
    char c;
    for (;;) {
        int n = ::recv(sock, &c, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (line->size() >= kMaxLine) {
            return false;  // reply too long for a Raft RPC — malformed peer
        }
        line->push_back(c);
    }
}

// Send `request`, read one reply line, and split it into whitespace tokens.
bool rpc(const std::string& host, std::uint16_t port, int timeout_ms,
         const std::string& request, std::vector<std::string>* tokens) {
    net::socket_t sock = net::connect_tcp(host, port);
    if (sock == net::kInvalidSocket) {
        return false;
    }
    net::set_io_timeout_ms(sock, timeout_ms);
    std::string reply;
    bool ok = send_all(sock, request) && recv_line(sock, &reply);
    net::close_socket(sock);
    if (!ok) {
        return false;
    }
    tokens->clear();
    std::istringstream iss(reply);
    std::string tok;
    while (iss >> tok) {
        tokens->push_back(tok);
    }
    return true;
}

}  // namespace

RaftNode::RaftNode(Config config) : config_(std::move(config)) {
    // Drop ourselves and any duplicates from the peer list. Otherwise a stray
    // "--raft <self>" would make us send a RequestVote to ourselves, grant it,
    // and count our own vote TWICE — letting us reach a "majority" without a real
    // quorum, which would break the one-leader-per-term guarantee.
    std::vector<std::string> peers;
    for (const std::string& p : config_.peers) {
        if (p != config_.self_id &&
            std::find(peers.begin(), peers.end(), p) == peers.end()) {
            peers.push_back(p);
        }
    }
    config_.peers = std::move(peers);

    // Seed the RNG with full entropy from the id and the clock (feeding both
    // 32-bit halves, since seed_seq consumes 32-bit values) so peers pick
    // different election timeouts — what makes split votes rare.
    std::uint64_t h = std::hash<std::string>{}(config_.self_id);
    std::uint64_t t =
        static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    std::seed_seq seed{static_cast<std::uint32_t>(h),
                       static_cast<std::uint32_t>(h >> 32),
                       static_cast<std::uint32_t>(t),
                       static_cast<std::uint32_t>(t >> 32)};
    rng_.seed(seed);

    last_heartbeat_sent_ = Clock::now();
    reset_election_deadline_locked();  // safe: single-threaded construction
}

RaftNode::~RaftNode() { stop(); }

void RaftNode::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
    }
    thread_ = std::thread([this] { tick_loop(); });
}

void RaftNode::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void RaftNode::tick_loop() {
    for (;;) {
        bool do_election = false;
        bool do_heartbeat = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(kTickMs),
                         [this] { return !running_; });
            if (!running_) {
                return;
            }
            Clock::time_point now = Clock::now();
            if (role_ == Role::Leader) {
                if (now - last_heartbeat_sent_ >=
                    std::chrono::milliseconds(config_.heartbeat_ms)) {
                    do_heartbeat = true;
                    last_heartbeat_sent_ = now;
                }
            } else if (now >= election_deadline_) {
                do_election = true;
            }
        }
        // Network I/O happens WITHOUT the lock held.
        if (do_heartbeat) {
            send_heartbeats();
        }
        if (do_election) {
            start_election();
        }
    }
}

void RaftNode::start_election() {
    long term;
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        role_ = Role::Candidate;
        current_term_ += 1;
        term = current_term_;
        voted_for_ = config_.self_id;  // vote for self
        leader_id_.clear();
        reset_election_deadline_locked();
        peers = config_.peers;
    }
    std::cerr << "[raft] " << config_.self_id << " starting election, term "
              << term << "\n";

    std::size_t votes = 1;  // our own
    bool won = false;
    for (const std::string& peer : peers) {
        long reply_term = 0;
        bool granted = false;
        if (!send_request_vote(peer, term, &reply_term, &granted)) {
            continue;  // peer unreachable; carry on (we may still get a majority)
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_term_ != term || role_ != Role::Candidate) {
            return;  // a newer term or a heartbeat superseded this election
        }
        if (reply_term > current_term_) {
            become_follower_locked(reply_term);
            return;
        }
        if (granted && ++votes >= majority()) {
            role_ = Role::Leader;
            leader_id_ = config_.self_id;
            last_heartbeat_sent_ = Clock::time_point{};  // force an immediate beat
            won = true;
            std::cerr << "[raft] " << config_.self_id << " WON election, term "
                      << term << "\n";
            break;
        }
    }
    if (won) {
        send_heartbeats();  // assert authority right away
    }
}

void RaftNode::send_heartbeats() {
    long term;
    std::vector<std::string> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ != Role::Leader) {
            return;
        }
        term = current_term_;
        peers = config_.peers;
        last_heartbeat_sent_ = Clock::now();
    }
    for (const std::string& peer : peers) {
        long reply_term = 0;
        bool success = false;
        if (!send_append_entries(peer, term, &reply_term, &success)) {
            continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ != Role::Leader || current_term_ != term) {
            return;
        }
        if (reply_term > current_term_) {
            become_follower_locked(reply_term);  // someone has a newer term
            return;
        }
    }
}

void RaftNode::become_follower_locked(long term) {
    role_ = Role::Follower;
    current_term_ = term;
    voted_for_.clear();
    leader_id_.clear();
    reset_election_deadline_locked();
}

void RaftNode::reset_election_deadline_locked() {
    std::uniform_int_distribution<int> dist(config_.election_timeout_min_ms,
                                            config_.election_timeout_max_ms);
    election_deadline_ = Clock::now() + std::chrono::milliseconds(dist(rng_));
}

std::string RaftNode::handle_request_vote(long term,
                                          const std::string& candidate_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (term > current_term_) {
        become_follower_locked(term);  // newer term: step down, clear our vote
    }
    bool granted = false;
    // Grant a vote at most once per term (and the candidate's term must be
    // current). Raft also requires the candidate's log be at least as up-to-date
    // as ours; this election-only build has no log, so that check is omitted.
    if (term == current_term_ &&
        (voted_for_.empty() || voted_for_ == candidate_id)) {
        voted_for_ = candidate_id;
        granted = true;
        reset_election_deadline_locked();  // granting a vote defers our own bid
    }
    return "__VOTE__ " + std::to_string(current_term_) + " " +
           (granted ? "1" : "0");
}

std::string RaftNode::handle_append_entries(long term,
                                            const std::string& leader_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool success = false;
    if (term >= current_term_) {
        if (term > current_term_) {
            current_term_ = term;
            voted_for_.clear();
        }
        role_ = Role::Follower;        // a current leader exists: stand down
        leader_id_ = leader_id;
        reset_election_deadline_locked();
        success = true;
    }
    return "__APPENDED__ " + std::to_string(current_term_) + " " +
           (success ? "1" : "0");
}

bool RaftNode::send_request_vote(const std::string& peer, long term,
                                 long* reply_term, bool* granted) {
    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(peer, &host, &port)) {
        return false;
    }
    std::vector<std::string> tok;
    std::string req = "__RAFT_VOTE__ " + std::to_string(term) + " " +
                      config_.self_id + "\n";
    if (!rpc(host, port, config_.rpc_timeout_ms, req, &tok)) {
        return false;
    }
    if (tok.size() < 3 || tok[0] != "__VOTE__") {
        return false;
    }
    try {
        *reply_term = std::stol(tok[1]);
    } catch (const std::exception&) {
        return false;
    }
    *granted = (tok[2] == "1");
    return true;
}

bool RaftNode::send_append_entries(const std::string& peer, long term,
                                   long* reply_term, bool* success) {
    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(peer, &host, &port)) {
        return false;
    }
    std::vector<std::string> tok;
    std::string req = "__RAFT_APPEND__ " + std::to_string(term) + " " +
                      config_.self_id + "\n";
    if (!rpc(host, port, config_.rpc_timeout_ms, req, &tok)) {
        return false;
    }
    if (tok.size() < 3 || tok[0] != "__APPENDED__") {
        return false;
    }
    try {
        *reply_term = std::stol(tok[1]);
    } catch (const std::exception&) {
        return false;
    }
    *success = (tok[2] == "1");
    return true;
}

bool RaftNode::is_leader() {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_ == Role::Leader;
}

std::string RaftNode::leader_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return leader_id_;
}

long RaftNode::current_term() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_term_;
}

RaftNode::Role RaftNode::role() {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
}

std::string RaftNode::status_line() {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* r = role_ == Role::Leader
                        ? "leader"
                        : (role_ == Role::Candidate ? "candidate" : "follower");
    std::string ldr = leader_id_.empty() ? "(none)" : leader_id_;
    return std::string("role=") + r + " term=" + std::to_string(current_term_) +
           " leader=" + ldr;
}

}  // namespace redon
