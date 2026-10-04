// OpenVDB files read (Vdb.h): the header, the grids' descriptors, and each
// grid -- its metadata, its transform, its tree's topology and values --
// laid out as a dense volume in the world. Written from what OpenVDB's
// io::Archive, GridDescriptor, RootNode, InternalNode, LeafNode and
// Compression read, and tested against files the library writes.
#include "pg/io/Vdb.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/io/Blosc.h"
#include "pg/io/Inflate.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace pg::io {
namespace {

static_assert(std::endian::native == std::endian::little, "the files are little-endian, as the machine is");

constexpr int64_t kMagic = 0x56444220;  // "VDB "
constexpr uint32_t kOldest = 222;       // compression per grid, and a byte before each node's values
constexpr uint32_t kNewest = 225;       // what OpenVDB 13 writes; 224 OpenVDB 4 to 12
constexpr uint32_t kZip = 0x1, kActiveMask = 0x2, kBlosc = 0x4;
constexpr char kSeparator = '\x1e';               // a grid's unique name: its name, this, a number
constexpr size_t kMaxString = size_t(1) << 24;    // longer is not a name, nor metadata
constexpr size_t kMaxSparse = size_t(1) << 29;    // floats a grid's leaves may hold: 2 GB
constexpr int32_t kRootSpan = 4096, kUpperSpan = 128, kLeafSpan = 8;  // voxels a side: a root's child, an upper node's, a leaf
constexpr int32_t kFarthest = 1 << 30;  // no node is further from the origin, in voxels

// What is written of a node's inactive values, in a byte before its values.
enum : uint8_t {
    kNoMaskOrInactiveVals,     // every one the background
    kNoMaskAndMinusBg,         // every one minus the background
    kNoMaskAndOneInactiveVal,  // every one a value written there
    kMaskAndNoInactiveVals,    // minus the background or -- where a mask written there says -- the background
    kMaskAndOneInactiveVal,    // a value written there, or the background
    kMaskAndTwoInactiveVals,   // one of two values written there
    kNoMaskAndAllVals,         // every value written, active or not
};

using Coord = std::array<int32_t, 3>;  // ordered x, then y, then z, as OpenVDB's
using Value = std::array<float, 3>;    // a voxel's value: one float, or three for a vector

// --- bytes ------------------------------------------------------------------------------

/// Bytes read in order, never past their end: a read past it fails, and
/// every read after it.
class Cursor {
public:
    explicit Cursor(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    bool ok() const { return ok_; }
    size_t pos() const { return pos_; }
    size_t left() const { return ok_ ? bytes_.size() - pos_ : 0; }
    /// The next `n` bytes, or null.
    const uint8_t* take(size_t n) {
        if (!ok_ || n > bytes_.size() - pos_) {
            ok_ = false;
            return nullptr;
        }
        const uint8_t* at = bytes_.data() + pos_;
        pos_ += n;
        return at;
    }
    template <typename T>
    T pod() {
        T v{};
        if (const uint8_t* at = take(sizeof(T))) std::memcpy(&v, at, sizeof(T));
        return v;
    }
    uint8_t u8() { return pod<uint8_t>(); }
    uint32_t u32() { return pod<uint32_t>(); }
    int32_t i32() { return pod<int32_t>(); }
    int64_t i64() { return pod<int64_t>(); }
    double f64() { return pod<double>(); }
    /// OpenVDB's readString: a 32-bit length, then the characters.
    std::string string() {
        const uint32_t n = u32();
        if (n > kMaxString) ok_ = false;
        const uint8_t* at = take(n);
        return at ? std::string(reinterpret_cast<const char*>(at), n) : std::string();
    }
    Coord coord() { return {i32(), i32(), i32()}; }

private:
    std::span<const uint8_t> bytes_;
    size_t pos_ = 0;
    bool ok_ = true;
};

/// Where a file's bytes are: in memory, or in the file -- read a range at a
/// time, so that a large file is never all in memory.
class Source {
public:
    explicit Source(std::span<const uint8_t> bytes) : memory_(bytes), size_(bytes.size()) {}
    explicit Source(const std::string& path) : file_(path, std::ios::binary), isFile_(true) {
        if (file_) {
            file_.seekg(0, std::ios::end);
            const std::streamoff end = file_.tellg();
            size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
        }
    }
    bool open() const { return !isFile_ || static_cast<bool>(file_); }
    uint64_t size() const { return size_; }
    /// The bytes [at, at + n) -- fewer at the end -- in `storage` or not;
    /// empty when they cannot be read.
    std::span<const uint8_t> read(uint64_t at, uint64_t n, std::vector<uint8_t>& storage) {
        if (at >= size_) return {};
        n = std::min(n, size_ - at);
        if (!isFile_) return memory_.subspan(static_cast<size_t>(at), static_cast<size_t>(n));
        storage.resize(static_cast<size_t>(n));
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(at));
        file_.read(reinterpret_cast<char*>(storage.data()), static_cast<std::streamsize>(n));
        if (static_cast<uint64_t>(file_.gcount()) != n) return {};
        return storage;
    }

private:
    std::span<const uint8_t> memory_;
    std::ifstream file_;
    bool isFile_ = false;
    uint64_t size_ = 0;
};

/// `parse(cursor, why)` run on the bytes from `at` on: on 64 KB of them,
/// then on more and more until they are enough -- or are all there are,
/// and it fails.
template <typename Parse>
bool windowed(Source& src, uint64_t at, std::string& error, Parse&& parse) {
    std::vector<uint8_t> storage;
    for (uint64_t window = uint64_t(1) << 16;; window *= 16) {
        const std::span<const uint8_t> bytes = src.read(at, window, storage);
        const uint64_t wanted = at < src.size() ? std::min(window, src.size() - at) : 0;
        if (bytes.size() != wanted) {
            error = "the file cannot be read";
            return false;
        }
        Cursor in(bytes);
        std::string why;
        if (parse(in, why)) return true;
        if (in.ok() || bytes.size() == src.size() - std::min(at, src.size())) {
            error = why.empty() ? "the file is cut short" : why;
            return false;
        }
    }
}

// --- what the file is -------------------------------------------------------------------

struct Header {
    uint32_t version = 0;
    bool offsets = true;   // each grid says where it starts and ends
    uint64_t first = 0;    // where the first grid's descriptor is
    int32_t count = 0;     // grids
};

/// A grid as its descriptor names it, and where it is.
struct Descriptor {
    std::string unique;  // the name in the file: two grids of one name are told apart by a suffix
    std::string name;
    std::string type;    // "Tree_float_5_4_3", without "_HalfFloat"
    std::string parent;  // the unique name of the grid whose tree it shares, or empty
    bool half = false;   // kept as half floats
    uint64_t at = 0;     // where the descriptor is
    uint64_t grid = 0, end = 0;  // where the grid's data starts, and ends
};

/// The kind of a grid's values: how many floats a voxel, how wide each is
/// in the file, whether they are reals -- what may be kept as half floats.
struct Kind {
    int comps = 1;
    int width = 4;
    bool real = true;
};

/// The kind a grid's type names -- "Tree_float_5_4_3" -- or false for one
/// not read: of other values, or another layout of the tree.
bool kindOf(const std::string& type, Kind& kind, std::string& values) {
    values = type;
    if (type.rfind("Tree_", 0) != 0) return false;
    const size_t layout = type.find('_', 5);
    values = type.substr(5, layout == std::string::npos ? std::string::npos : layout - 5);
    if (layout == std::string::npos || type.substr(layout) != "_5_4_3") return false;
    if (values == "float") kind = {1, 4, true};
    else if (values == "double") kind = {1, 8, true};
    else if (values == "int32") kind = {1, 4, false};
    else if (values == "int64") kind = {1, 8, false};
    else if (values == "vec3s") kind = {3, 4, true};
    else if (values == "vec3d") kind = {3, 8, true};
    else if (values == "vec3i") kind = {3, 4, false};
    else if (values == "half") kind = {1, 2, false};  // a half grid: what is written is what it holds
    else return false;
    return true;
}

/// One component of a value, `width` bytes as the file keeps it.
float component(const uint8_t* p, int width, bool real) {
    if (width == 2) {
        uint16_t h;
        std::memcpy(&h, p, 2);
        return floatFromHalf(h);
    }
    if (width == 4) {
        if (real) {
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }
        int32_t i;
        std::memcpy(&i, p, 4);
        return static_cast<float>(i);
    }
    if (real) {
        double d;
        std::memcpy(&d, p, 8);
        return static_cast<float>(d);
    }
    int64_t i;
    std::memcpy(&i, p, 8);
    return static_cast<float>(i);
}

/// The header and the file's metadata, up to the first descriptor.
bool parseHeader(Cursor& in, Header& h, std::string& why) {
    if (in.i64() != kMagic) {
        if (in.ok()) why = "not an OpenVDB file";
        return false;
    }
    h.version = in.u32();
    if (!in.ok()) return false;
    if (h.version < kOldest) {
        why = "an OpenVDB file of version " + std::to_string(h.version) + ", older than the " + std::to_string(kOldest) +
              " to " + std::to_string(kNewest) + " read";
        return false;
    }
    in.u32();  // the library's version, major
    in.u32();  // ... and minor
    h.offsets = in.u8() != 0;
    in.take(36);  // the UUID, as text
    // The file's metadata: name, type, size, value each.
    const uint32_t count = in.u32();
    for (uint32_t i = 0; i < count && in.ok(); ++i) {
        in.string();
        in.string();
        in.take(in.u32());
    }
    h.count = in.i32();
    if (!in.ok()) return false;
    if (h.count < 0) {
        why = "a broken OpenVDB file: " + std::to_string(h.count) + " grids";
        return false;
    }
    h.first = in.pos();
    return true;
}

bool parseDescriptor(Cursor& in, Descriptor& d) {
    d.unique = in.string();
    d.name = d.unique.substr(0, d.unique.find(kSeparator));
    d.type = in.string();
    const std::string half = "_HalfFloat";
    if (d.type.size() > half.size() && d.type.compare(d.type.size() - half.size(), half.size(), half) == 0) {
        d.half = true;
        d.type.resize(d.type.size() - half.size());
    }
    d.parent = in.string();
    const int64_t grid = in.i64();
    in.i64();  // where its values start
    const int64_t end = in.i64();
    d.grid = static_cast<uint64_t>(std::max<int64_t>(grid, 0));
    d.end = static_cast<uint64_t>(std::max<int64_t>(end, 0));
    return in.ok();
}

// --- a grid -----------------------------------------------------------------------------

/// What of a grid's metadata is read.
struct Meta {
    std::string gridClass = "unknown";
    bool hasMin = false, hasMax = false;
    Coord min{}, max{};  // the box of its active voxels, index space
};

bool parseMeta(Cursor& in, Meta& meta) {
    const uint32_t count = in.u32();
    for (uint32_t i = 0; i < count && in.ok(); ++i) {
        const std::string name = in.string(), type = in.string();
        const uint32_t size = in.u32();
        const uint8_t* at = in.take(size);
        if (!at) break;
        if (name == "class" && type == "string") {
            meta.gridClass.assign(reinterpret_cast<const char*>(at), size);
        } else if ((name == "file_bbox_min" || name == "file_bbox_max") && type == "vec3i" && size == 12) {
            Coord c;
            std::memcpy(c.data(), at, 12);
            (name == "file_bbox_min" ? meta.min : meta.max) = c;
            (name == "file_bbox_min" ? meta.hasMin : meta.hasMax) = true;
        }
    }
    return in.ok();
}

/// Index space to the world, as row vectors: world = index m + t.
struct Map {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double t[3] = {0, 0, 0};
    bool frustum = false;  // a NonlinearFrustumMap: read past, not laid out

