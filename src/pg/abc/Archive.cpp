#include "pg/abc/Archive.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace pg::abc {

namespace {

/// The format version Alembic's Ogawa archives carry, and the library
/// version whose files these are like (1.8.3).
constexpr int32_t kFormatVersion = 0;
constexpr int32_t kLibraryVersion = 10803;
/// Deeper than this, a file is taken for broken.
constexpr int kMaxDepth = 256;
constexpr size_t kMaxObjects = 10000000;

// --- MurmurHash3, x64, 128 bits (Austin Appleby; public domain) -------------------------------

uint64_t fmix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
}

uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

Key murmur3(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    const size_t blocks = size / 16;
    uint64_t h1 = 0, h2 = 0;
    constexpr uint64_t c1 = 0x87c37b91114253d5ull, c2 = 0x4cf5ad432745937full;
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t k1 = le64(bytes + 16 * i), k2 = le64(bytes + 16 * i + 8);
        k1 *= c1;
        k1 = std::rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        h1 = std::rotl(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;
        k2 *= c2;
        k2 = std::rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        h2 = std::rotl(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }
    const uint8_t* tail = bytes + 16 * blocks;
    const size_t rest = size & 15;
    uint64_t k1 = 0, k2 = 0;
    for (size_t i = rest; i > 8; --i) k2 ^= static_cast<uint64_t>(tail[i - 1]) << (8 * (i - 9));
    if (rest > 8) {
        k2 *= c2;
        k2 = std::rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
    }
    for (size_t i = std::min<size_t>(rest, 8); i > 0; --i) k1 ^= static_cast<uint64_t>(tail[i - 1]) << (8 * (i - 1));
    if (rest > 0) {
        k1 *= c1;
        k1 = std::rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
    }
    h1 ^= size;
    h2 ^= size;
    h1 += h2;
    h2 += h1;
    h1 = fmix(h1);
    h2 = fmix(h2);
    h1 += h2;
    h2 += h1;
    Key out;
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(i)] = static_cast<uint8_t>(h1 >> (8 * i));
        out[static_cast<size_t>(8 + i)] = static_cast<uint8_t>(h2 >> (8 * i));
    }
    return out;
}

// --- Little-endian bytes ------------------------------------------------------------------------

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void putF64(std::vector<uint8_t>& out, double v) {
    uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(b >> (8 * i)));
}

/// `v` in 1, 2 or 4 bytes, as `hint` (0, 1, 2) says.
void putSized(std::vector<uint8_t>& out, uint32_t v, uint32_t hint) {
    const int bytes = hint == 0 ? 1 : hint == 1 ? 2 : 4;
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

/// Reads bytes off the front of a block, never past its end.
struct Cursor {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool ok = true;

    size_t left() const { return at <= bytes.size() ? bytes.size() - at : 0; }
    uint32_t u(int size) {
        if (!ok || left() < static_cast<size_t>(size)) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = size - 1; i >= 0; --i) v = (v << 8) | bytes[at + static_cast<size_t>(i)];
        at += static_cast<size_t>(size);
        return v;
    }
    double f64() {
        if (!ok || left() < 8) {
            ok = false;
            return 0.0;
        }
        const uint64_t b = le64(bytes.data() + at);
        at += 8;
        double v = 0.0;
        std::memcpy(&v, &b, sizeof v);
        return v;
    }
    std::string text(size_t size) {
        if (!ok || left() < size) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes.data() + at), size);
        at += size;
        return s;
    }
};

// What the 32 bits of a property's header hold.
constexpr uint32_t kTypeMask = 0x3, kHintShift = 2, kPodShift = 4, kHasTimeSampling = 0x100, kFirstLast = 0x200,
                   kHomogenous = 0x400, kConstant = 0x800, kExtentShift = 12, kMetaShift = 20;

}  // namespace

