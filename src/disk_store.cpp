// disk_store.cpp — implementation of the log-structured disk storage engine.
#include "disk_store.h"

#include <cstdio>   // std::remove, std::rename
#include <ios>
#include <utility>

namespace redon {
namespace {

// Reject an absurd field length from a corrupt/truncated file rather than trying
// to allocate gigabytes (same guard as the WAL).
constexpr std::size_t kMaxField = 256u * 1024 * 1024;

}  // namespace

DiskStore::DiskStore(std::string path) : path_(std::move(path)) {
    // Make sure the file exists, then open it for both reading and writing.
    { std::ofstream create(path_, std::ios::app | std::ios::binary); }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    ok_ = file_.is_open();
    if (ok_) {
        recover();
    }
}

bool DiskStore::read_value(const Entry& e, std::string* out) {
    file_.clear();  // clear any EOF/fail bits from a previous op
    file_.seekg(e.value_pos);
    std::string buf(e.value_len, '\0');
    if (e.value_len > 0) {
        file_.read(&buf[0], static_cast<std::streamsize>(e.value_len));
    }
    if (!file_) {
        file_.clear();
        return false;
    }
    *out = std::move(buf);
    return true;
}

bool DiskStore::set(const std::string& key, const std::string& value) {
    if (!ok_) {
        return false;
    }
    file_.clear();
    file_.seekp(0, std::ios::end);  // append
    file_ << "SET " << key.size() << " " << value.size() << " ";
    file_.write(key.data(), static_cast<std::streamsize>(key.size()));
    const std::streampos value_pos = file_.tellp();  // value starts here
    file_.write(value.data(), static_cast<std::streamsize>(value.size()));
    file_.put('\n');
    file_.flush();  // durable before we acknowledge
    if (!file_) {
        ok_ = false;
        return false;
    }
    index_[key] = Entry{value_pos, value.size()};
    return true;
}

bool DiskStore::get(const std::string& key, std::string* out) {
    if (!ok_) {
        return false;
    }
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    return read_value(it->second, out);
}

bool DiskStore::del(const std::string& key) {
    if (!ok_) {
        return false;
    }
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;  // nothing to remove
    }
    file_.clear();
    file_.seekp(0, std::ios::end);
    file_ << "DEL " << key.size() << " ";
    file_.write(key.data(), static_cast<std::streamsize>(key.size()));
    file_.put('\n');
    file_.flush();
    if (!file_) {
        ok_ = false;
        return false;
    }
    index_.erase(it);
    return true;
}

void DiskStore::clear() {
    index_.clear();
    file_.close();
    // Truncate by reopening with trunc, then reopen for normal read+write.
    { std::ofstream trunc(path_, std::ios::out | std::ios::trunc | std::ios::binary); }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    ok_ = file_.is_open();
}

std::vector<std::pair<std::string, std::string>> DiskStore::snapshot() {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(index_.size());
    for (const auto& kv : index_) {
        std::string value;
        if (read_value(kv.second, &value)) {
            out.emplace_back(kv.first, std::move(value));
        }
    }
    return out;
}

void DiskStore::recover() {
    file_.clear();
    file_.seekg(0, std::ios::beg);

    std::string op;
    while (file_ >> op) {  // reads a whitespace-delimited token
        if (op == "SET") {
            std::size_t klen = 0;
            std::size_t vlen = 0;
            if (!(file_ >> klen >> vlen)) break;
            if (klen == 0 || vlen == 0 || klen > kMaxField || vlen > kMaxField) {
                break;
            }
            if (file_.get() != ' ') break;
            std::string key(klen, '\0');
            file_.read(&key[0], static_cast<std::streamsize>(klen));
            if (!file_) break;
            const std::streampos value_pos = file_.tellg();  // value starts here
            file_.seekg(static_cast<std::streamoff>(vlen), std::ios::cur);  // skip it
            if (!file_) break;
            if (file_.get() != '\n') break;
            index_[key] = Entry{value_pos, vlen};
        } else if (op == "DEL") {
            std::size_t klen = 0;
            if (!(file_ >> klen)) break;
            if (klen == 0 || klen > kMaxField) break;
            if (file_.get() != ' ') break;
            std::string key(klen, '\0');
            file_.read(&key[0], static_cast<std::streamsize>(klen));
            if (!file_) break;
            if (file_.get() != '\n') break;
            index_.erase(key);
        } else {
            break;  // corruption: stop, keeping every complete record before it
        }
    }
    file_.clear();  // clear EOF so later reads/writes work

    // Compact if the file is mostly garbage (lots of overwrites/deletes).
    file_.seekg(0, std::ios::end);
    const std::streamoff file_size = file_.tellg();
    file_.clear();
    std::size_t live_bytes = 0;
    for (const auto& kv : index_) {
        live_bytes += kv.first.size() + kv.second.value_len + 24;  // ~header+nl
    }
    if (file_size > 0 &&
        static_cast<std::size_t>(file_size) > 2 * live_bytes + 4096) {
        compact();
    }
}

void DiskStore::compact() {
    const std::string tmp = path_ + ".tmp";
    std::unordered_map<std::string, Entry> new_index;
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) {
            return;  // can't compact; keep the existing file
        }
        for (const auto& kv : index_) {
            std::string value;
            if (!read_value(kv.second, &value)) {
                continue;
            }
            out << "SET " << kv.first.size() << " " << value.size() << " ";
            out.write(kv.first.data(),
                      static_cast<std::streamsize>(kv.first.size()));
            const std::streampos value_pos = out.tellp();
            out.write(value.data(), static_cast<std::streamsize>(value.size()));
            out.put('\n');
            new_index[kv.first] = Entry{value_pos, value.size()};
        }
        out.flush();
        if (!out) {
            return;  // write failed; keep the existing file
        }
    }

    // Replace the data file with the compacted one and adopt the new offsets.
    // (There is a brief window between remove and rename where a crash would lose
    // the file; a production engine renames atomically. Acceptable here, and this
    // runs only at startup before serving clients.)
    file_.close();
    std::remove(path_.c_str());
    if (std::rename(tmp.c_str(), path_.c_str()) == 0) {
        index_ = std::move(new_index);
    }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    ok_ = file_.is_open();
}

}  // namespace redon