    void apply(const double index[3], double world[3]) const {
        for (int c = 0; c < 3; ++c) world[c] = index[0] * m[0][c] + index[1] * m[1][c] + index[2] * m[2][c] + t[c];
    }
    double determinant() const {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
    /// The edge of a voxel along index axis r, world units.
    double edge(int r) const { return std::sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]); }
    /// Turned about x so that z up becomes y up: (x, y, z) -> (x, z, -y).
    void zUp() {
        for (double* row : {m[0], m[1], m[2], t}) {
            const double y = row[1], z = row[2];
            row[1] = z;
            row[2] = -y;
        }
    }
};

/// A grid's transform: the name of its map, then the map.
bool parseMap(Cursor& in, Map& map, std::string& why) {
    const std::string type = in.string();
    auto vec3 = [&](double* v) {
        for (int a = 0; a < 3; ++a) v[a] = in.f64();
    };
    double scale[3] = {1, 1, 1}, unused[3];
    if (type == "UniformScaleTranslateMap" || type == "ScaleTranslateMap") {
        // The translation, the scale, then what the library works out of
        // them: the voxel's size, the inverse scale, its square, half of it.
        vec3(map.t);
        vec3(scale);
        for (int k = 0; k < 4; ++k) vec3(unused);
    } else if (type == "UniformScaleMap" || type == "ScaleMap") {
        vec3(scale);
        for (int k = 0; k < 4; ++k) vec3(unused);
    } else if (type == "TranslationMap") {
        vec3(map.t);
    } else if (type == "AffineMap" || type == "UnitaryMap") {
        // A 4 x 4 matrix of row vectors: the translation in its last row.
        double mm[16];
        for (double& v : mm) v = in.f64();
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) map.m[r][c] = mm[4 * r + c];
        }
        for (int c = 0; c < 3; ++c) map.t[c] = mm[12 + c];
    } else if (type == "NonlinearFrustumMap") {
        // Read past -- its box, taper and depth, then the linear map after
        // them -- to the tree: a camera's frustum is not laid out as a volume.
        for (int k = 0; k < 8; ++k) in.f64();
        Map second;
        if (!parseMap(in, second, why)) return false;
        map.frustum = true;
        return true;
    } else {
        if (in.ok()) why = "a transform of another kind (" + type + ") is not read";
        return false;
    }
    if (type.find("Scale") != std::string::npos) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) map.m[r][c] = r == c ? scale[r] : 0.0;
        }
    }
    if (!in.ok()) return false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(map.m[r][c])) why = "a transform that is not finite";
        }
        if (!std::isfinite(map.t[r])) why = "a transform that is not finite";
    }
    if (why.empty() && !(std::fabs(map.determinant()) > 1e-300)) why = "a transform that flattens the grid";
    return why.empty();
}

