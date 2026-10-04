#pragma once
//
// Alembic archives (.abc), read and written without the library: the
// layout Alembic's Ogawa core gives an archive, as the files it writes
// hold it (pg/io/Ogawa.h for the container).
//
//   root group   the format version (0), the library version, the top
//                object, the archive's metadata, its time samplings, and
//                the metadata strings kept by number
//   object       its properties (a compound), each child object, then the
//                children's headers -- a name and metadata each -- and two
//                hashes, of its properties and of its children
//   compound     each property, then their headers
//   scalar       a block of data a sample
//   array        two a sample: the values, and their dimensions (empty
//                for a plain list)
//
// Each sample's block starts with its key, the 128-bit MurmurHash3 of its
// bytes. A sample the same as one written before -- in any property -- is
// not written again: the group points at the one there. A property whose
// samples are all the same keeps one ("constant").
//
// A property's header packs into 32 bits what it is (scalar, array or
// compound), the type and count of its numbers, whether it is constant and
// the number of its metadata; then its sample count, the first and last
// that change (when not the obvious ones), its time sampling, its name and
// -- when not kept by number -- its metadata: in 1, 2 or 4 bytes each, as
// the biggest needs.
//
// Metadata is "key=value;key=value". A time sampling is uniform (a sample
// every so often), cyclic (several in each cycle) or acyclic (each sample
// its own time).
//
#include "pg/io/Ogawa.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pg::abc {

/// The plain types of the numbers in a property.
enum class Pod : uint8_t { Bool, U8, I8, U16, I16, U32, I32, U64, I64, F16, F32, F64, String, WString };
/// Bytes a number of it takes; 0 for strings.
size_t podSize(Pod pod);
const char* podName(Pod pod);

enum class PropertyType : uint8_t { Compound, Scalar, Array };

/// key=value pairs, by key -- as Alembic's MetaData is a map.
using MetaData = std::map<std::string, std::string>;
std::string serialize(const MetaData& meta);
MetaData parseMetaData(std::string_view text);

/// When the samples of a property are.
struct TimeSampling {
    /// Alembic's "acyclic" time per cycle: each sample its own time.
    static constexpr double kAcyclic = 1.7976931348623157e308 / 32.0;

    double timePerCycle = 1.0;
    std::vector<double> times = {0.0};  ///< those in the first cycle; acyclic: every one
    uint32_t maxSamples = 0;            ///< the most samples a property of it has (written)

    /// A sample every `step` seconds, the first at `start`.
    static TimeSampling uniform(double step, double start);
    bool acyclic() const { return timePerCycle >= kAcyclic * 0.5; }
    /// The time of sample `i`.
    double timeAt(size_t i) const;
    /// The samples, of `count`, round `time`: `lo` at or before it (the
    /// first if none), `hi` the next (as `lo` at the end), `t` how far
    /// between them, 0 to 1.
    void bracket(double time, size_t count, size_t& lo, size_t& hi, double& t) const;
    bool operator==(const TimeSampling& o) const { return timePerCycle == o.timePerCycle && times == o.times; }
};

/// A 128-bit key: MurmurHash3 (x64, 128 bits, seed 0) of a sample's bytes.
using Key = std::array<uint8_t, 16>;
Key keyOf(const void* bytes, size_t size);

// --- Writing ---------------------------------------------------------------------------------

class ArchiveWriter;

/// A scalar or array property: its samples, written as they come.
class PropertyWriter {
public:
    /// The next sample. A scalar: `extent` numbers. An array: any number
    /// of `extent` numbers each -- `count` of them, the bytes of all.
    void scalar(const void* bytes);
    void array(const void* bytes, size_t count);
    /// The next sample the same as the last (none yet: an empty one).
    void repeat();

    const std::string& name() const { return name_; }
    size_t samples() const { return samples_.size(); }

private:
    friend class CompoundWriter;
    friend class ArchiveWriter;
    PropertyWriter(ArchiveWriter& archive, std::string name, PropertyType type, Pod pod, uint8_t extent,
                   uint32_t timeSampling, MetaData meta)
        : archive_(archive), name_(std::move(name)), type_(type), pod_(pod), extent_(extent),
          timeSampling_(timeSampling), meta_(std::move(meta)) {}

