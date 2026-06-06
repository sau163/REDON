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
#include <string>

namespace redon {
namespace {

// Remove leading and trailing whitespace (spaces, tabs, and the trailing '\r'
// that arrives when a client sends Windows-style "\r\n" line endings).
std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(s[begin]))) {
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
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
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
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    std::string token = s.substr(start, i - start);
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
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
                         bool* should_close) {
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

    if (verb == "SET") {
        // Needs a key and a non-empty value.
        if (key.empty() || rest.empty()) {
            return wrong_args("set");
        }
        store.set(key, rest);
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
        return "(integer) " + std::to_string(store.del(key));
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