/// A grid's tree, as it keeps its values: leaves of 8^3 voxels, and tiles --
/// one value for all the voxels of a node's slot -- at every level.
struct Tree {
    Kind kind;
    Value background{};
    struct Leaf {
        Coord origin;
        std::array<uint64_t, 8> active;  // voxel n = x 64 + y 8 + z in bit n % 64 of word n / 64
    };
    std::vector<Leaf> leaves;
    std::vector<float> values;  // each leaf's 512 voxels in turn, `comps` floats each
    struct Tile {
        Coord origin;
        int32_t span;  // voxels a side
        bool active;
        Value value;
    };
    std::vector<Tile> tiles;  // those that are active or not the background
};

/// Reads a tree as the library writes it: the topology -- the root's
/// background, tiles and children, each internal node's masks and tile
/// values, each leaf's mask -- then the leaves' values.
class TreeReader {
public:
    TreeReader(Cursor& in, const Kind& kind, bool half, uint32_t compression, std::string& why)
        : in_(in), kind_(kind), halves_(half && kind.real), compression_(compression), why_(why) {}

    bool read(Tree& tree) {
        tree.kind = kind_;
        in_.i32();  // buffers: 1
        background_ = value();
        tree.background = background_;
        const uint32_t tiles = in_.u32(), children = in_.u32();
        if (!in_.ok()) return false;
        const size_t tileBytes = 12 + static_cast<size_t>(kind_.comps * kind_.width) + 1;
        if (tiles > in_.left() / tileBytes || children > in_.left() / 12) return cut();
        for (uint32_t n = 0; n < tiles; ++n) {
            const Coord origin = in_.coord();
            const Value v = value();
            const bool active = in_.u8() != 0;
            if (!inRange(origin)) return broken("a tile out of place");
            if (active || v != background_) tree.tiles.push_back({origin, kRootSpan, active, v});
        }
        // The children, each with the range of leaves under it.
        struct Top {
            Coord origin;
            size_t first = 0, end = 0;
        };
        std::vector<Top> tops;
        for (uint32_t n = 0; n < children && in_.ok(); ++n) {
            Top top;
            top.origin = in_.coord();
            if (((top.origin[0] | top.origin[1] | top.origin[2]) & (kRootSpan - 1)) || !inRange(top.origin)) {
                return broken("a node out of place");
            }
            top.first = tree.leaves.size();
            if (!node(2, top.origin, tree)) return false;
            top.end = tree.leaves.size();
            tops.push_back(top);
        }
        if (!in_.ok()) return cut();
        // The root keeps one thing at each place: a child takes a tile's.
        std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.origin < b.origin; });
        for (size_t n = 1; n < tops.size(); ++n) {
            if (tops[n - 1].origin == tops[n].origin) return broken("two nodes in one place");
        }
        std::erase_if(tree.tiles, [&](const Tree::Tile& t) {
            return t.span == kRootSpan && std::binary_search(tops.begin(), tops.end(), Top{t.origin},
                                                             [](const Top& a, const Top& b) { return a.origin < b.origin; });
        });
        // The values: the leaves under each child, the children in the order
        // the root keeps them -- by their places -- each leaf's mask again
        // before them.
        const size_t perLeaf = 512 * static_cast<size_t>(kind_.comps);
        tree.values.assign(tree.leaves.size() * perLeaf, 0.0f);
        for (const Top& top : tops) {
            for (size_t l = top.first; l < top.end; ++l) {
                if (!in_.take(64)) return cut();
                if (!values(tree.values.data() + l * perLeaf, 512, tree.leaves[l].active.data())) return false;
            }
        }
        return true;
    }

