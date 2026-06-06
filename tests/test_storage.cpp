// test_storage.cpp — unit tests for the Storage engine.
#include <cstddef>
#include <string>

#include "storage.h"
#include "test_util.h"

using redon::Storage;

void test_set_then_get() {
    Storage s;
    std::string value;
    CHECK(!s.get("missing", &value));  // absent before anything is set
    s.set("name", "Saurabh");
    CHECK(s.get("name", &value));
    CHECK_EQ(value, std::string("Saurabh"));
    CHECK(s.exists("name"));
    CHECK_EQ(s.size(), static_cast<std::size_t>(1));
}

void test_overwrite_keeps_one_entry() {
    Storage s;
    s.set("k", "one");
    s.set("k", "two");  // overwrite, not insert
    std::string value;
    CHECK(s.get("k", &value));
    CHECK_EQ(value, std::string("two"));
    CHECK_EQ(s.size(), static_cast<std::size_t>(1));
}

void test_del_reports_count() {
    Storage s;
    s.set("k", "v");
    CHECK_EQ(s.del("k"), static_cast<std::size_t>(1));  // removed exactly one
    CHECK_EQ(s.del("k"), static_cast<std::size_t>(0));  // already gone
    CHECK(!s.exists("k"));
    std::string value;
    CHECK(!s.get("k", &value));
}

void test_get_leaves_out_param_untouched_when_missing() {
    Storage s;
    std::string value = "sentinel";
    CHECK(!s.get("nope", &value));
    CHECK_EQ(value, std::string("sentinel"));  // must not be modified
}

int main() {
    RUN(test_set_then_get);
    RUN(test_overwrite_keeps_one_entry);
    RUN(test_del_reports_count);
    RUN(test_get_leaves_out_param_untouched_when_missing);
    return REPORT();
}