size_t podSize(Pod pod) {
    switch (pod) {
        case Pod::Bool:
        case Pod::U8:
        case Pod::I8: return 1;
        case Pod::U16:
        case Pod::I16:
        case Pod::F16: return 2;
        case Pod::U32:
        case Pod::I32:
        case Pod::F32: return 4;
        case Pod::U64:
        case Pod::I64:
        case Pod::F64: return 8;
        default: return 0;
    }
}

const char* podName(Pod pod) {
    static const char* const names[] = {"bool_t",   "uint8_t",  "int8_t",   "uint16_t", "int16_t",
                                        "uint32_t", "int32_t",  "uint64_t", "int64_t",  "float16_t",
                                        "float32_t", "float64_t", "string",  "wstring"};
    const auto i = static_cast<size_t>(pod);
    return i < std::size(names) ? names[i] : "unknown";
}

std::string serialize(const MetaData& meta) {
    std::string out;
    for (const auto& [key, value] : meta) {
        if (!out.empty()) out += ';';
        out += key + '=' + value;
    }
    return out;
}

MetaData parseMetaData(std::string_view text) {
    MetaData out;
    while (!text.empty()) {
        const size_t end = text.find(';');
        const std::string_view pair = text.substr(0, end);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos && eq > 0) out[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}

Key keyOf(const void* bytes, size_t size) { return murmur3(bytes, size); }

// --- Time sampling --------------------------------------------------------------------------------

TimeSampling TimeSampling::uniform(double step, double start) {
    TimeSampling ts;
    ts.timePerCycle = step;
    ts.times = {start};
    return ts;
}

double TimeSampling::timeAt(size_t i) const {
    if (times.empty()) return static_cast<double>(i);
    if (acyclic()) return times[std::min(i, times.size() - 1)];
    const size_t n = times.size();
    return static_cast<double>(i / n) * timePerCycle + times[i % n];
}

void TimeSampling::bracket(double time, size_t count, size_t& lo, size_t& hi, double& t) const {
    lo = hi = 0;
    t = 0.0;
    // A time this near a sample's is that sample's: frame f asked for as
    // f / fps -- rounded otherwise than it was written -- is frame f's, not
    // the one before it blended in by a hair, or held where nothing blends.
    const double near = 1e-6 * (count > 1 ? std::fabs(timeAt(1) - timeAt(0)) : 1.0);
    if (count <= 1 || !(time > timeAt(0) + near)) return;
    if (!(time < timeAt(count - 1) - near)) {
        lo = hi = count - 1;
        return;
    }
    // The last at or before `time`: the times grow with the index.
    size_t a = 0, b = count - 1;
    while (b - a > 1) {
        const size_t m = a + (b - a) / 2;
        (timeAt(m) <= time ? a : b) = m;
    }
    const double span = timeAt(b) - timeAt(a);
    if (timeAt(b) - time <= 1e-6 * span) {
        lo = hi = b;
        return;
    }
    lo = a;
    hi = b;
    t = span > 0.0 && time - timeAt(a) > 1e-6 * span ? std::clamp((time - timeAt(a)) / span, 0.0, 1.0) : 0.0;
}

size_t PropertyHeader::stored(size_t i) const {
    if (samples == 0 || constant() || i < firstChanged) return 0;
    if (i >= lastChanged) return lastChanged - firstChanged + 1;
    return i - firstChanged + 1;
}

// --- Writing ------------------------------------------------------------------------------------

void PropertyWriter::scalar(const void* bytes) {
    Key key;
    const io::ogawa::Entry e = archive_.sampleData(bytes, podSize(pod_) * extent_, key);
    samples_.emplace_back(e, io::ogawa::kEmptyData);
    keys_.push_back(key);
}

void PropertyWriter::array(const void* bytes, size_t count) {
    Key key;
    const io::ogawa::Entry e = archive_.sampleData(bytes, count * podSize(pod_) * extent_, key);
    samples_.emplace_back(e, archive_.dimsData(count));
    keys_.push_back(key);
    if (count != 1) scalarLike_ = false;
}

void PropertyWriter::repeat() {
    if (samples_.empty()) {
        if (type_ == PropertyType::Array) {
            array(nullptr, 0);
        } else {
            const std::vector<uint8_t> zero(podSize(pod_) * extent_, 0);
            scalar(zero.data());
        }
        return;
    }
    samples_.push_back(samples_.back());
    keys_.push_back(keys_.back());
}

CompoundWriter& CompoundWriter::compound(const std::string& name, MetaData meta) {
    compounds_.push_back(std::unique_ptr<CompoundWriter>(new CompoundWriter(archive_, name, std::move(meta))));
    order_.emplace_back(true, compounds_.size() - 1);
    return *compounds_.back();
}

PropertyWriter& CompoundWriter::scalar(const std::string& name, Pod pod, uint8_t extent, uint32_t timeSampling, MetaData meta) {
    properties_.push_back(std::unique_ptr<PropertyWriter>(
        new PropertyWriter(archive_, name, PropertyType::Scalar, pod, extent, timeSampling, std::move(meta))));
    order_.emplace_back(false, properties_.size() - 1);
    return *properties_.back();
}

PropertyWriter& CompoundWriter::array(const std::string& name, Pod pod, uint8_t extent, uint32_t timeSampling, MetaData meta) {
    properties_.push_back(std::unique_ptr<PropertyWriter>(
        new PropertyWriter(archive_, name, PropertyType::Array, pod, extent, timeSampling, std::move(meta))));
    order_.emplace_back(false, properties_.size() - 1);
    return *properties_.back();
}

ObjectWriter& ObjectWriter::child(const std::string& name, MetaData meta) {
    children_.push_back(std::unique_ptr<ObjectWriter>(new ObjectWriter(archive_, name, std::move(meta))));
    return *children_.back();
}

size_t ArchiveWriter::KeyHash::operator()(const Key& k) const {
    uint64_t h = 0;
    std::memcpy(&h, k.data(), sizeof h);
    return static_cast<size_t>(h);
}

ArchiveWriter::ArchiveWriter() : top_(new ObjectWriter(*this, "ABC", {})) {
    timeSamplings_.push_back(TimeSampling{});
}

ArchiveWriter::~ArchiveWriter() = default;

bool ArchiveWriter::open(const std::string& path, const MetaData& meta, std::string& error) {
    if (!ogawa_.open(path, error)) return false;
    meta_ = meta;
    begin();
    return true;
}

void ArchiveWriter::openMemory(const MetaData& meta) {
    ogawa_.openMemory();
    meta_ = meta;
    begin();
}

void ArchiveWriter::begin() {
    // The versions first, as Alembic lays them out.
    std::vector<uint8_t> version, library;
    putU32(version, static_cast<uint32_t>(kFormatVersion));
    putU32(library, static_cast<uint32_t>(kLibraryVersion));
    versionData_ = ogawa_.data(version);
    libraryData_ = ogawa_.data(library);
    open_ = true;
}

uint32_t ArchiveWriter::timeSampling(const TimeSampling& ts) {
    for (size_t i = 0; i < timeSamplings_.size(); ++i) {
        if (timeSamplings_[i] == ts) return static_cast<uint32_t>(i);
    }
    timeSamplings_.push_back(ts);
    timeSamplings_.back().maxSamples = 0;
    return static_cast<uint32_t>(timeSamplings_.size() - 1);
}

io::ogawa::Entry ArchiveWriter::sampleData(const void* bytes, size_t size, Key& key) {
    key = keyOf(bytes, size);
    if (size == 0) return io::ogawa::kEmptyData;
    if (const auto it = written_.find(key); it != written_.end()) return it->second;
    std::vector<uint8_t> block(16 + size);
    std::memcpy(block.data(), key.data(), 16);
    std::memcpy(block.data() + 16, bytes, size);
    const io::ogawa::Entry e = ogawa_.data(block);
    written_.emplace(key, e);
    return e;
}

io::ogawa::Entry ArchiveWriter::dimsData(size_t) {
    // A plain list: its length is what its values' size says.
    return io::ogawa::kEmptyData;
}

uint8_t ArchiveWriter::metaIndex(const std::string& text) {
    if (text.empty()) return 0;
    for (size_t i = 0; i < metaStrings_.size(); ++i) {
        if (metaStrings_[i] == text) return static_cast<uint8_t>(i + 1);
    }
    if (text.size() > 255 || metaStrings_.size() >= 254) return 0xff;
    metaStrings_.push_back(text);
    return static_cast<uint8_t>(metaStrings_.size());
}

void ArchiveWriter::writeProperty(const PropertyWriter& p, io::ogawa::Entry& group, std::vector<uint8_t>& headers) {
    const size_t n = p.samples_.size();
    // The first and last samples that differ from the one before them.
    uint32_t first = 0, last = 0;
    for (size_t i = 1; i < n; ++i) {
        if (p.samples_[i] == p.samples_[i - 1]) continue;
        if (first == 0) first = static_cast<uint32_t>(i);
        last = static_cast<uint32_t>(i);
    }
    const bool constant = first == 0 && last == 0;
    std::vector<io::ogawa::Entry> children;
    auto store = [&](size_t i) {
        children.push_back(p.samples_[i].first);
        if (p.type_ == PropertyType::Array) children.push_back(p.samples_[i].second);
    };
    if (n > 0) {
        store(0);
        if (!constant) {
            for (size_t i = first; i <= last; ++i) store(i);
        }
    }
    group = ogawa_.group(children);
    TimeSampling& ts = timeSamplings_[std::min<size_t>(p.timeSampling_, timeSamplings_.size() - 1)];
    ts.maxSamples = std::max<uint32_t>(ts.maxSamples, static_cast<uint32_t>(n));

    const std::string meta = serialize(p.meta_);
    const uint8_t index = metaIndex(meta);
    const uint32_t biggest = std::max({static_cast<uint32_t>(n), first, last, p.timeSampling_, static_cast<uint32_t>(p.name_.size()),
                                       index == 0xff ? static_cast<uint32_t>(meta.size()) : 0u});
    const uint32_t hint = biggest > 0xffff ? 2 : biggest > 0xff ? 1 : 0;
    uint32_t info = static_cast<uint32_t>(p.type_ == PropertyType::Scalar ? 1 : 2);
    if (p.type_ == PropertyType::Array && p.scalarLike_ && n > 0) info |= 1;
    info |= hint << kHintShift;
    info |= static_cast<uint32_t>(p.pod_) << kPodShift;
    if (p.timeSampling_ != 0) info |= kHasTimeSampling;
    const bool firstLast = !constant && (first != 1 || last != n - 1);
    if (constant) info |= kConstant;
    if (firstLast) info |= kFirstLast;
    // As Alembic marks them: scalars, and arrays of single numbers -- what
    // its check of a sample's points against its numbers comes to. Readers
    // go by the sizes of the data.
    const bool homogenous = p.type_ == PropertyType::Scalar || p.extent_ == 1;
    if (homogenous) info |= kHomogenous;
    info |= static_cast<uint32_t>(p.extent_) << kExtentShift;
    info |= static_cast<uint32_t>(index) << kMetaShift;
    putU32(headers, info);
    putSized(headers, static_cast<uint32_t>(n), hint);
    if (firstLast) {
        putSized(headers, first, hint);
        putSized(headers, last, hint);
    }
    if (p.timeSampling_ != 0) putSized(headers, p.timeSampling_, hint);
    putSized(headers, static_cast<uint32_t>(p.name_.size()), hint);
    headers.insert(headers.end(), p.name_.begin(), p.name_.end());
    if (index == 0xff) {
        putSized(headers, static_cast<uint32_t>(meta.size()), hint);
        headers.insert(headers.end(), meta.begin(), meta.end());
    }
}

io::ogawa::Entry ArchiveWriter::writeCompound(const CompoundWriter& c, std::vector<Key>& keys) {
    if (c.order_.empty()) return io::ogawa::kEmptyGroup;
    std::vector<io::ogawa::Entry> children;
    std::vector<uint8_t> headers;
    for (const auto& [isCompound, which] : c.order_) {
        if (isCompound) {
            const CompoundWriter& sub = *c.compounds_[which];
            children.push_back(writeCompound(sub, keys));
            const std::string meta = serialize(sub.meta_);
            const uint8_t index = metaIndex(meta);
            const uint32_t biggest =
                std::max(static_cast<uint32_t>(sub.name_.size()), index == 0xff ? static_cast<uint32_t>(meta.size()) : 0u);
            const uint32_t hint = biggest > 0xffff ? 2 : biggest > 0xff ? 1 : 0;
            putU32(headers, (hint << kHintShift) | (static_cast<uint32_t>(index) << kMetaShift));
            putSized(headers, static_cast<uint32_t>(sub.name_.size()), hint);
            headers.insert(headers.end(), sub.name_.begin(), sub.name_.end());
            if (index == 0xff) {
                putSized(headers, static_cast<uint32_t>(meta.size()), hint);
                headers.insert(headers.end(), meta.begin(), meta.end());
            }
        } else {
            const PropertyWriter& p = *c.properties_[which];
            io::ogawa::Entry group = io::ogawa::kEmptyGroup;
            writeProperty(p, group, headers);
            children.push_back(group);
            keys.insert(keys.end(), p.keys_.begin(), p.keys_.end());
        }
    }
    children.push_back(ogawa_.data(headers));
    return ogawa_.group(children);
}

io::ogawa::Entry ArchiveWriter::writeObject(const ObjectWriter& o, Key& propertiesHash, Key& childrenHash) {
    std::vector<Key> keys;
    std::vector<io::ogawa::Entry> children = {writeCompound(o.properties_, keys)};
    propertiesHash = keyOf(keys.data(), keys.size() * sizeof(Key));
    std::vector<uint8_t> headers, childHashes;
    for (const auto& child : o.children_) {
        Key ph, ch;
        children.push_back(writeObject(*child, ph, ch));
        childHashes.insert(childHashes.end(), child->name_.begin(), child->name_.end());
        childHashes.insert(childHashes.end(), ph.begin(), ph.end());
        childHashes.insert(childHashes.end(), ch.begin(), ch.end());
        putU32(headers, static_cast<uint32_t>(child->name_.size()));
        headers.insert(headers.end(), child->name_.begin(), child->name_.end());
        const std::string meta = serialize(child->meta_);
        const uint8_t index = metaIndex(meta);
        headers.push_back(index);
        if (index == 0xff) {
            putU32(headers, static_cast<uint32_t>(meta.size()));
            headers.insert(headers.end(), meta.begin(), meta.end());
        }
    }
    childrenHash = Key{};
    if (!o.children_.empty()) childrenHash = keyOf(childHashes.data(), childHashes.size());
    headers.insert(headers.end(), propertiesHash.begin(), propertiesHash.end());
    headers.insert(headers.end(), childrenHash.begin(), childrenHash.end());
    children.push_back(ogawa_.data(headers));
    return ogawa_.group(children);
}

bool ArchiveWriter::close(std::string& error) {
    if (!open_) {
        error = "the archive is not open";
        return false;
    }
    open_ = false;
    Key ph, ch;
    const io::ogawa::Entry top = writeObject(*top_, ph, ch);
    const std::string meta = serialize(meta_);
    const io::ogawa::Entry metaData = ogawa_.data(meta.data(), meta.size());
    std::vector<uint8_t> samplings;
    for (const TimeSampling& ts : timeSamplings_) {
        putU32(samplings, std::max<uint32_t>(ts.maxSamples, &ts == &timeSamplings_.front() ? 1u : 0u));
        putF64(samplings, ts.timePerCycle);
        putU32(samplings, static_cast<uint32_t>(ts.times.size()));
        for (const double t : ts.times) putF64(samplings, t);
    }
    const io::ogawa::Entry samplingData = ogawa_.data(samplings);
    std::vector<uint8_t> strings;
    for (const std::string& s : metaStrings_) {
        strings.push_back(static_cast<uint8_t>(s.size()));
        strings.insert(strings.end(), s.begin(), s.end());
    }
    const io::ogawa::Entry stringData = ogawa_.data(strings);
    const io::ogawa::Entry root[6] = {versionData_, libraryData_, top, metaData, samplingData, stringData};
    return ogawa_.close(root, error);
}

// --- Reading --------------------------------------------------------------------------------------

const PropertyReader* PropertyReader::find(std::string_view name) const {
    for (const PropertyReader& c : children) {
        if (c.header.name == name) return &c;
    }
    return nullptr;
}

bool ArchiveReader::open(const std::string& path, std::string& error) {
    path_ = path;
    if (!ogawa_.open(path, error)) return false;
    if (!load(error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool ArchiveReader::openMemory(std::string bytes, std::string& error) {
    path_ = "(memory)";
    if (!ogawa_.openMemory(std::move(bytes), error)) return false;
    return load(error);
}

bool ArchiveReader::load(std::string& error) {
    std::vector<io::ogawa::Entry> root;
    if (!ogawa_.children(ogawa_.root(), root, error)) return false;
    if (root.size() < 6 || !io::ogawa::isData(root[0]) || !io::ogawa::isData(root[1]) || io::ogawa::isData(root[2]) ||
        !io::ogawa::isData(root[3]) || !io::ogawa::isData(root[4]) || !io::ogawa::isData(root[5])) {
        error = "not laid out as an Alembic archive";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ogawa_.data(root[0], bytes, error)) return false;
    {
        Cursor c{bytes};
        const auto version = static_cast<int32_t>(c.u(4));
        if (!c.ok || version != kFormatVersion) {
            error = "an Alembic archive of an unknown version (" + std::to_string(version) + ")";
            return false;
        }
    }
    if (!ogawa_.data(root[1], bytes, error)) return false;
    {
        Cursor c{bytes};
        libraryVersion_ = static_cast<int32_t>(c.u(4));
    }
    if (!ogawa_.data(root[3], bytes, error)) return false;
    meta_ = parseMetaData(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    // The time samplings: how many samples, the time per cycle, those in a cycle.
    if (!ogawa_.data(root[4], bytes, error)) return false;
    {
        Cursor c{bytes};
        timeSamplings_.clear();
        while (c.ok && c.left() > 0) {
            TimeSampling ts;
            ts.maxSamples = c.u(4);
            ts.timePerCycle = c.f64();
            const uint32_t n = c.u(4);
            if (!c.ok || n > c.left() / 8) {
                error = "its time samplings are broken";
                return false;
            }
            ts.times.resize(n);
            for (double& t : ts.times) t = c.f64();
            if (ts.times.empty()) ts.times = {0.0};
            timeSamplings_.push_back(std::move(ts));
        }
        if (!c.ok) {
            error = "its time samplings are broken";
            return false;
        }
        if (timeSamplings_.empty()) timeSamplings_.push_back(TimeSampling{});
    }
    // The metadata kept by number: 0 is empty.
    if (!ogawa_.data(root[5], bytes, error)) return false;
    {
        Cursor c{bytes};
        metaStrings_ = {""};
        while (c.ok && c.left() > 0) {
            const uint32_t n = c.u(1);
            metaStrings_.push_back(c.text(n));
        }
        if (!c.ok) {
            error = "its metadata is broken";
            return false;
        }
    }
    top_ = ObjectReader();
    top_.name = "ABC";
    top_.path = "/";
    objectCount_ = 0;
    return readObject(root[2], top_, 0, error);
}

bool ArchiveReader::readCompound(io::ogawa::Entry group, PropertyReader& into, int depth, std::string& error) {
    into.children.clear();
    into.group = group;
    if (group == io::ogawa::kEmptyGroup) return true;
    if (depth > kMaxDepth) {
        error = "properties nested too deep";
        return false;
    }
    std::vector<io::ogawa::Entry> children;
    if (!ogawa_.children(group, children, error)) return false;
    if (children.empty()) return true;
    if (!io::ogawa::isData(children.back())) {
        error = "a compound property without its headers";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ogawa_.data(children.back(), bytes, error)) return false;
    Cursor c{bytes};
    const size_t count = children.size() - 1;
    for (size_t k = 0; k < count; ++k) {
        PropertyHeader h;
        const uint32_t info = c.u(4);
        const uint32_t type = info & kTypeMask;
        const int size = 1 << ((info >> kHintShift) & 3);
        if (size > 4) {
            error = "a property header that cannot be";
            return false;
        }
        if (type == 0) {
            h.type = PropertyType::Compound;
        } else {
            h.type = type == 1 ? PropertyType::Scalar : PropertyType::Array;
            h.scalarLike = type != 2;
            const uint32_t pod = (info >> kPodShift) & 0xf;
            if (pod > static_cast<uint32_t>(Pod::WString)) {
                error = "a property of an unknown type";
                return false;
            }
            h.pod = static_cast<Pod>(pod);
            h.extent = static_cast<uint8_t>((info >> kExtentShift) & 0xff);
            h.samples = c.u(size);
            if (info & kFirstLast) {
                h.firstChanged = c.u(size);
                h.lastChanged = c.u(size);
            } else if (!(info & kConstant) && h.samples > 0) {
                h.firstChanged = 1;
                h.lastChanged = h.samples - 1;
            }
            if (info & kHasTimeSampling) h.timeSampling = c.u(size);
            if (h.lastChanged < h.firstChanged || (h.samples > 0 && h.lastChanged >= h.samples)) {
                error = "a property whose samples do not add up";
                return false;
            }
        }
        h.name = c.text(c.u(size));
        const uint32_t index = (info >> kMetaShift) & 0xff;
        if (index == 0xff) {
            h.meta = parseMetaData(c.text(c.u(size)));
        } else if (index < metaStrings_.size()) {
            h.meta = parseMetaData(metaStrings_[index]);
        } else {
            error = "a property's metadata is not in the archive";
            return false;
        }
        if (!c.ok) {
            error = "its property headers are cut short";
            return false;
        }
        PropertyReader p;
        p.header = std::move(h);
        p.group = children[k];
        if (p.header.type == PropertyType::Compound) {
            if (io::ogawa::isData(p.group) || !readCompound(p.group, p, depth + 1, error)) {
                if (error.empty()) error = "a compound property that is data";
                return false;
            }
        } else if (io::ogawa::isData(p.group)) {
            error = "a property that is data, not a group";
            return false;
        }
        into.children.push_back(std::move(p));
    }
    return true;
}

bool ArchiveReader::readObject(io::ogawa::Entry group, ObjectReader& into, int depth, std::string& error) {
    if (depth > kMaxDepth || ++objectCount_ > kMaxObjects) {
        error = "objects nested too deep";
        return false;
    }
    std::vector<io::ogawa::Entry> children;
    if (!ogawa_.children(group, children, error)) return false;
    if (children.empty()) return true;  // nothing in it, not even properties
    if (io::ogawa::isData(children[0]) || !readCompound(children[0], into.properties, depth + 1, error)) {
        if (error.empty()) error = "an object whose properties are data";
        return false;
    }
    into.properties.header.type = PropertyType::Compound;
    if (children.size() < 2) return true;
    if (!io::ogawa::isData(children.back())) {
        error = "an object without its children's headers";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ogawa_.data(children.back(), bytes, error)) return false;
    const size_t count = children.size() - 2;
    // The headers, then two hashes of 16 bytes.
    if (bytes.size() >= 32) bytes.resize(bytes.size() - 32);
    Cursor c{bytes};
    for (size_t k = 0; k < count; ++k) {
        auto child = std::make_unique<ObjectReader>();
        child->name = c.text(c.u(4));
        const uint32_t index = c.u(1);
        if (index == 0xff) {
            child->meta = parseMetaData(c.text(c.u(4)));
        } else if (index < metaStrings_.size()) {
            child->meta = parseMetaData(metaStrings_[index]);
        } else {
            error = "an object's metadata is not in the archive";
            return false;
        }
        if (!c.ok) {
            error = "its object headers are cut short";
            return false;
        }
        child->path = (into.path == "/" ? "" : into.path) + "/" + child->name;
        child->parent = &into;
        const io::ogawa::Entry g = children[1 + k];
        if (io::ogawa::isData(g) || !readObject(g, *child, depth + 1, error)) {
            if (error.empty()) error = "an object that is data";
            return false;
        }
        into.children.push_back(std::move(child));
    }
    return true;
}

std::vector<const ObjectReader*> ArchiveReader::objects() const {
    std::vector<const ObjectReader*> out, stack = {&top_};
    while (!stack.empty()) {
        const ObjectReader* o = stack.back();
        stack.pop_back();
        out.push_back(o);
        for (size_t i = o->children.size(); i-- > 0;) stack.push_back(o->children[i].get());
    }
    return out;
}

bool ArchiveReader::read(const PropertyReader& p, size_t i, Sample& out, std::string& error) const {
    out = Sample();
    const PropertyHeader& h = p.header;
    if (h.type == PropertyType::Compound) {
        error = h.name + " is a compound property: no samples of its own";
        return false;
    }
    if (h.samples == 0) {
        error = h.name + " has no samples";
        return false;
    }
    const size_t stored = h.stored(std::min<size_t>(i, h.samples - 1));
    std::vector<io::ogawa::Entry> children;
    if (!ogawa_.children(p.group, children, error)) return false;
    const size_t per = h.type == PropertyType::Array ? 2 : 1;
    if (children.size() < per * (stored + 1)) {
        error = h.name + ": fewer samples stored than its header says";
        return false;
    }
    const io::ogawa::Entry data = children[per * stored];
    uint64_t size = 0;
    if (!ogawa_.dataSize(data, size, error)) return false;
    if (size > 0) {
        if (size < 16) {
            error = h.name + ": a sample shorter than its key";
            return false;
        }
        if (!ogawa_.data(data, 16, size - 16, out.bytes, error)) return false;
    }
    const size_t each = podSize(h.pod) * std::max<size_t>(h.extent, 1);
    if (h.type == PropertyType::Scalar) {
        out.count = 1;
        if (each > 0 && out.bytes.size() < each) {
            error = h.name + ": a sample shorter than its type";
            return false;
        }
        return true;
    }
    std::vector<uint8_t> dims;
    if (!ogawa_.data(children[2 * stored + 1], dims, error)) return false;
    for (size_t k = 0; k + 8 <= dims.size(); k += 8) out.dims.push_back(le64(dims.data() + k));
    out.count = each > 0 ? out.bytes.size() / each : out.bytes.size();
    if (each > 0) out.bytes.resize(out.count * each);
    return true;
}

const TimeSampling& ArchiveReader::timeSamplingOf(const PropertyReader& p) const {
    const size_t i = p.header.timeSampling;
    return timeSamplings_[i < timeSamplings_.size() ? i : 0];
}

double ArchiveReader::timeOf(const PropertyReader& p, size_t i) const { return timeSamplingOf(p).timeAt(i); }

std::shared_ptr<const ArchiveReader> ArchiveReader::openCached(const std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    auto stampOf = [](const std::string& f) {
        std::error_code ec;
        const auto size = fs::file_size(f, ec);
        const auto time = fs::last_write_time(f, ec);
        return std::to_string(ec ? 0 : size) + ":" +
               std::to_string(static_cast<long long>(time.time_since_epoch().count()));
    };
    struct Entry {
        std::shared_ptr<const ArchiveReader> archive;
        std::string stamp;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    const std::string stamp = stampOf(path);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(path); it != cache.end()) {
            if (it->second.stamp == stamp) return it->second.archive;
            cache.erase(it);
        }
    }
    auto archive = std::make_shared<ArchiveReader>();
    if (!archive->open(path, error)) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 16) cache.erase(cache.begin());
    cache[path] = {archive, stamp};
    return archive;
}

}  // namespace pg::abc