private:
    static bool inRange(const Coord& c) {
        return std::all_of(c.begin(), c.end(), [](int32_t x) { return x >= -kFarthest && x <= kFarthest; });
    }
    bool cut() {
        if (why_.empty()) why_ = "the grid is cut short";
        return false;
    }
    bool broken(const std::string& what) {
        if (why_.empty()) why_ = "a broken grid: " + what;
        return false;
    }

    /// A value as the file keeps it at full precision: a background, a
    /// root's tile, an inactive value.
    Value value() {
        Value v{};
        for (int c = 0; c < kind_.comps; ++c) {
            const uint8_t* at = in_.take(static_cast<size_t>(kind_.width));
            v[static_cast<size_t>(c)] = at ? component(at, kind_.width, kind_.real) : 0.0f;
        }
        return v;
    }

    /// `count` values as the library writes them (readData): compressed as
    /// the grid says, or not -- a size before them, below 0 for those that
    /// were not made smaller -- half floats when it keeps them so.
    bool data(size_t count, float* out) {
        if (count == 0 && halves_) return true;  // nothing is written
        const size_t width = halves_ ? 2 : static_cast<size_t>(kind_.width);
        const size_t floats = count * static_cast<size_t>(kind_.comps), bytes = floats * width;
        const uint8_t* raw = nullptr;
        if (compression_ & (kBlosc | kZip)) {
            const int64_t size = in_.i64();
            if (!in_.ok()) return cut();
            if (size <= 0) {
                if (uint64_t(0) - static_cast<uint64_t>(size) != bytes) return broken("values of the wrong size");
                raw = in_.take(bytes);
            } else {
                const uint8_t* packed = in_.take(static_cast<size_t>(size));
                if (!packed) return cut();
                std::string why;
                unpacked_.clear();
                const std::span<const uint8_t> span(packed, static_cast<size_t>(size));
                const bool ok = (compression_ & kBlosc) ? bloscDecompress(span, unpacked_, bytes, why)
                                                        : zlibInflate(span, unpacked_, bytes, why);
                if (!ok || unpacked_.size() != bytes) {
                    return broken("values that do not decompress" + (why.empty() ? std::string() : " (" + why + ")"));
                }
                raw = unpacked_.data();
            }
        } else {
            raw = in_.take(bytes);
        }
        if (!raw && bytes > 0) return cut();
        if (width == 4 && kind_.real) {
            if (bytes > 0) std::memcpy(out, raw, bytes);
        } else {
            for (size_t i = 0; i < floats; ++i) out[i] = component(raw + i * width, static_cast<int>(width), kind_.real);
        }
        return true;
    }

    /// A node's `count` values (readCompressedValues) into `out`: what is
    /// there of its inactive ones, then the values written -- only the
    /// active ones, with the active mask compression -- the rest made from
    /// what was said of them.
    bool values(float* out, size_t count, const uint64_t* active) {
        const size_t comps = static_cast<size_t>(kind_.comps), words = count / 64;
        const uint8_t meta = in_.u8();
        if (!in_.ok()) return cut();
        if (meta > kNoMaskAndAllVals) return broken("values of an unknown kind");
        Value inactive0 = background_, inactive1 = background_;
        if (meta != kNoMaskOrInactiveVals) {
            for (float& v : inactive0) v = -v;
        }
        if (meta == kNoMaskAndOneInactiveVal || meta == kMaskAndOneInactiveVal || meta == kMaskAndTwoInactiveVals) {
            inactive0 = value();
            if (meta == kMaskAndTwoInactiveVals) inactive1 = value();
        }
        selection_.assign(words, 0);
        if (meta == kMaskAndNoInactiveVals || meta == kMaskAndOneInactiveVal || meta == kMaskAndTwoInactiveVals) {
            const uint8_t* at = in_.take(words * 8);
            if (at) std::memcpy(selection_.data(), at, words * 8);
        }
        if (!in_.ok()) return cut();
        size_t stored = count;
        if ((compression_ & kActiveMask) && meta != kNoMaskAndAllVals) {
            stored = 0;
            for (size_t w = 0; w < words; ++w) stored += static_cast<size_t>(std::popcount(active[w]));
        }
        if (stored == count) return data(count, out);
        scratch_.resize(stored * comps);
        if (!data(stored, scratch_.data())) return false;
        const float* from = scratch_.data();
        for (size_t i = 0; i < count; ++i, out += comps) {
            if ((active[i >> 6] >> (i & 63)) & 1u) {
                std::memcpy(out, from, comps * sizeof(float));
                from += comps;
            } else {
                const Value& v = ((selection_[i >> 6] >> (i & 63)) & 1u) ? inactive1 : inactive0;
                std::memcpy(out, v.data(), comps * sizeof(float));
            }
        }
        return true;
    }

    /// An internal node's topology: which slots hold a child, which tiles
    /// are active, the tiles' values, then the children -- an upper node's
    /// (level 2, 32^3 slots) internal nodes, a lower one's (16^3) leaves.
    bool node(int level, const Coord& origin, Tree& tree) {
        const int log2 = level == 2 ? 5 : 4;
        const size_t slots = size_t(1) << (3 * log2), words = slots / 64;
        const int32_t span = level == 2 ? kUpperSpan : kLeafSpan, side = 1 << log2;
        std::vector<uint64_t> masks(2 * words);
        const uint8_t* at = in_.take(2 * words * 8);
        if (!at) return cut();
        std::memcpy(masks.data(), at, 2 * words * 8);
        const uint64_t* child = masks.data();
        const uint64_t* active = masks.data() + words;
        const size_t comps = static_cast<size_t>(kind_.comps);
        std::vector<float> tileValues(slots * comps);
        if (!values(tileValues.data(), slots, active)) return false;
        auto on = [](const uint64_t* mask, size_t n) { return ((mask[n >> 6] >> (n & 63)) & 1u) != 0; };
        auto place = [&](size_t n) {
            return Coord{origin[0] + static_cast<int32_t>(n >> (2 * log2)) * span,
                         origin[1] + static_cast<int32_t>((n >> log2) & static_cast<size_t>(side - 1)) * span,
                         origin[2] + static_cast<int32_t>(n & static_cast<size_t>(side - 1)) * span};
        };
        for (size_t n = 0; n < slots; ++n) {
            if (on(child, n)) continue;
            Value v{};
            std::memcpy(v.data(), tileValues.data() + n * comps, comps * sizeof(float));
            const bool isActive = on(active, n);
            if (isActive || v != background_) tree.tiles.push_back({place(n), span, isActive, v});
        }
        for (size_t n = 0; n < slots; ++n) {
            if (!on(child, n)) continue;
            if (level == 2) {
                if (!node(1, place(n), tree)) return false;
                continue;
            }
            const uint8_t* mask = in_.take(64);
            if (!mask) return cut();
            Tree::Leaf leaf;
            leaf.origin = place(n);
            std::memcpy(leaf.active.data(), mask, 64);
            tree.leaves.push_back(leaf);
            if (tree.leaves.size() * 512 * comps > kMaxSparse) {
                if (why_.empty()) why_ = "a grid of more voxels than are read (" + std::to_string(kMaxSparse / comps) + ")";
                return false;
            }
        }
        return true;
    }

    Cursor& in_;
    Kind kind_;
    bool halves_;
    uint32_t compression_;
    std::string& why_;
    Value background_{};
    std::vector<uint8_t> unpacked_;
    std::vector<float> scratch_;
    std::vector<uint64_t> selection_;
};

/// A grid read: its descriptor, what it is, and -- when its values were
/// read -- its tree.
struct Grid {
    Descriptor d;
    Kind kind;
    bool readable = false;
    std::string values;  // the kind of values its type names: float, vec3s, bool ...
    Meta meta;
    Map map;
    std::shared_ptr<const Tree> tree;
};

