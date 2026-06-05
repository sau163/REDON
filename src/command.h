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

// Parse one line of input, run it against `store`, and return the reply text to
// send back to the client. The returned string does NOT include a trailing
// newline — the caller (server) adds the line terminator when it sends.
//
// `*should_close` is set to true when the command asks to end the connection
// (QUIT/EXIT). Callers pass the address of a bool they own; it is always written.
std::string execute_line(const std::string& line, Storage& store,
                         bool* should_close);

}  // namespace redon

#endif  // REDON_COMMAND_H
