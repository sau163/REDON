// test_raft.cpp — deterministic tests of the Raft RPC handlers.
//
// The election itself is timing- and network-driven (covered by the end-to-end
// demo), but the RPC handlers — the heart of Raft's safety — are pure state
// transitions we can test directly. We construct a RaftNode but never start() it,
// so there is no timer thread and no networking: just the term/vote logic.
#include <string>

#include "raft.h"
#include "test_util.h"

using redon::RaftNode;

namespace {
RaftNode::Config make_config() {
    RaftNode::Config c;
    c.self_id = "self:1";
    c.peers = {"peer:2", "peer:3"};  // 3-node cluster: majority is 2
    return c;
}
}  // namespace

// A node grants its vote at most once per term.
void test_vote_granted_once_per_term() {
    RaftNode node(make_config());
    CHECK_EQ(node.current_term(), static_cast<long>(0));
    CHECK(node.role() == RaftNode::Role::Follower);

    // A request in a newer term is granted (and bumps our term).
    CHECK_EQ(node.handle_request_vote(1, "A"), std::string("__VOTE__ 1 1"));
    CHECK_EQ(node.current_term(), static_cast<long>(1));
    // Same term, a different candidate: we already voted -> denied.
    CHECK_EQ(node.handle_request_vote(1, "B"), std::string("__VOTE__ 1 0"));
    // Same term, the SAME candidate again: idempotently granted.
    CHECK_EQ(node.handle_request_vote(1, "A"), std::string("__VOTE__ 1 1"));
}

// A higher term resets our vote so a new candidate can win it; an older term is
// rejected.
void test_higher_term_steps_down_and_revotes() {
    RaftNode node(make_config());
    node.handle_request_vote(1, "A");  // term 1, voted for A
    CHECK_EQ(node.handle_request_vote(2, "B"), std::string("__VOTE__ 2 1"));
    CHECK_EQ(node.current_term(), static_cast<long>(2));
    // A stale request (older term) is denied, reporting our higher term.
    CHECK_EQ(node.handle_request_vote(1, "C"), std::string("__VOTE__ 2 0"));
}

// AppendEntries (heartbeat) term rules: reject older, accept current/newer,
// recognise the leader, and stay a follower.
void test_append_entries_term_rules() {
    RaftNode node(make_config());
    node.handle_request_vote(2, "B");  // advance to term 2

    // Stale heartbeat: rejected, leader unchanged.
    CHECK_EQ(node.handle_append_entries(1, "X"), std::string("__APPENDED__ 2 0"));
    CHECK(node.leader_id().empty());

    // Current heartbeat: accepted; we recognise leader L and remain a follower.
    CHECK_EQ(node.handle_append_entries(2, "L"), std::string("__APPENDED__ 2 1"));
    CHECK_EQ(node.leader_id(), std::string("L"));
    CHECK(node.role() == RaftNode::Role::Follower);

    // Newer-term heartbeat: bump the term and switch leader.
    CHECK_EQ(node.handle_append_entries(3, "M"), std::string("__APPENDED__ 3 1"));
    CHECK_EQ(node.current_term(), static_cast<long>(3));
    CHECK_EQ(node.leader_id(), std::string("M"));

    // A now-stale vote request is denied with our higher term.
    CHECK_EQ(node.handle_request_vote(2, "Z"), std::string("__VOTE__ 3 0"));
    CHECK(!node.is_leader());
}

int main() {
    RUN(test_vote_granted_once_per_term);
    RUN(test_higher_term_steps_down_and_revotes);
    RUN(test_append_entries_term_rules);
    return REPORT();
}
