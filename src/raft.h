// raft.h — Raft leader election: the cluster agrees on WHO is the leader.
//
// Phase 5 had a fixed leader chosen by the operator; if it died, a human had to
// promote a follower. Phase 6 makes that automatic with the election half of the
// Raft consensus algorithm. Each node is a Follower, Candidate, or Leader:
//
//   * A Follower expects regular heartbeats from a Leader. If none arrive within
//     a randomized "election timeout", it suspects the Leader is dead.
//   * It then becomes a Candidate: it bumps the TERM (a logical clock /
//     generation number), votes for itself, and asks every peer for a vote
//     (RequestVote RPC).
//   * A peer grants its vote at most once per term. A Candidate that collects a
//     MAJORITY (quorum) becomes the Leader.
//   * The Leader sends heartbeats (AppendEntries RPC, here with no log entries)
//     to suppress new elections. If anyone reports a higher term, the Leader
//     steps down.
//
// Randomized timeouts make split votes rare; the majority rule guarantees at
// most one Leader per term. This file implements LEADER ELECTION only — Raft's
// log replication (moving the actual data through the elected leader) is the
// natural next step and is described in docs/PHASE6.md.
#ifndef REDON_RAFT_H
#define REDON_RAFT_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace redon {

class RaftNode {
public:
    enum class Role { Follower, Candidate, Leader };

    struct Config {
        std::string self_id;             // this node's "host:port"
        std::vector<std::string> peers;  // the other nodes' "host:port"
        int heartbeat_ms = 100;          // leader heartbeat period
        int election_timeout_min_ms = 400;
        int election_timeout_max_ms = 800;
        int rpc_timeout_ms = 120;        // per-RPC send/recv timeout
    };

    explicit RaftNode(Config config);
    ~RaftNode();

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    void start();  // spawn the election/heartbeat timer thread
    void stop();   // signal it to stop and join

    // Inbound RPC handlers, invoked by the command layer when a peer's RPC line
    // arrives. Each returns the reply line (without trailing newline).
    std::string handle_request_vote(long term, const std::string& candidate_id);
    std::string handle_append_entries(long term, const std::string& leader_id);

    // For write routing and the ROLE status command.
    bool is_leader();
    std::string leader_id();        // current known leader, "" if none
    long current_term();
    Role role();
    std::string status_line();      // "role=<r> term=<t> leader=<id>"

private:
    using Clock = std::chrono::steady_clock;

    void tick_loop();
    void start_election();          // network I/O; takes/releases the lock itself
    void send_heartbeats();         // network I/O; takes/releases the lock itself

    // Helpers that REQUIRE the caller to already hold mutex_.
    void become_follower_locked(long term);
    void reset_election_deadline_locked();
    std::size_t majority() const { return (config_.peers.size() + 1) / 2 + 1; }

    // Send one RPC to `peer`; fills reply term/flag. Returns false if the peer
    // could not be reached or sent a malformed reply. Does NOT hold the lock.
    bool send_request_vote(const std::string& peer, long term, long* reply_term,
                           bool* granted);
    bool send_append_entries(const std::string& peer, long term,
                             long* reply_term, bool* success);

    Config config_;

    std::mutex mutex_;
    std::condition_variable cv_;
    Role role_ = Role::Follower;
    long current_term_ = 0;
    std::string voted_for_;                 // "" = haven't voted this term
    std::string leader_id_;                 // "" = unknown
    Clock::time_point election_deadline_;   // start an election at/after this
    Clock::time_point last_heartbeat_sent_;
    std::mt19937 rng_;
    bool running_ = false;
    std::thread thread_;
};

}  // namespace redon

#endif  // REDON_RAFT_H