    ArchiveWriter& archive_;
    std::string name_;
    PropertyType type_;
    Pod pod_;
    uint8_t extent_;
    uint32_t timeSampling_;
    MetaData meta_;
    /// Each sample's blocks: the values, and (array) the dimensions.
    std::vector<std::pair<io::ogawa::Entry, io::ogawa::Entry>> samples_;
    bool scalarLike_ = true;  // an array whose every sample holds one
    std::vector<Key> keys_;   // each sample's, for the hash of the object
};

/// A compound property: properties in it, in the order they are made.
class CompoundWriter {
public:
    CompoundWriter& compound(const std::string& name, MetaData meta = {});
    PropertyWriter& scalar(const std::string& name, Pod pod, uint8_t extent, uint32_t timeSampling, MetaData meta = {});
    PropertyWriter& array(const std::string& name, Pod pod, uint8_t extent, uint32_t timeSampling, MetaData meta = {});

private:
    friend class ObjectWriter;
    friend class ArchiveWriter;
    CompoundWriter(ArchiveWriter& archive, std::string name, MetaData meta)
        : archive_(archive), name_(std::move(name)), meta_(std::move(meta)) {}

    ArchiveWriter& archive_;
    std::string name_;
    MetaData meta_;
    // In the order made; either a compound or a property each.
    std::vector<std::unique_ptr<CompoundWriter>> compounds_;
    std::vector<std::unique_ptr<PropertyWriter>> properties_;
    std::vector<std::pair<bool, size_t>> order_;  // compound?, which
};

/// An object: its properties and its children.
class ObjectWriter {
public:
    ObjectWriter& child(const std::string& name, MetaData meta = {});
    CompoundWriter& properties() { return properties_; }
    const std::string& name() const { return name_; }

private:
    friend class ArchiveWriter;
    ObjectWriter(ArchiveWriter& archive, std::string name, MetaData meta)
        : archive_(archive), name_(std::move(name)), meta_(std::move(meta)), properties_(archive, "", {}) {}

    ArchiveWriter& archive_;
    std::string name_;
    MetaData meta_;
    CompoundWriter properties_;
    std::vector<std::unique_ptr<ObjectWriter>> children_;
};

/// An archive, written as its samples come: their data at once, the tree
/// round them at close().
class ArchiveWriter {
public:
    ArchiveWriter();
    ~ArchiveWriter();
    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    /// Starts `path`; `meta` is the archive's (the application, the frame
    /// rate...). False, with why, if it cannot be written.
    bool open(const std::string& path, const MetaData& meta, std::string& error);
    /// ... in memory (bytes()).
    void openMemory(const MetaData& meta);

    /// A time sampling, by its number; 0 is one sample a second from 0.
    uint32_t timeSampling(const TimeSampling& ts);
    /// The object everything hangs from ("ABC").
    ObjectWriter& top() { return *top_; }

    /// Writes the tree and finishes the file. False, with why, on a failed
    /// write.
    bool close(std::string& error);
    const std::string& bytes() const { return ogawa_.bytes(); }
    uint64_t size() const { return ogawa_.size(); }

private:
    friend class PropertyWriter;
    friend class CompoundWriter;

    /// The versions, written first.
    void begin();
    /// A sample's block: written once, then pointed at.
    io::ogawa::Entry sampleData(const void* bytes, size_t size, Key& key);
    io::ogawa::Entry dimsData(size_t count);
    /// The metadata's number (0 empty, 0xff not kept by number).
    uint8_t metaIndex(const std::string& text);
    void writeProperty(const PropertyWriter& p, io::ogawa::Entry& group, std::vector<uint8_t>& headers);
    io::ogawa::Entry writeCompound(const CompoundWriter& c, std::vector<Key>& keys);
    io::ogawa::Entry writeObject(const ObjectWriter& o, Key& propertiesHash, Key& childrenHash);

