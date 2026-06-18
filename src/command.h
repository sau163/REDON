// command.h — the protocol layer: turn a line of text into an action and run it.
//
// Phase 1 protocol = one command per line, e.g.  "SET name Saurabh".
// This header exposes a single function, execute_line(), which the server calls
// for each complete line it receives. Keeping parse + execute together means new
// commands in later phases touch only command.cpp.
#ifndef REDON_COMMAND_H
#define REDON_COMMAND_H

#include <string>

#include "storage.h"

namespace redon {

class RaftNode;  // forward declaration; command.cpp includes raft.h

// Parse one line of input, run it against `store`, and return the reply text to
// send back to the client. The returned string does NOT include a trailing
// newline — the caller (server) adds the line terminator when it sends.
//
// `*should_close` is set to true when the command asks to end the connection
// (QUIT/EXIT). Callers pass the address of a bool they own; it is always written.
//
// Replication parameters (Phase 5):
//   `node_is_follower` — true on a follower node, which is read-only to ordinary
//       clients (SET/DEL are rejected) unless they arrive on the replication link.
//   `is_replica_link` — per-connection flag (owned by the caller). The leader's
//       replication handshake (__REPLSYNC__) sets it true, after which writes on
//       that connection are accepted as the replicated stream.
// Raft parameter (Phase 6):
//   `raft` — if non-null, this node is in a Raft cluster: inter-node RPCs are
//       dispatched to it, the ROLE command reports its status, and client writes
//       are accepted only when this node is the elected leader.
std::string execute_line(const std::string& line, Storage& store,
                         bool* should_close, bool node_is_follower = false,
                         bool* is_replica_link = nullptr,
                         RaftNode* raft = nullptr);

}  // namespace redon

#endif  // REDON_COMMAND_H
