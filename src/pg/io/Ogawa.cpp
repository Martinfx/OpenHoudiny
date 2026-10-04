#include "pg/io/Ogawa.h"

#include <algorithm>
#include <cstring>

namespace pg::io::ogawa {

namespace {

/// "Ogawa", 0 while it is being written, the version 0 1.
constexpr char kMagic[5] = {'O', 'g', 'a', 'w', 'a'};
constexpr uint8_t kWhole = 0xff;
constexpr uint64_t kHeader = 16;

uint64_t u64At(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

}  // namespace

// --- Writer ----------------------------------------------------------------------------------

Writer::~Writer() {
    if (file_.is_open()) file_.close();
}

bool Writer::open(const std::string& path, std::string& error) {
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_) {
        error = path + ": cannot write it";
        return false;
    }
    toMemory_ = false;
    failed_ = false;
    open_ = true;
    size_ = 0;
    // The header, not whole yet; the root's place comes at the end.
    put(kMagic, sizeof kMagic);
    const uint8_t rest[3] = {0, 0, 1};
    put(rest, sizeof rest);
    putU64(0);
    return true;
}

void Writer::openMemory() {
    toMemory_ = true;
    failed_ = false;
    open_ = true;
    size_ = 0;
    memory_.clear();
    put(kMagic, sizeof kMagic);
    const uint8_t rest[3] = {0, 0, 1};
    put(rest, sizeof rest);
    putU64(0);
}

void Writer::put(const void* bytes, size_t size) {
    if (size == 0) return;
    if (toMemory_) {
        memory_.append(static_cast<const char*>(bytes), size);
    } else if (!file_.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size))) {
        failed_ = true;
    }
    size_ += size;
}

void Writer::putU64(uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
    put(b, sizeof b);
}

Entry Writer::data(const void* bytes, size_t size) {
    if (size == 0 || !open_) return kEmptyData;
    const uint64_t at = size_;
    putU64(size);
    put(bytes, size);
    return at | kDataBit;
}

Entry Writer::group(std::span<const Entry> children) {
    if (children.empty() || !open_) return kEmptyGroup;
    const uint64_t at = size_;
    putU64(children.size());
    for (const Entry e : children) putU64(e);
    return at;
}

bool Writer::close(std::span<const Entry> children, std::string& error) {
    if (!open_) {
        error = "nothing to close";
        return false;
    }
    // The root, even when empty: its place in the header must be one.
    const uint64_t root = size_;
    putU64(children.size());
    for (const Entry e : children) putU64(e);
    uint8_t place[8];
    for (int i = 0; i < 8; ++i) place[i] = static_cast<uint8_t>(root >> (8 * i));
    if (toMemory_) {
        std::memcpy(memory_.data() + 8, place, 8);
        memory_[5] = static_cast<char>(kWhole);
    } else {
        file_.seekp(8);
        file_.write(reinterpret_cast<const char*>(place), 8);
        file_.seekp(5);
        const char whole = static_cast<char>(kWhole);
        file_.write(&whole, 1);
        file_.flush();
        if (!file_) failed_ = true;
        file_.close();
    }
    open_ = false;
    if (failed_) {
        error = "the file could not be written whole (is the disk full?)";
        return false;
    }
    return true;
}

// --- Reader ----------------------------------------------------------------------------------

bool Reader::open(const std::string& path, std::string& error) {
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file) {
        error = path + ": cannot read it";
        return false;
    }
    file->seekg(0, std::ios::end);
    const std::streamoff end = file->tellg();
    if (end < static_cast<std::streamoff>(kHeader)) {
        error = path + ": not an Alembic (Ogawa) file";
        return false;
    }
    file_ = std::move(file);
    inMemory_ = false;
    path_ = path;
    size_ = static_cast<uint64_t>(end);
    uint8_t header[kHeader];
    if (!read(0, header, kHeader, error)) return false;
    if (std::memcmp(header, kMagic, sizeof kMagic) != 0) {
        error = path + ": not an Alembic (Ogawa) file" +
                (std::memcmp(header, "\x89HDF", 4) == 0 ? " -- an HDF5 one, which Alembic wrote before 1.5" : "");
        return false;
    }
    if (header[5] != kWhole) {
        error = path + ": never finished -- the program writing it stopped before closing it";
        return false;
    }
    root_ = u64At(header + 8);
    if (root_ < kHeader || root_ >= size_ || isData(root_)) {
        error = path + ": the root group is not where the header says";
        return false;
    }
    return true;
}