/// A grid's data, from where it starts: its compression, metadata and
/// transform, then -- unless `tree` is false, or it shares another's --
/// its tree.
bool parseGrid(Cursor& in, Grid& g, bool tree, std::string& why) {
    const uint32_t compression = in.u32();
    if (!parseMeta(in, g.meta)) return false;
    if (!parseMap(in, g.map, why)) return false;
    if (!tree || !g.d.parent.empty()) return true;
    if (!g.readable) {
        why = "a grid of " + g.values + " is not read";
        return false;
    }
    auto t = std::make_shared<Tree>();
    TreeReader reader(in, g.kind, g.d.half, compression, why);
    if (!reader.read(*t)) return false;
    g.tree = std::move(t);
    return true;
}

/// The file's header and every grid's descriptor -- and, for a file that
/// does not say where its grids end, every grid read in full on the way.
bool scan(Source& src, Header& h, std::vector<Grid>& grids, std::string& error) {
    if (!src.open()) {
        error = "the file cannot be opened";
        return false;
    }
    if (!windowed(src, 0, error, [&](Cursor& in, std::string& why) { return parseHeader(in, h, why); })) return false;
    const auto count = static_cast<size_t>(h.count);
    if (count > src.size() / 16) {
        error = "a broken OpenVDB file: " + std::to_string(count) + " grids";
        return false;
    }
    grids.assign(count, Grid());
    if (!h.offsets) {
        // One grid after another: each read to its end to find the next.
        std::vector<uint8_t> storage;
        const std::span<const uint8_t> rest = src.read(h.first, src.size() - std::min(h.first, src.size()), storage);
        Cursor in(rest);
        for (Grid& g : grids) {
            g.d.at = h.first + in.pos();
            if (!parseDescriptor(in, g.d)) {
                error = "the file is cut short";
                return false;
            }
            g.readable = kindOf(g.d.type, g.kind, g.values);
            g.d.grid = h.first + in.pos();
            std::string why;
            if (!parseGrid(in, g, true, why)) {
                error = "grid \"" + g.d.name + "\": " + (why.empty() ? "cut short" : why);
                if (!g.readable) error += ", and the file does not say where it ends: none after it can be read";
                return false;
            }
            g.d.end = h.first + in.pos();
        }
        return true;
    }
    uint64_t at = h.first;
    for (Grid& g : grids) {
        g.d.at = at;
        uint64_t after = 0;
        if (!windowed(src, at, error, [&](Cursor& in, std::string&) {
                if (!parseDescriptor(in, g.d)) return false;
                after = at + in.pos();
                return true;
            })) {
            return false;
        }
        if (g.d.grid < after || g.d.end < g.d.grid || g.d.end > src.size()) {
            error = "a broken OpenVDB file: grid \"" + g.d.name + "\" is out of place";
            return false;
        }
        g.readable = kindOf(g.d.type, g.kind, g.values);
        at = g.d.end;
    }
    return true;
}

/// Reads grid `g`'s data -- with its tree when `tree` is set -- from where
/// its descriptor says.
bool load(Source& src, Grid& g, bool tree, std::string& error) {
    std::string why;
    bool ok = false;
    if (tree) {
        std::vector<uint8_t> storage;
        const std::span<const uint8_t> bytes = src.read(g.d.grid, g.d.end - g.d.grid, storage);
        if (bytes.size() != g.d.end - g.d.grid) {
            error = "the file cannot be read";
            return false;
        }
        Cursor in(bytes);
        ok = parseGrid(in, g, true, why);
        if (!ok && why.empty()) why = "cut short";
    } else {
        std::string cut;
        ok = windowed(src, g.d.grid, cut, [&](Cursor& in, std::string& w) {
            const bool read = parseGrid(in, g, false, w);
            return read;
        });
        if (!ok) why = cut;
    }
    if (!ok) error = "grid \"" + g.d.name + "\": " + why;
    return ok;
}

// --- a grid as a volume -----------------------------------------------------------------

/// The box of a tree's active voxels, both corners in it; false when none is.
bool activeBox(const Tree& tree, Coord& lo, Coord& hi) {
    bool any = false;
    auto grow = [&](const Coord& a, const Coord& b) {
        for (size_t c = 0; c < 3; ++c) {
            lo[c] = any ? std::min(lo[c], a[c]) : a[c];
            hi[c] = any ? std::max(hi[c], b[c]) : b[c];
        }
        any = true;
    };
    for (const Tree::Leaf& leaf : tree.leaves) {
        // Word x holds the voxels of x: y in its bytes, z in their bits.
        int x0 = 8, x1 = -1;
        uint8_t ys = 0, zs = 0;
        for (int x = 0; x < 8; ++x) {
            const uint64_t w = leaf.active[static_cast<size_t>(x)];
            if (!w) continue;
            x0 = std::min(x0, x);
            x1 = x;
            for (int y = 0; y < 8; ++y) {
                const auto row = static_cast<uint8_t>(w >> (8 * y));
                if (row) ys = static_cast<uint8_t>(ys | (1u << y));
                zs = static_cast<uint8_t>(zs | row);
            }
        }
        if (x1 < 0) continue;
        const int y0 = std::countr_zero(ys), y1 = 7 - std::countl_zero(ys);
        const int z0 = std::countr_zero(zs), z1 = 7 - std::countl_zero(zs);
        const Coord& o = leaf.origin;
        grow({o[0] + x0, o[1] + y0, o[2] + z0}, {o[0] + x1, o[1] + y1, o[2] + z1});
    }
    for (const Tree::Tile& t : tree.tiles) {
        if (t.active) grow(t.origin, {t.origin[0] + t.span - 1, t.origin[1] + t.span - 1, t.origin[2] + t.span - 1});
    }
    return any;
}

