// test_util.h — a tiny zero-dependency test harness.
//
// We avoid pulling in a framework (GoogleTest, Catch2) for Phase 1 so the build
// stays trivial. Each test executable is a normal program with a main() that
// runs some check functions and returns 0 if all CHECKs passed, 1 otherwise —
// exactly what CTest expects.
#ifndef REDON_TEST_UTIL_H
#define REDON_TEST_UTIL_H

#include <iostream>

namespace redon_test {
// One failure counter per test program (each test exe is its own process).
inline int& failures() {
    static int count = 0;
    return count;
}
}  // namespace redon_test

// Assert a boolean condition.
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "  FAIL: " #cond "   @ " __FILE__ ":" << __LINE__ \
                      << "\n";                                            \
            ++redon_test::failures();                                     \
        }                                                                 \
    } while (0)

// Assert two values are equal, printing both when they differ.
#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        auto _a = (actual);                                               \
        auto _e = (expected);                                             \
        if (!(_a == _e)) {                                                \
            std::cerr << "  FAIL: " #actual " == " #expected "   got <"   \
                      << _a << "> expected <" << _e << ">   @ " __FILE__  \
                         ":"                                              \
                      << __LINE__ << "\n";                                \
            ++redon_test::failures();                                     \
        }                                                                 \
    } while (0)

// Run a named test function with a small heading.
#define RUN(test_fn)                              \
    do {                                          \
        std::cout << "running " #test_fn "\n";    \
        test_fn();                                \
    } while (0)

// Print a summary and yield the process exit code (0 = success).
#define REPORT()                                                       \
    (redon_test::failures() == 0                                       \
         ? (std::cout << "ALL TESTS PASSED\n", 0)                      \
         : (std::cerr << redon_test::failures() << " CHECK(s) FAILED\n", \
            1))

#endif  // REDON_TEST_UTIL_H