bool Reader::openMemory(std::string bytes, std::string& error) {
    memory_ = std::move(bytes);
    inMemory_ = true;
    path_ = "(memory)";
    size_ = memory_.size();
    file_.reset();
    if (size_ < kHeader || std::memcmp(memory_.data(), kMagic, sizeof kMagic) != 0) {
        error = "not an Alembic (Ogawa) file";
        return false;
    }
    const auto* header = reinterpret_cast<const uint8_t*>(memory_.data());
    if (header[5] != kWhole) {
        error = "never finished -- the program writing it stopped before closing it";
        return false;
    }
    root_ = u64At(header + 8);
    if (root_ < kHeader || root_ >= size_ || isData(root_)) {
        error = "the root group is not where the header says";
        return false;
    }
    return true;
}

bool Reader::read(uint64_t at, void* out, uint64_t size, std::string& error) const {
    if (size == 0) return true;
    if (at > size_ || size > size_ - at) {
        error = path_ + ": something points past the end of the file (cut short?)";
        return false;
    }
    if (inMemory_) {
        std::memcpy(out, memory_.data() + at, size);
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    file_->clear();
    file_->seekg(static_cast<std::streamoff>(at));
    if (!file_->read(static_cast<char*>(out), static_cast<std::streamsize>(size))) {
        error = path_ + ": cannot read it";
        return false;
    }
    return true;
}

bool Reader::children(Entry group, std::vector<Entry>& out, std::string& error) const {
    out.clear();
    if (isData(group)) {
        error = path_ + ": data where a group should be";
        return false;
    }
    if (group == kEmptyGroup) return true;
    uint8_t count[8];
    if (group < kHeader || !read(group, count, 8, error)) {
        if (error.empty()) error = path_ + ": a group inside the header";
        return false;
    }
    const uint64_t n = u64At(count);
    if (n > (size_ - group - 8) / 8) {
        error = path_ + ": a group bigger than the file";
        return false;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(n) * 8);
    if (!read(group + 8, raw.data(), raw.size(), error)) return false;
    out.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < out.size(); ++i) out[i] = u64At(raw.data() + 8 * i);
    return true;
}

bool Reader::dataSize(Entry entry, uint64_t& size, std::string& error) const {
    size = 0;
    if (!isData(entry)) {
        error = path_ + ": a group where data should be";
        return false;
    }
    const uint64_t at = offsetOf(entry);
    if (at == 0) return true;
    uint8_t n[8];
    if (at < kHeader || !read(at, n, 8, error)) {
        if (error.empty()) error = path_ + ": data inside the header";
        return false;
    }
    size = u64At(n);
    if (size > size_ - at - 8) {
        error = path_ + ": data bigger than the file";
        return false;
    }
    return true;
}

bool Reader::data(Entry entry, uint64_t from, uint64_t count, std::vector<uint8_t>& out, std::string& error) const {
    out.clear();
    uint64_t size = 0;
    if (!dataSize(entry, size, error)) return false;
    if (from > size || count > size - from) {
        error = path_ + ": reading past the end of a block of data";
        return false;
    }
    out.resize(static_cast<size_t>(count));
    return read(offsetOf(entry) + 8 + from, out.data(), count, error);
}

bool Reader::data(Entry entry, std::vector<uint8_t>& out, std::string& error) const {
    uint64_t size = 0;
    if (!dataSize(entry, size, error)) return false;
    return data(entry, 0, size, out, error);
}

}  // namespace pg::io::ogawa