/// The tree's values on `n` cells from voxel `lo`, `f` voxels a side to a
/// cell -- their mean -- x fastest, `comps` floats a cell.
std::vector<float> dense(const Tree& tree, const Coord& lo, const std::array<int64_t, 3>& n, int f) {
    const size_t comps = static_cast<size_t>(tree.kind.comps);
    const size_t cells = static_cast<size_t>(n[0] * n[1] * n[2]);
    const int64_t box[3] = {static_cast<int64_t>(lo[0]) + n[0] * f, static_cast<int64_t>(lo[1]) + n[1] * f,
                            static_cast<int64_t>(lo[2]) + n[2] * f};  // past the last voxel
    auto cellOf = [&](int64_t x, int64_t y, int64_t z) {
        return static_cast<size_t>((x - lo[0]) / f + n[0] * ((y - lo[1]) / f + n[1] * ((z - lo[2]) / f)));
    };
    std::vector<float> out(cells * comps);
    if (f == 1) {
        for (size_t i = 0; i < cells; ++i) std::memcpy(out.data() + i * comps, tree.background.data(), comps * sizeof(float));
    }
    // A tile: every voxel of it in the box -- or, averaging, its share of
    // each cell it covers.
    for (const Tree::Tile& t : tree.tiles) {
        int64_t a[3], b[3];
        bool inside = true;
        for (int c = 0; c < 3; ++c) {
            a[c] = std::max<int64_t>(t.origin[static_cast<size_t>(c)], lo[static_cast<size_t>(c)]);
            b[c] = std::min<int64_t>(static_cast<int64_t>(t.origin[static_cast<size_t>(c)]) + t.span, box[c]);
            inside = inside && a[c] < b[c];
        }
        if (!inside) continue;
        int64_t q0[3], q1[3];
        for (int c = 0; c < 3; ++c) {
            q0[c] = (a[c] - lo[static_cast<size_t>(c)]) / f;
            q1[c] = (b[c] - 1 - lo[static_cast<size_t>(c)]) / f;
        }
        for (int64_t z = q0[2]; z <= q1[2]; ++z) {
            for (int64_t y = q0[1]; y <= q1[1]; ++y) {
                for (int64_t x = q0[0]; x <= q1[0]; ++x) {
                    float* to = out.data() + static_cast<size_t>(x + n[0] * (y + n[1] * z)) * comps;
                    if (f == 1) {
                        std::memcpy(to, t.value.data(), comps * sizeof(float));
                        continue;
                    }
                    const int64_t q[3] = {x, y, z};
                    double share = 1.0;
                    for (int c = 0; c < 3; ++c) {
                        const int64_t from = lo[static_cast<size_t>(c)] + q[c] * f;
                        share *= static_cast<double>(std::min(b[c], from + f) - std::max(a[c], from));
                    }
                    for (size_t k = 0; k < comps; ++k) to[k] += static_cast<float>(share) * (t.value[k] - tree.background[k]);
                }
            }
        }
    }
    // The leaves: each voxel in the box.
    const size_t perLeaf = 512 * comps;
    for (size_t l = 0; l < tree.leaves.size(); ++l) {
        const Coord& o = tree.leaves[l].origin;
        if (o[0] + 8 <= lo[0] || o[1] + 8 <= lo[1] || o[2] + 8 <= lo[2] || o[0] >= box[0] || o[1] >= box[1] || o[2] >= box[2]) {
            continue;
        }
        const float* v = tree.values.data() + l * perLeaf;
        for (int x = 0; x < 8; ++x) {
            const int64_t gx = static_cast<int64_t>(o[0]) + x;
            if (gx < lo[0] || gx >= box[0]) continue;
            for (int y = 0; y < 8; ++y) {
                const int64_t gy = static_cast<int64_t>(o[1]) + y;
                if (gy < lo[1] || gy >= box[1]) continue;
                for (int z = 0; z < 8; ++z) {
                    const int64_t gz = static_cast<int64_t>(o[2]) + z;
                    if (gz < lo[2] || gz >= box[2]) continue;
                    const float* from = v + static_cast<size_t>((x << 6) | (y << 3) | z) * comps;
                    float* to = out.data() + cellOf(gx, gy, gz) * comps;
                    if (f == 1) {
                        std::memcpy(to, from, comps * sizeof(float));
                    } else {
                        for (size_t k = 0; k < comps; ++k) to[k] += from[k] - tree.background[k];
                    }
                }
            }
        }
    }
    if (f > 1) {
        const float share = 1.0f / static_cast<float>(f * f * f);
        for (size_t i = 0; i < cells; ++i) {
            for (size_t k = 0; k < comps; ++k) out[i * comps + k] = tree.background[k] + out[i * comps + k] * share;
        }
    }
    return out;
}

/// Whether the map takes each index axis r onto a world axis -- `axis[r]`,
/// its way `sign[r]`: turned by quarter turns, mirrored -- each voxel a
/// cube of edge `s`.
bool alongAxes(const Map& m, int axis[3], int sign[3], double& s) {
    bool used[3] = {false, false, false};
    double edge[3];
    for (int r = 0; r < 3; ++r) {
        int c = 0;
        for (int k = 1; k < 3; ++k) {
            if (std::fabs(m.m[r][k]) > std::fabs(m.m[r][c])) c = k;
        }
        edge[r] = std::fabs(m.m[r][c]);
        for (int k = 0; k < 3; ++k) {
            if (k != c && std::fabs(m.m[r][k]) > 1e-6 * edge[r]) return false;
        }
        if (!(edge[r] > 0.0) || used[c]) return false;
        used[c] = true;
        axis[r] = c;
        sign[r] = m.m[r][c] > 0.0 ? 1 : -1;
    }
    s = edge[0];
    return std::fabs(edge[1] - s) <= 1e-6 * s && std::fabs(edge[2] - s) <= 1e-6 * s;
}

std::string amount(size_t n) {
    char text[32];
    if (n >= 1000000000) std::snprintf(text, sizeof text, "%.1f billion", static_cast<double>(n) / 1e9);
    else if (n >= 1000000) std::snprintf(text, sizeof text, "%.1f million", static_cast<double>(n) / 1e6);
    else std::snprintf(text, sizeof text, "%zu", n);
    return text;
}