    io::ogawa::Writer ogawa_;
    MetaData meta_;
    std::vector<TimeSampling> timeSamplings_;
    std::unique_ptr<ObjectWriter> top_;
    struct KeyHash {
        size_t operator()(const Key& k) const;
    };
    std::unordered_map<Key, io::ogawa::Entry, KeyHash> written_;
    std::vector<std::string> metaStrings_;  // kept by number, from 1
    io::ogawa::Entry versionData_ = io::ogawa::kEmptyData, libraryData_ = io::ogawa::kEmptyData;
    bool open_ = false;
};

// --- Reading ---------------------------------------------------------------------------------

struct PropertyHeader {
    std::string name;
    PropertyType type = PropertyType::Compound;
    Pod pod = Pod::U8;
    uint8_t extent = 1;
    bool scalarLike = false;       ///< an array whose every sample holds one
    uint32_t samples = 0, firstChanged = 0, lastChanged = 0, timeSampling = 0;
    MetaData meta;

    bool constant() const { return firstChanged == 0 && lastChanged == 0; }
    /// Which stored sample sample `i` is.
    size_t stored(size_t i) const;
};

class ArchiveReader;

/// A property as read: its header, its group, and -- a compound -- the
/// properties in it.
struct PropertyReader {
    PropertyHeader header;
    io::ogawa::Entry group = io::ogawa::kEmptyGroup;
    std::vector<PropertyReader> children;

    const PropertyReader* find(std::string_view name) const;
    bool is(PropertyType t) const { return header.type == t; }
};

struct ObjectReader {
    std::string name, path;  ///< its name, and its path from the top: /box/Cube
    MetaData meta;
    PropertyReader properties;  ///< a compound
    std::vector<std::unique_ptr<ObjectReader>> children;
    const ObjectReader* parent = nullptr;
};

/// A sample's values as read: its bytes, how many of `extent` numbers.
struct Sample {
    std::vector<uint8_t> bytes;
    size_t count = 0;  ///< elements (each `extent` numbers); 1 for a scalar
    std::vector<uint64_t> dims;  ///< an array's dimensions, when not plain
};

/// An archive open for reading: the tree of objects and the headers of
/// their properties read at open; samples when asked.
class ArchiveReader {
public:
    /// Opens `path`. False, with why, if it is no Alembic archive (an HDF5
    /// one, from before 2013, is said to be one), or a broken one.
    bool open(const std::string& path, std::string& error);
    bool openMemory(std::string bytes, std::string& error);
    /// The archive of `path`, opened once while it is the same file: the
    /// same path, size and time of change.
    static std::shared_ptr<const ArchiveReader> openCached(const std::string& path, std::string& error);

    const MetaData& meta() const { return meta_; }
    const std::vector<TimeSampling>& timeSamplings() const { return timeSamplings_; }
    const ObjectReader& top() const { return top_; }
    int32_t libraryVersion() const { return libraryVersion_; }
    /// Every object, in the order of the tree.
    std::vector<const ObjectReader*> objects() const;

    /// Sample `i` (of the property's count; past the last, the last) of a
    /// scalar or array property. False, with why, for a broken one.
    bool read(const PropertyReader& p, size_t i, Sample& out, std::string& error) const;
    /// The time of sample `i` of `p`.
    double timeOf(const PropertyReader& p, size_t i) const;
    const TimeSampling& timeSamplingOf(const PropertyReader& p) const;

    const std::string& path() const { return path_; }

private:
    bool load(std::string& error);
    bool readCompound(io::ogawa::Entry group, PropertyReader& into, int depth, std::string& error);
    bool readObject(io::ogawa::Entry group, ObjectReader& into, int depth, std::string& error);

    io::ogawa::Reader ogawa_;
    std::string path_;
    MetaData meta_;
    std::vector<TimeSampling> timeSamplings_;
    std::vector<std::string> metaStrings_;
    ObjectReader top_;
    int32_t libraryVersion_ = 0;
    size_t objectCount_ = 0;
};

}  // namespace pg::abc
