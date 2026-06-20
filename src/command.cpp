// command.cpp — parse one line of the Phase 1 protocol and execute it.
//
// Grammar (one command per line):
//   SET <key> <value...>   value is the rest of the line; spaces allowed
//   GET <key>
//   DEL <key>              (DELETE is accepted as a synonym)
//   EXISTS <key>
//   PING
//   QUIT                   (EXIT is accepted as a synonym)
//
// Verbs are case-insensitive ("set", "Set", "SET" all work). Replies imitate
// redis-cli so the output feels familiar.
#include "command.h"

#include <cctype>
#include <stdexcept>
#include <string>

#include "raft.h"

namespace redon {
namespace {

// Whitespace for tokenizing a command — an EXPLICIT ASCII set, deliberately not
// std::isspace, which is locale-dependent: a high byte (e.g. 0xA0) is whitespace
// in some locales but not others. A key-value store must split keys the same way
// everywhere, regardless of a process's LC_* settings, or the router (which
// hashes the key) and the shard (which stores it) could disagree on a non-ASCII
// key's boundary. (Phase 7 router uses the same set in server.cpp.)
bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

// Remove leading and trailing whitespace (spaces, tabs, and the trailing '\r'
// that arrives when a client sends Windows-style "\r\n" line endings).
std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && is_space(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    // Skip a leading UTF-8 byte-order mark (EF BB BF) if present. Editors like
    // Windows Notepad prepend one when saving "UTF-8" files, and isspace()
    // doesn't treat it as whitespace, so without this the first command of a
    // piped file would parse as an unknown verb.
    if (end - begin >= 3 &&
        static_cast<unsigned char>(s[begin]) == 0xEF &&
        static_cast<unsigned char>(s[begin + 1]) == 0xBB &&
        static_cast<unsigned char>(s[begin + 2]) == 0xBF) {
        begin += 3;
    }
    while (end > begin && is_space(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// Read the next whitespace-delimited token from `s` starting at `pos`, advancing
// `pos` past the token and any whitespace that follows it. Returns "" when there
// is no token left.
std::string next_token(const std::string& s, std::size_t* pos) {
    std::size_t i = *pos;
    std::size_t start = i;
    while (i < s.size() && !is_space(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::string token = s.substr(start, i - start);
    while (i < s.size() && is_space(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    *pos = i;
    return token;
}

std::string wrong_args(const std::string& verb_lower) {
    return "ERR wrong number of arguments for '" + verb_lower + "' command";
}

}  // namespace

std::string execute_line(const std::string& line, Storage& store,
                         bool* should_close, bool node_is_follower,
                         bool* is_replica_link, RaftNode* raft) {
    *should_close = false;

    const std::string s = trim(line);
    if (s.empty()) {
        // Blank line (e.g. the user just pressed Enter): nothing to do. We still
        // return a reply string; the server adds a newline, yielding a blank
        // line back, which is harmless.
        return "";
    }

    std::size_t pos = 0;
    const std::string verb = to_upper(next_token(s, &pos));
    const std::string key = next_token(s, &pos);
    const std::string rest = s.substr(pos);  // everything after the key

    // Replication handshake: mark this connection as the leader's replica link
    // and reset the follower's data for a fresh full sync. Guarded so an
    // ordinary client can't use it to wipe data:
    //   * only a follower accepts it (a leader's authoritative data is never
    //     cleared by a stray client sending __REPLSYNC__),
    //   * only once per connection (a replayed handshake can't clear a sync that
    //     is already in progress).
    if (verb == "__REPLSYNC__") {
        if (!node_is_follower) {
            return "ERR __REPLSYNC__ is only valid on a read-only replica";
        }
        if (is_replica_link != nullptr && *is_replica_link) {
            return "ERR replication sync already in progress on this connection";
        }
        if (is_replica_link != nullptr) {
            *is_replica_link = true;
        }
        store.clear();
        return "OK";
    }

    // Raft inter-node RPCs (Phase 6). These travel over the same line protocol
    // between cluster nodes; dispatch them to the local Raft state machine.
    if (verb == "__RAFT_VOTE__" || verb == "__RAFT_APPEND__") {
        if (raft == nullptr) {
            return "ERR this node is not part of a Raft cluster";
        }
        long term = 0;
        try {
            term = std::stol(key);
        } catch (const std::exception&) {
            return "ERR malformed Raft RPC";
        }
        return verb == "__RAFT_VOTE__" ? raft->handle_request_vote(term, rest)
                                       : raft->handle_append_entries(term, rest);
    }
    // Report this node's Raft role/term/leader (handy for demos and clients).
    if (verb == "ROLE") {
        return raft != nullptr ? raft->status_line()
                               : std::string("standalone (not a Raft cluster)");
    }

    // A follower is read-only to ordinary clients: only writes arriving on the
    // replication link (from the leader) are applied.
    const bool replica_link = (is_replica_link != nullptr && *is_replica_link);
    if (node_is_follower && !replica_link &&
        (verb == "SET" || verb == "DEL" || verb == "DELETE")) {
        return "ERR READONLY this node is a read-only replica";
    }

    // In a Raft cluster only the elected leader accepts writes; point clients at
    // the current leader otherwise.
    if (raft != nullptr && !raft->is_leader() &&
        (verb == "SET" || verb == "DEL" || verb == "DELETE")) {
        const std::string leader = raft->leader_id();
        return "ERR NOTLEADER " + (leader.empty() ? std::string("(unknown)")
                                                  : leader);
    }

    if (verb == "SET") {
        // Needs a key and a non-empty value.
        if (key.empty() || rest.empty()) {
            return wrong_args("set");
        }
        if (!store.set(key, rest)) {
            // The durable write failed: never claim success we can't back.
            return "ERR write failed: data not persisted";
        }
        return "OK";
    }

    if (verb == "GET") {
        if (key.empty() || !rest.empty()) {
            return wrong_args("get");
        }
        std::string value;
        if (store.get(key, &value)) {
            return value;
        }
        return "(nil)";
    }

    if (verb == "DEL" || verb == "DELETE") {
        if (key.empty() || !rest.empty()) {
            return wrong_args("del");
        }
        bool durable = true;
        std::size_t removed = store.del(key, &durable);
        if (!durable) {
            return "ERR write failed: data not persisted";
        }
        return "(integer) " + std::to_string(removed);
    }

    if (verb == "EXISTS") {
        if (key.empty() || !rest.empty()) {
            return wrong_args("exists");
        }
        return "(integer) " + std::to_string(store.exists(key) ? 1 : 0);
    }

    if (verb == "PING") {
        if (!key.empty() || !rest.empty()) {
            return wrong_args("ping");
        }
        return "PONG";
    }

    if (verb == "QUIT" || verb == "EXIT") {
        if (!key.empty() || !rest.empty()) {
            return wrong_args("quit");
        }
        *should_close = true;
        return "OK";
    }

    return "ERR unknown command '" + verb + "'";
}

}  // namespace redon