/// Grid `g` as volumes -- one, or three for a vector -- onto the end of `out`.
void toVolumes(const Grid& g, const VdbReadOptions& o, VdbVolumes& out) {
    const Tree& tree = *g.tree;
    const std::string name = g.d.name.empty() ? "volume" : g.d.name;
    Coord lo{}, hi{};
    if (!activeBox(tree, lo, hi)) {
        out.notes.push_back("grid \"" + name + "\" has no active voxel");
        return;
    }
    Map map = g.map;
    if (o.zUp) map.zUp();
    const size_t comps = static_cast<size_t>(tree.kind.comps);
    std::array<int64_t, 3> extent;
    for (size_t c = 0; c < 3; ++c) extent[c] = static_cast<int64_t>(hi[c]) - lo[c] + 1;
    const size_t limit = std::max<size_t>(o.maxVoxels, 1);
    auto cellsAt = [&](int f) {
        double n = 1.0;
        for (const int64_t e : extent) n *= static_cast<double>((e + f - 1) / f);
        return n;
    };
    int f = std::max(o.downsample, 1);
    const double all = cellsAt(1);
    if (cellsAt(f) > static_cast<double>(limit)) f = std::max(f, static_cast<int>(std::cbrt(all / static_cast<double>(limit))));
    while (cellsAt(f) > static_cast<double>(limit)) ++f;
    if (f > std::max(o.downsample, 1)) {
        out.notes.push_back("grid \"" + name + "\": " + amount(static_cast<size_t>(all)) + " voxels, averaged down " +
                            std::to_string(f) + " \xc3\x97 " + std::to_string(f) + " \xc3\x97 " + std::to_string(f) + " to fit");
    }
    std::array<int64_t, 3> n;
    for (size_t c = 0; c < 3; ++c) n[c] = (extent[c] + f - 1) / f;
    const std::vector<float> cells = dense(tree, lo, n, f);
    // The middle of cell q, in index space.
    auto middle = [&](size_t r, double q) { return static_cast<double>(lo[r]) + q * f + 0.5 * (f - 1); };

    std::vector<std::vector<float>> values(comps);
    Volume v;
    int axis[3], sign[3];
    double s = 0.0;
    if (alongAxes(map, axis, sign, s)) {
        // Voxel for voxel, the axes put in the world's order.
        for (int r = 0; r < 3; ++r) v.res[axis[r]] = static_cast<int>(n[static_cast<size_t>(r)]);
        v.voxel = static_cast<float>(s * f);
        double first[3], world[3];
        for (int r = 0; r < 3; ++r) first[r] = middle(static_cast<size_t>(r), sign[r] > 0 ? 0.0 : static_cast<double>(n[static_cast<size_t>(r)] - 1));
        map.apply(first, world);
        v.origin = Vec3(static_cast<float>(world[0] - 0.5 * s * f), static_cast<float>(world[1] - 0.5 * s * f),
                        static_cast<float>(world[2] - 0.5 * s * f));
        for (auto& c : values) c.resize(v.count());
        int64_t step[3], start = 0;  // through the cells as the volume's x, y, z go
        for (int r = 0; r < 3; ++r) {
            const int64_t stride = r == 0 ? 1 : r == 1 ? n[0] : n[0] * n[1];
            step[axis[r]] = sign[r] * stride;
            if (sign[r] < 0) start += (n[static_cast<size_t>(r)] - 1) * stride;
        }
        size_t i = 0;
        for (int z = 0; z < v.res[2]; ++z) {
            for (int y = 0; y < v.res[1]; ++y) {
                int64_t at = start + y * step[1] + z * step[2];
                for (int x = 0; x < v.res[0]; ++x, ++i, at += step[0]) {
                    for (size_t k = 0; k < comps; ++k) values[k][i] = cells[static_cast<size_t>(at) * comps + k];
                }
            }
        }
    } else {
        // Resampled: cubic voxels as long as the grid's shortest edge, over
        // the box round it in the world, each the grid's value at its middle.
        double vlo[3] = {1e300, 1e300, 1e300}, vhi[3] = {-1e300, -1e300, -1e300};
        for (int corner = 0; corner < 8; ++corner) {
            double index[3], world[3];
            for (int r = 0; r < 3; ++r) {
                index[r] = (corner >> r) & 1 ? lo[static_cast<size_t>(r)] + n[static_cast<size_t>(r)] * f - 0.5
                                             : lo[static_cast<size_t>(r)] - 0.5;
            }
            map.apply(index, world);
            for (int c = 0; c < 3; ++c) {
                vlo[c] = std::min(vlo[c], world[c]);
                vhi[c] = std::max(vhi[c], world[c]);
            }
        }
        double voxel = std::min({map.edge(0), map.edge(1), map.edge(2)}) * f;
        auto resOf = [&](double vx, int c) { return std::max(1, static_cast<int>(std::ceil((vhi[c] - vlo[c]) / vx - 1e-6))); };
        auto total = [&](double vx) {
            return static_cast<double>(resOf(vx, 0)) * resOf(vx, 1) * resOf(vx, 2);
        };
        if (total(voxel) > static_cast<double>(limit)) {
            voxel *= std::cbrt(total(voxel) / static_cast<double>(limit));
            while (total(voxel) > static_cast<double>(limit)) voxel *= 1.01;
        }
        for (int c = 0; c < 3; ++c) v.res[c] = resOf(voxel, c);
        v.voxel = static_cast<float>(voxel);
        v.origin = Vec3(static_cast<float>(vlo[0]), static_cast<float>(vlo[1]), static_cast<float>(vlo[2]));
        out.notes.push_back("grid \"" + name + "\" is turned or its voxels are not cubes: resampled onto cubes of " +
                            std::to_string(voxel) + " m");
        for (auto& c : values) c.resize(v.count());
        // World to index: the inverse of the map's matrix.
        const double (&m)[3][3] = map.m;
        const double det = map.determinant();
        double inv[3][3];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
                inv[r][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / det;
            }
        }
        pg::parallelFor(static_cast<size_t>(v.res[2]), 1, [&](size_t z0, size_t z1) {
            for (size_t z = z0; z < z1; ++z) {
                for (int y = 0; y < v.res[1]; ++y) {
                    for (int x = 0; x < v.res[0]; ++x) {
                        const double p[3] = {vlo[0] + (x + 0.5) * voxel - map.t[0], vlo[1] + (y + 0.5) * voxel - map.t[1],
                                             vlo[2] + (static_cast<double>(z) + 0.5) * voxel - map.t[2]};
                        double u[3];
                        for (int r = 0; r < 3; ++r) {
                            const double index = p[0] * inv[0][r] + p[1] * inv[1][r] + p[2] * inv[2][r];
                            u[r] = (index - middle(static_cast<size_t>(r), 0.0)) / f;
                        }
                        // Trilinear between the cells' middles; the background beyond them.
                        int64_t i0[3];
                        double w[3];
                        for (int r = 0; r < 3; ++r) {
                            const double fl = std::floor(u[r]);
                            i0[r] = static_cast<int64_t>(std::clamp(fl, -2.0, 4e9));
                            w[r] = u[r] - fl;
                        }
                        double sum[3] = {0.0, 0.0, 0.0};
                        for (int corner = 0; corner < 8; ++corner) {
                            double weight = 1.0;
                            int64_t q[3];
                            bool inside = true;
                            for (int r = 0; r < 3; ++r) {
                                const int b = (corner >> r) & 1;
                                q[r] = i0[r] + b;
                                weight *= b ? w[r] : 1.0 - w[r];
                                inside = inside && q[r] >= 0 && q[r] < n[static_cast<size_t>(r)];
                            }
                            if (weight == 0.0) continue;
                            const float* value =
                                inside ? cells.data() + static_cast<size_t>(q[0] + n[0] * (q[1] + n[1] * q[2])) * comps : tree.background.data();
                            for (size_t k = 0; k < comps; ++k) sum[k] += weight * value[k];
                        }
                        const size_t at = static_cast<size_t>(x) + static_cast<size_t>(v.res[0]) * (static_cast<size_t>(y) + static_cast<size_t>(v.res[1]) * z);
                        for (size_t k = 0; k < comps; ++k) values[k][at] = static_cast<float>(sum[k]);
                    }
                }
            }
        });
    }
    if (!(v.voxel > 0.0f) || !std::isfinite(v.voxel) || !std::isfinite(v.origin.x) || !std::isfinite(v.origin.y) ||
        !std::isfinite(v.origin.z)) {
        out.notes.push_back("grid \"" + name + "\" is too small or too far away to place");
        return;
    }
    if (comps == 3 && o.zUp) {
        // A vector turned with the world: (x, y, z) -> (x, z, -y).
        std::swap(values[1], values[2]);
        for (float& z : values[2]) z = -z;
    }
    for (size_t k = 0; k < comps; ++k) {
        Volume c = v;
        c.name = comps == 1 ? name : name + (k == 0 ? ".x" : k == 1 ? ".y" : ".z");
        c.values = std::make_shared<const std::vector<float>>(std::move(values[k]));
        out.volumes.push_back(std::move(c));
        out.classes.push_back(g.meta.gridClass);
        out.components.push_back(static_cast<int>(comps));
    }
}

