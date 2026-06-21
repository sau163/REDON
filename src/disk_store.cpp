// disk_store.cpp — implementation of the log-structured disk storage engine.
#include "disk_store.h"

#include <cstdint>
#include <filesystem>
#include <ios>
#include <system_error>
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
    // Empty keys/values are rejected so set() and recover() agree on what's a
    // valid record (recover() treats a zero length as corruption). The protocol
    // already forbids them; this keeps the two halves consistent.
    if (!ok_ || key.empty() || value.empty()) {
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
    if (!ok_ || key.empty()) {
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

bool DiskStore::clear() {
    index_.clear();
    file_.close();
    // Truncate by reopening with trunc, then reopen for normal read+write.
    { std::ofstream trunc(path_, std::ios::out | std::ios::trunc | std::ios::binary); }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    ok_ = file_.is_open();
    return ok_;
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

    std::streamoff last_good = 0;  // byte offset just past the last COMPLETE record
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
        last_good = file_.tellg();  // this record parsed cleanly to here
    }
    file_.clear();

    // Heal a corrupt/truncated tail: physically drop everything past the last
    // complete record. Otherwise the next append would land AFTER the garbage,
    // and a later restart would stop at the garbage again and lose those writes.
    file_.seekg(0, std::ios::end);
    std::streamoff file_size = file_.tellg();
    file_.clear();
    if (file_size > last_good) {
        file_.close();
        std::error_code ec;
        std::filesystem::resize_file(
            path_, static_cast<std::uintmax_t>(last_good), ec);
        file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
        ok_ = file_.is_open();
        if (!ok_) {
            return;
        }
        file_size = last_good;
    }

    // Compact if the file is mostly garbage (lots of overwrites/deletes).
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
    bool failed = false;
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) {
            return;  // can't even open the temp; keep the existing file
        }
        for (const auto& kv : index_) {
            std::string value;
            if (!read_value(kv.second, &value)) {
                failed = true;  // a read failed: abort rather than DROP a live key
                break;
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
            failed = true;
        }
    }

    std::error_code ec;
    if (failed) {
        std::filesystem::remove(tmp, ec);  // abandon compaction; original intact
        return;
    }

    // Atomically replace the data file with the compacted one. std::filesystem::
    // rename overwrites the destination in one step (no remove-then-rename gap),
    // so on failure the original file is untouched and the index stays valid.
    file_.close();
    std::filesystem::rename(tmp, path_, ec);
    if (!ec) {
        index_ = std::move(new_index);  // adopt the new offsets
    } else {
        std::filesystem::remove(tmp, ec);  // keep the old file and its index
    }
    file_.open(path_, std::ios::in | std::ios::out | std::ios::binary);
    ok_ = file_.is_open();
}

}  // namespace redon
