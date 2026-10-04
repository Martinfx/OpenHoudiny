#pragma once
//
// Ogawa: the container Alembic keeps its archives in -- every .abc file
// written since Alembic 1.5 (2013). A file is a tree of groups and blocks
// of data:
//
//   header   "Ogawa"; a byte that is 0xff once the file is whole (0 while
//            it is being written); the version, 0 1; where the root group
//            is (64 bits)
//   data     its size (64 bits), then its bytes
//   group    how many children (64 bits), then an entry for each: where
//            the child is, the top bit set for data and clear for a group;
//            0 is the empty group, the top bit alone empty data
//
// All numbers little-endian. Data are written as they come and never
// moved; a group is written once its children are -- the root last, and
// its place put in the header. What Alembic puts in which group is its own
// business (pg/abc); this is only the boxes.
//
// Written without the library, from what its files hold. The reader reads
// what it is asked for and no more -- a file may be gigabytes of frames --
// and believes nothing: an entry that points out of the file, a group that
// does not fit in it, is an error, not a read past the end.
//
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace pg::io::ogawa {

/// Where a child is, in its group: the top bit set for data.
using Entry = uint64_t;
inline constexpr Entry kDataBit = 0x8000000000000000ull;
inline constexpr Entry kEmptyGroup = 0;
inline constexpr Entry kEmptyData = kDataBit;

inline bool isData(Entry e) { return (e & kDataBit) != 0; }
inline uint64_t offsetOf(Entry e) { return e & ~kDataBit; }

/// Writes a file as its blocks come: data at once, each group when asked,
/// the root and the header's "whole" byte last (close). Into a file, or
/// into memory (for tests and what is sent elsewhere).
class Writer {
public:
    Writer() = default;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    /// Starts `path`, emptied. False, with why, if it cannot be written.
    bool open(const std::string& path, std::string& error);
    /// Starts one in memory (bytes()).
    void openMemory();

    /// A block of data, appended: its entry. Empty: kEmptyData, nothing
    /// written.
    Entry data(const void* bytes, size_t size);
    Entry data(std::span<const uint8_t> bytes) { return data(bytes.data(), bytes.size()); }
    /// A group of `children`, appended: its entry. None: kEmptyGroup.
    Entry group(std::span<const Entry> children);

    /// Writes the root group of `children`, puts its place in the header
    /// and marks the file whole. False, with why, if any write failed.
    bool close(std::span<const Entry> children, std::string& error);

    /// What has been written so far, in bytes.
    uint64_t size() const { return size_; }
    /// The file, written in memory (openMemory).
    const std::string& bytes() const { return memory_; }

private:
    void put(const void* bytes, size_t size);
    void putU64(uint64_t v);

    std::ofstream file_;
    std::string memory_;
    bool toMemory_ = false, failed_ = false, open_ = false;
    uint64_t size_ = 0;
};

/// Reads a file's groups and data where asked: none of it is read at open
/// but the header. Safe to read from on many threads at once.
class Reader {
public:
    /// Opens `path`. False, with why, if it is not an Ogawa file, or one
    /// that was never finished.
    bool open(const std::string& path, std::string& error);
    /// ... or the bytes of one.
    bool openMemory(std::string bytes, std::string& error);

    Entry root() const { return root_; }
    uint64_t fileSize() const { return size_; }

    /// The entries of `group`; none for kEmptyGroup. False, with why, for
    /// an entry that is data, out of the file, or a group that does not fit
    /// in it.
    bool children(Entry group, std::vector<Entry>& out, std::string& error) const;
    /// How many bytes the data `entry` holds; 0 for kEmptyData.
    bool dataSize(Entry entry, uint64_t& size, std::string& error) const;
    /// Its bytes from `from` on, `count` of them, onto `out` (resized).
    bool data(Entry entry, uint64_t from, uint64_t count, std::vector<uint8_t>& out, std::string& error) const;
    /// All of them.
    bool data(Entry entry, std::vector<uint8_t>& out, std::string& error) const;

private:
    bool read(uint64_t at, void* out, uint64_t size, std::string& error) const;

    std::shared_ptr<std::ifstream> file_;
    std::string memory_;
    bool inMemory_ = false;
    std::string path_;
    uint64_t size_ = 0;
    Entry root_ = kEmptyGroup;
    mutable std::mutex mutex_;
};

}  // namespace pg::io::ogawa