bool readFrom(Source& src, VdbVolumes& out, std::string& error, const VdbReadOptions& o) {
    out = VdbVolumes();
    Header h;
    std::vector<Grid> grids;
    if (!scan(src, h, grids, error)) return false;
    if (h.version > kNewest) {
        out.notes.push_back("an OpenVDB file of version " + std::to_string(h.version) + ", newer than the " +
                            std::to_string(kNewest) + " read: read as that");
    }
    // The grids asked for -- by name, or every one -- and the trees they need.
    std::map<std::string, size_t> byUnique;
    for (size_t i = 0; i < grids.size(); ++i) byUnique.emplace(grids[i].d.unique, i);
    std::vector<bool> wanted(grids.size(), false);
    for (size_t i = 0; i < grids.size(); ++i) {
        const Grid& g = grids[i];
        const bool named = o.grids.empty() || std::find(o.grids.begin(), o.grids.end(), g.d.name) != o.grids.end();
        if (!named) continue;
        if (!g.readable) {
            out.notes.push_back("grid \"" + g.d.name + "\" of " + g.values + " is not read");
            continue;
        }
        wanted[i] = true;
    }
    for (const std::string& name : o.grids) {
        if (std::none_of(grids.begin(), grids.end(), [&](const Grid& g) { return g.d.name == name; })) {
            out.notes.push_back("no grid \"" + name + "\" in the file");
        }
    }
    // Read each grid wanted, and the grid an instance shares its tree with.
    std::vector<bool> loaded(grids.size(), !h.offsets);
    std::function<bool(size_t, int)> need = [&](size_t i, int depth) -> bool {
        Grid& g = grids[i];
        if (!loaded[i]) {
            if (!load(src, g, true, error)) return false;
            loaded[i] = true;
        }
        if (g.d.parent.empty() || g.tree) return true;
        const auto parent = byUnique.find(g.d.parent);
        if (parent == byUnique.end() || parent->second == i || depth > 16) {
            error = "grid \"" + g.d.name + "\" shares the tree of a grid that is not there (" + g.d.parent + ")";
            return false;
        }
        Grid& p = grids[parent->second];
        if (p.kind.comps != g.kind.comps || p.kind.width != g.kind.width || p.kind.real != g.kind.real) {
            error = "grid \"" + g.d.name + "\" shares the tree of a grid of another kind";
            return false;
        }
        if (!need(parent->second, depth + 1)) return false;
        g.tree = p.tree;
        return g.tree != nullptr;
    };
    for (size_t i = 0; i < grids.size(); ++i) {
        if (!wanted[i]) continue;
        if (!need(i, 0)) return false;
        if (grids[i].map.frustum) {
            out.notes.push_back("grid \"" + grids[i].d.name + "\" is in a camera's frustum (NonlinearFrustumMap): not read");
            continue;
        }
        toVolumes(grids[i], o, out);
    }
    return true;
}

}  // namespace

bool vdbGrids(const std::string& path, std::vector<VdbGridInfo>& out, std::string& error, bool zUp) {
    out.clear();
    Source src(path);
    Header h;
    std::vector<Grid> grids;
    if (!scan(src, h, grids, error)) {
        error = path + ": " + error;
        return false;
    }
    for (Grid& g : grids) {
        if (h.offsets && !load(src, g, false, error)) {
            error = path + ": " + error;
            return false;
        }
        VdbGridInfo info;
        info.name = g.d.name;
        info.type = g.values;
        info.gridClass = g.meta.gridClass;
        info.half = g.d.half;
        info.readable = g.readable && !g.map.frustum;
        Map map = g.map;
        if (zUp) map.zUp();
        info.voxel = static_cast<float>(std::min({map.edge(0), map.edge(1), map.edge(2)}));
        if (g.meta.hasMin && g.meta.hasMax && g.meta.min[0] <= g.meta.max[0] && g.meta.min[1] <= g.meta.max[1] &&
            g.meta.min[2] <= g.meta.max[2]) {
            double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
            for (int corner = 0; corner < 8; ++corner) {
                double index[3], world[3];
                for (int r = 0; r < 3; ++r) {
                    index[r] = (corner >> r) & 1 ? g.meta.max[static_cast<size_t>(r)] + 0.5 : g.meta.min[static_cast<size_t>(r)] - 0.5;
                }
                map.apply(index, world);
                for (int c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], world[c]);
                    hi[c] = std::max(hi[c], world[c]);
                }
            }
            info.bounded = true;
            info.lo = Vec3(static_cast<float>(lo[0]), static_cast<float>(lo[1]), static_cast<float>(lo[2]));
            info.hi = Vec3(static_cast<float>(hi[0]), static_cast<float>(hi[1]), static_cast<float>(hi[2]));
        }
        out.push_back(std::move(info));
    }
    return true;
}

bool readVdb(const std::string& path, VdbVolumes& out, std::string& error, const VdbReadOptions& options) {
    Source src(path);
    if (!readFrom(src, out, error, options)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool parseVdb(std::span<const uint8_t> bytes, VdbVolumes& out, std::string& error, const VdbReadOptions& options) {
    Source src(bytes);
    return readFrom(src, out, error, options);
}

}  // namespace pg::io
