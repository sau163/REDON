// test_command.cpp — tests for the protocol layer (parsing + execution).
//
// execute_line() is pure with respect to a Storage object — no sockets — so we
// can exercise the entire command language here without any networking.
#include <string>

#include "command.h"
#include "storage.h"
#include "test_util.h"

using redon::execute_line;
using redon::Storage;

// Helper: run one line against `store`, optionally reporting the close flag.
static std::string run(Storage& store, const std::string& line,
                       bool* closed = nullptr) {
    bool c = false;
    std::string reply = execute_line(line, store, &c);
    if (closed != nullptr) {
        *closed = c;
    }
    return reply;
}

void test_basic_flow() {
    Storage s;
    CHECK_EQ(run(s, "SET name Saurabh"), std::string("OK"));
    CHECK_EQ(run(s, "GET name"), std::string("Saurabh"));
    CHECK_EQ(run(s, "EXISTS name"), std::string("(integer) 1"));
    CHECK_EQ(run(s, "DEL name"), std::string("(integer) 1"));
    CHECK_EQ(run(s, "GET name"), std::string("(nil)"));
    CHECK_EQ(run(s, "EXISTS name"), std::string("(integer) 0"));
}

void test_value_may_contain_spaces() {
    Storage s;
    CHECK_EQ(run(s, "SET title Senior Software Engineer"), std::string("OK"));
    CHECK_EQ(run(s, "GET title"), std::string("Senior Software Engineer"));
}

void test_verbs_case_insensitive_and_synonyms() {
    Storage s;
    CHECK_EQ(run(s, "set k v"), std::string("OK"));
    CHECK_EQ(run(s, "GeT k"), std::string("v"));
    CHECK_EQ(run(s, "delete k"), std::string("(integer) 1"));  // DELETE == DEL
}

void test_crlf_is_tolerated() {
    Storage s;
    CHECK_EQ(run(s, "SET k v\r"), std::string("OK"));  // trailing CR trimmed
    CHECK_EQ(run(s, "GET k\r"), std::string("v"));
}

void test_extra_whitespace_is_tolerated() {
    Storage s;
    CHECK_EQ(run(s, "   SET   k   hello world  "), std::string("OK"));
    CHECK_EQ(run(s, "GET k"), std::string("hello world"));
}

void test_ping_and_quit() {
    Storage s;
    CHECK_EQ(run(s, "PING"), std::string("PONG"));

    bool closed = false;
    CHECK_EQ(run(s, "QUIT", &closed), std::string("OK"));
    CHECK(closed);  // QUIT must request connection close

    closed = false;
    run(s, "GET k", &closed);
    CHECK(!closed);  // ordinary commands must not request close
}

void test_argument_and_unknown_errors() {
    Storage s;
    CHECK_EQ(run(s, "SET"),
             std::string("ERR wrong number of arguments for 'set' command"));
    CHECK_EQ(run(s, "SET onlykey"),
             std::string("ERR wrong number of arguments for 'set' command"));
    CHECK_EQ(run(s, "GET"),
             std::string("ERR wrong number of arguments for 'get' command"));
    CHECK_EQ(run(s, "GET a b"),
             std::string("ERR wrong number of arguments for 'get' command"));
    CHECK_EQ(run(s, "BOGUS x"), std::string("ERR unknown command 'BOGUS'"));
    CHECK_EQ(run(s, ""), std::string(""));  // blank line -> empty reply
}

int main() {
    RUN(test_basic_flow);
    RUN(test_value_may_contain_spaces);
    RUN(test_verbs_case_insensitive_and_synonyms);
    RUN(test_crlf_is_tolerated);
    RUN(test_extra_whitespace_is_tolerated);
    RUN(test_ping_and_quit);
    RUN(test_argument_and_unknown_errors);
    return REPORT();
}
