// .usdc: USD's binary "crate". Read from the file's bytes as they lie:
//
//   header   "PXR-USDC", the version (major, minor, patch), where the
//            table of contents is
//   TOKENS   every token (name, type, value) once; LZ4
//   STRINGS  strings as tokens, by number
//   FIELDS   a name (a token) and a value (a "rep") each
//   FIELDSETS each spec's fields, in runs ended by ~0
//   PATHS    the paths as a tree: each an element under the one before it,
//            or under an earlier one it jumps back to
//   SPECS    a path, its fields and its kind (prim, attribute...) each
//
// The tables of numbers are coded (USD's integer coding: differences from
// the one before, in 2-bit classes and 1, 2 or 4 bytes each) and then LZ4
// compressed. A rep is 64 bits: whether it is an array, inlined,
// compressed; its type; and its value where small enough (inlined), else
// where in the file it is. Versions from 0.4.0 (2017) on are read.
#include "pg/usd/Layer.h"

#include "pg/core/Half.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace pg::usd {

namespace {

// --- LZ4 as TfFastCompression wraps it -----------------------------------------------------

/// Decompresses an LZ4 block of `size` bytes onto the end of `out`, which
/// may grow to `limit`. False for bytes that are not one.
bool lz4Block(const uint8_t* in, size_t size, std::vector<uint8_t>& out, size_t limit) {
    const uint8_t* end = in + size;
    while (in < end) {
        const uint8_t token = *in++;
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t more;
            do {
                if (in >= end) return false;
                more = *in++;
                literals += more;
            } while (more == 255);
        }
        if (static_cast<size_t>(end - in) < literals || out.size() + literals > limit) return false;
        out.insert(out.end(), in, in + literals);
        in += literals;
        if (in >= end) break;  // the last sequence has only literals
        if (end - in < 2) return false;
        const size_t offset = static_cast<size_t>(in[0]) | static_cast<size_t>(in[1]) << 8;
        in += 2;
        if (offset == 0 || offset > out.size()) return false;
        size_t match = (token & 15u) + 4u;
        if ((token & 15u) == 15u) {
            uint8_t more;
            do {
                if (in >= end) return false;
                more = *in++;
                match += more;
            } while (more == 255);
        }
        if (out.size() + match > limit) return false;
        const size_t from = out.size() - offset;
        for (size_t k = 0; k < match; ++k) out.push_back(out[from + k]);
    }
    return true;
}

/// TfFastCompression: a byte with the number of chunks, 0 for one; then the
/// block, or each chunk's size (int32) and block. At most `limit` bytes
/// come out; with `exact`, just so many.
bool fastDecompress(const uint8_t* in, size_t size, std::vector<uint8_t>& out, size_t limit, bool exact = true) {
    out.clear();
    out.reserve(limit);
    if (size < 1) return false;
    const int chunks = static_cast<int8_t>(in[0]);
    ++in;
    --size;
    if (chunks == 0) return lz4Block(in, size, out, limit) && (!exact || out.size() == limit);
    for (int c = 0; c < chunks; ++c) {
        if (size < 4) return false;
        int32_t n;
        std::memcpy(&n, in, 4);
        in += 4;
        size -= 4;
        if (n < 0 || static_cast<size_t>(n) > size) return false;
        if (!lz4Block(in, static_cast<size_t>(n), out, limit)) return false;
        in += n;
        size -= static_cast<size_t>(n);
    }
    return !exact || out.size() == limit;
}

/// USD's integer coding, decoded: the most common difference, 2 bits a
/// number saying what it is (00 that difference, then 1, 2 or 4 bytes --
/// 2, 4 or 8 for 64-bit numbers), then those bytes.
template <class Int>
bool decodeInts(const uint8_t* data, size_t size, size_t count, Int* out) {
    using Signed = std::make_signed_t<Int>;
    using Small = std::conditional_t<sizeof(Int) == 4, int8_t, int16_t>;
    using Medium = std::conditional_t<sizeof(Int) == 4, int16_t, int32_t>;
    using Large = std::conditional_t<sizeof(Int) == 4, int32_t, int64_t>;
    if (size < sizeof(Signed)) return count == 0;
    Signed common;
    std::memcpy(&common, data, sizeof common);
    const size_t codeBytes = (count * 2 + 7) / 8;
    if (sizeof(Signed) + codeBytes > size) return false;
    const uint8_t* codes = data + sizeof(Signed);
    const uint8_t* ints = codes + codeBytes;
    const uint8_t* end = data + size;
    Signed previous = 0;
    for (size_t i = 0; i < count; ++i) {
        const unsigned code = (codes[i / 4] >> (2 * (i % 4))) & 3u;
        auto take = [&](auto type) -> bool {
            using T = decltype(type);
            if (static_cast<size_t>(end - ints) < sizeof(T)) return false;
            T v;
            std::memcpy(&v, ints, sizeof v);
            ints += sizeof v;
            previous = static_cast<Signed>(previous + static_cast<Signed>(v));
            return true;
        };
        switch (code) {
            case 0: previous = static_cast<Signed>(previous + common); break;
            case 1:
                if (!take(Small{})) return false;
                break;
            case 2:
                if (!take(Medium{})) return false;
                break;
            default:
                if (!take(Large{})) return false;
        }
        out[i] = static_cast<Int>(previous);
    }
    return true;
}

/// The size the coded form of `count` numbers can take at most.
template <class Int>
size_t codedSize(size_t count) {
    return sizeof(Int) + (count * 2 + 7) / 8 + count * sizeof(Int);
}

// --- The crate ---------------------------------------------------------------------------------

enum Type : uint8_t {
    TBool = 1, TUChar, TInt, TUInt, TInt64, TUInt64, THalf, TFloat, TDouble, TString, TToken, TAsset,
    TMatrix2d, TMatrix3d, TMatrix4d, TQuatd, TQuatf, TQuath,
    TVec2d, TVec2f, TVec2h, TVec2i, TVec3d, TVec3f, TVec3h, TVec3i, TVec4d, TVec4f, TVec4h, TVec4i,
    TDictionary, TTokenListOp, TStringListOp, TPathListOp, TReferenceListOp, TIntListOp, TInt64ListOp,
    TUIntListOp, TUInt64ListOp, TPathVector, TTokenVector, TSpecifier, TPermission, TVariability,
    TVariantSelectionMap, TTimeSamples, TPayload, TDoubleVector, TLayerOffsetVector, TStringVector,
    TValueBlock, TValue, TUnregisteredValue, TUnregisteredValueListOp, TPayloadListOp, TTimeCode,
    TPathExpression, TRelocates, TSpline, TAnimationBlock,
};

/// What a numeric type is made of: its name, the scalar each number is
/// stored as, how many.
struct NumericType {
    const char* name;
    Type scalar;
    int width;
};

bool numeric(uint8_t t, NumericType& out) {
    static const NumericType kTypes[] = {
        {"", TBool, 0},         {"bool", TBool, 1},       {"uchar", TUChar, 1},     {"int", TInt, 1},
        {"uint", TUInt, 1},     {"int64", TInt64, 1},     {"uint64", TUInt64, 1},   {"half", THalf, 1},
        {"float", TFloat, 1},   {"double", TDouble, 1},   {"", TBool, 0},           {"", TBool, 0},
        {"", TBool, 0},         {"matrix2d", TDouble, 4}, {"matrix3d", TDouble, 9}, {"matrix4d", TDouble, 16},
        {"quatd", TDouble, 4},  {"quatf", TFloat, 4},     {"quath", THalf, 4},      {"double2", TDouble, 2},
        {"float2", TFloat, 2},  {"half2", THalf, 2},      {"int2", TInt, 2},        {"double3", TDouble, 3},
        {"float3", TFloat, 3},  {"half3", THalf, 3},      {"int3", TInt, 3},        {"double4", TDouble, 4},
        {"float4", TFloat, 4},  {"half4", THalf, 4},      {"int4", TInt, 4},
    };
    if (t == TTimeCode) {
        out = {"timecode", TDouble, 1};
        return true;
    }
    if (t >= sizeof(kTypes) / sizeof(kTypes[0]) || kTypes[t].width == 0) return false;
    out = kTypes[t];
    return true;
}

size_t scalarSize(Type t) {
    switch (t) {
        case TBool:
        case TUChar: return 1;
        case THalf: return 2;
        case TInt:
        case TUInt:
        case TFloat: return 4;
        default: return 8;
    }
}

double scalarAt(const uint8_t* p, Type t) {
    switch (t) {
        case TBool: return p[0] ? 1.0 : 0.0;
        case TUChar: return p[0];
        case THalf: {
            uint16_t h;
            std::memcpy(&h, p, 2);
            return floatFromHalf(h);
        }
        case TInt: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case TUInt: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case TFloat: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case TInt64: {
            int64_t v;
            std::memcpy(&v, p, 8);
            return static_cast<double>(v);
        }
        case TUInt64: {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return static_cast<double>(v);
        }
        default: {
            double v;
            std::memcpy(&v, p, 8);
            return v;
        }
    }
}

struct PathEntry {
    std::string text;         ///< the whole path
    int parent = -1;
    PathElement::Kind kind = PathElement::Kind::Prim;
    std::string name, variant;
    bool root = false;
};

class Crate {
public:
    explicit Crate(std::span<const uint8_t> bytes) : b_(bytes) {}

    bool read(Layer& layer, std::string& error) {
        if (!header() || !sections() || !buildLayer(layer)) {
            error = error_.empty() ? "a crate it cannot read" : error_;
            return false;
        }
        return true;
    }

private:
    // --- Reading bytes ---------------------------------------------------------------------

    bool fail(const std::string& why) {
        if (error_.empty()) error_ = why;
        return false;
    }
    bool has(size_t at, size_t n) const { return at <= b_.size() && n <= b_.size() - at; }
    template <class T>
    T get(size_t& at) {
        T v{};
        if (!has(at, sizeof(T))) {
            fail("the crate ends early");
            at = b_.size();
            return v;
        }
        std::memcpy(&v, b_.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    bool atLeast(int major, int minor, int patch) const {
        return std::array<int, 3>{major_, minor_, patch_} >= std::array<int, 3>{major, minor, patch};
    }

    /// uint64 size, then compressed numbers (USD's integer coding in LZ4).
    template <class Int>
    bool compressedInts(size_t& at, size_t count, std::vector<Int>& out) {
        const uint64_t size = get<uint64_t>(at);
        if (!has(at, size)) return fail("the crate ends inside a table");
        out.assign(count, 0);
        std::vector<uint8_t> coded;
        const bool ok = count == 0 || (fastDecompress(b_.data() + at, size, coded, codedSize<Int>(count), false) &&
                                       decodeInts(coded.data(), coded.size(), count, out.data()));
        at += size;
        return ok || fail("numbers in the crate that do not decode");
    }

    // --- Structure ---------------------------------------------------------------------------

    bool header() {
        if (b_.size() < 88 || std::memcmp(b_.data(), "PXR-USDC", 8) != 0) return fail("not a crate");
        major_ = b_[8];
        minor_ = b_[9];
        patch_ = b_[10];
        if (!atLeast(0, 4, 0)) {
            return fail("crate version " + std::to_string(major_) + "." + std::to_string(minor_) + "." +
                        std::to_string(patch_) + " is older than 0.4.0: save it again with a newer USD");
        }
        if (major_ > 0) return fail("crate version " + std::to_string(major_) + ".x is newer than this reads");
        size_t at = 16;
        toc_ = get<uint64_t>(at);
        return has(toc_, 8) || fail("no table of contents");
    }

    bool sections() {
        size_t at = toc_;
        const uint64_t count = get<uint64_t>(at);
        if (count > 64) return fail("a table of contents that makes no sense");
        size_t tokens = 0, strings = 0, fields = 0, fieldSets = 0, paths = 0, specs = 0;
        for (uint64_t i = 0; i < count; ++i) {
            if (!has(at, 32)) return fail("the table of contents ends early");
            char name[17] = {};
            std::memcpy(name, b_.data() + at, 16);
            at += 16;
            const uint64_t start = get<uint64_t>(at);
            get<uint64_t>(at);  // size
            const std::string_view n(name);
            if (n == "TOKENS") tokens = start;
            else if (n == "STRINGS") strings = start;
            else if (n == "FIELDS") fields = start;
            else if (n == "FIELDSETS") fieldSets = start;
            else if (n == "PATHS") paths = start;
            else if (n == "SPECS") specs = start;
        }
        if (!tokens || !fields || !fieldSets || !paths || !specs) return fail("a crate without its tables");
        return readTokens(tokens) && readStrings(strings) && readFields(fields) && readFieldSets(fieldSets) &&
               readPaths(paths) && readSpecs(specs);
    }

    bool readTokens(size_t at) {
        const uint64_t count = get<uint64_t>(at);
        const uint64_t size = get<uint64_t>(at);
        const uint64_t compressed = get<uint64_t>(at);
        // LZ4 makes at most 255 bytes of a byte; a token is at least its 0.
        if (!has(at, compressed) || size > compressed * 255 + 64 || count > size) {
            return fail("the tokens do not fit the crate");
        }
        std::vector<uint8_t> chars;
        if (!fastDecompress(b_.data() + at, compressed, chars, size)) return fail("tokens that do not decompress");
        tokens_.clear();
        tokens_.reserve(count);
        size_t start = 0;
        for (size_t i = 0; i < chars.size() && tokens_.size() < count; ++i) {
            if (chars[i] == 0) {
                tokens_.emplace_back(reinterpret_cast<const char*>(chars.data()) + start, i - start);
                start = i + 1;
            }
        }
        if (tokens_.size() != count) return fail("fewer tokens than the crate says");
        return true;
    }

    bool readStrings(size_t at) {
        if (!at) return true;
        const uint64_t count = get<uint64_t>(at);
        if (count > b_.size() / 4 || !has(at, count * 4)) return fail("the strings do not fit the crate");
        strings_.resize(count);
        std::memcpy(strings_.data(), b_.data() + at, count * 4);
        for (const uint32_t s : strings_) {
            if (s >= tokens_.size()) return fail("a string that is no token");
        }
        return true;
    }

    bool readFields(size_t at) {
        const uint64_t count = get<uint64_t>(at);
        if (count > b_.size()) return fail("more fields than the crate has bytes");
        std::vector<uint32_t> names;
        if (!compressedInts(at, count, names)) return false;
        const uint64_t size = get<uint64_t>(at);
        if (!has(at, size)) return fail("the fields do not fit the crate");
        std::vector<uint8_t> reps;
        if (!fastDecompress(b_.data() + at, size, reps, count * 8)) return fail("fields that do not decompress");
        fields_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (names[i] >= tokens_.size()) return fail("a field whose name is no token");
            fields_[i].name = names[i];
            std::memcpy(&fields_[i].rep, reps.data() + i * 8, 8);
        }
        return true;
    }

    bool readFieldSets(size_t at) {
        const uint64_t count = get<uint64_t>(at);
        if (count > b_.size()) return fail("more field sets than the crate has bytes");
        return compressedInts(at, count, fieldSets_);
    }

    bool readPaths(size_t at) {
        const uint64_t count = get<uint64_t>(at);
        const uint64_t encoded = get<uint64_t>(at);
        if (count > b_.size() || encoded > count) return fail("more paths than the crate has bytes");
        std::vector<uint32_t> indexes;
        std::vector<int32_t> elements, jumps;
        if (!compressedInts(at, encoded, indexes) || !compressedInts(at, encoded, elements) ||
            !compressedInts(at, encoded, jumps)) {
            return false;
        }
        paths_.assign(count, {});
        // Each entry is an element under `parent`; then its child is next
        // (jump -1), or its sibling (0), or both, the sibling `jump` on (> 0),
        // or neither (-2).
        struct Pending {
            size_t index;
            int parent;
        };
        std::vector<Pending> todo{{0, -1}};
        std::vector<char> seen(count, 0);
        size_t budget = 2 * static_cast<size_t>(encoded) + 16;
        while (!todo.empty()) {
            Pending p = todo.back();
            todo.pop_back();
            for (;;) {
                if (budget-- == 0) return fail("paths in the crate that go round in circles");
                const size_t i = p.index++;
                if (i >= encoded || indexes[i] >= count) return fail("a path that is not in the crate");
                if (seen[indexes[i]]++) return fail("a path the crate has twice");
                PathEntry& e = paths_[indexes[i]];
                if (p.parent < 0) {
                    e.text = "/";
                    e.root = true;
                } else {
                    const PathEntry& parent = paths_[static_cast<size_t>(p.parent)];
                    const int32_t token = elements[i];
                    const size_t t = static_cast<size_t>(token < 0 ? -static_cast<int64_t>(token) : token);
                    if (t >= tokens_.size()) return fail("a path element that is no token");
                    const std::string& name = tokens_[t];
                    e.parent = p.parent;
                    if (token < 0) {
                        e.kind = PathElement::Kind::Property;
                        e.name = name;
                        e.text = parent.text + "." + name;
                    } else if (!name.empty() && name[0] == '{') {
                        e.kind = PathElement::Kind::Variant;
                        const size_t eq = name.find('=');
                        e.name = name.substr(1, eq == std::string::npos ? 0 : eq - 1);
                        e.variant = eq == std::string::npos ? "" : name.substr(eq + 1, name.size() - eq - 2);
                        e.text = parent.text + name;
                    } else {
                        e.kind = PathElement::Kind::Prim;
                        e.name = name;
                        e.text = childPath(parent.text, name);
                    }
                }
                const int32_t jump = jumps[i];
                const bool child = jump > 0 || jump == -1;
                const bool sibling = jump >= 0;
                if (child) {
                    if (sibling) todo.push_back({i + static_cast<size_t>(jump), p.parent});
                    p.parent = static_cast<int>(indexes[i]);
                }
                if (!child && !sibling) break;
            }
        }
        return true;
    }

    bool readSpecs(size_t at) {
        const uint64_t count = get<uint64_t>(at);
        if (count > b_.size()) return fail("more specs than the crate has bytes");
        std::vector<uint32_t> paths, sets, types;
        if (!compressedInts(at, count, paths) || !compressedInts(at, count, sets) || !compressedInts(at, count, types)) {
            return false;
        }
        specs_.resize(count);
        for (size_t i = 0; i < count; ++i) specs_[i] = {paths[i], sets[i], types[i]};
        return true;
    }

    // --- Values ------------------------------------------------------------------------------

    static constexpr uint64_t kArray = 1ull << 63, kInlined = 1ull << 62, kCompressed = 1ull << 61;
    static constexpr uint64_t kPayload = (1ull << 48) - 1;
    static uint8_t typeOf(uint64_t rep) { return static_cast<uint8_t>((rep >> 48) & 0xFFu); }

    const std::string& token(uint64_t index) {
        static const std::string none;
        if (index >= tokens_.size()) {
            fail("a value naming a token that is not there");
            return none;
        }
        return tokens_[index];
    }
    const std::string& string(uint64_t index) {
        static const std::string none;
        if (index >= strings_.size()) {
            fail("a value naming a string that is not there");
            return none;
        }
        return tokens_[strings_[index]];
    }
    const std::string& path(uint64_t index) {
        static const std::string none;
        if (index >= paths_.size()) {
            fail("a value naming a path that is not there");
            return none;
        }
        return paths_[index].text;
    }

    /// An array's length: after a rank before 0.5.0, 32 bits before 0.7.0.
    uint64_t arrayLength(size_t& at) {
        if (!atLeast(0, 5, 0)) get<uint32_t>(at);
        return atLeast(0, 7, 0) ? get<uint64_t>(at) : get<uint32_t>(at);
    }

    /// `count` scalars of `scalar` at `at`, onto `out`.
    bool scalars(size_t& at, Type scalar, size_t count, std::vector<double>& out) {
        const size_t size = scalarSize(scalar);
        if (count > b_.size() || !has(at, count * size)) return fail("an array that does not fit the crate");
        out.reserve(out.size() + count);
        for (size_t k = 0; k < count; ++k) out.push_back(scalarAt(b_.data() + at + k * size, scalar));
        at += count * size;
        return true;
    }

    Value numbers(uint64_t rep, const NumericType& t) {
        std::vector<double> out;
        const uint64_t payload = rep & kPayload;
        const bool quat = t.name[0] == 'q';
        if (!(rep & kArray)) {
            if (rep & kInlined) {
                uint8_t bytes[4];
                const uint32_t bits = static_cast<uint32_t>(payload);
                std::memcpy(bytes, &bits, 4);
                if (t.width > 1 && !quat) {
                    // Vectors of small whole numbers: a byte each; matrices
                    // with only a diagonal: its numbers.
                    const bool matrix = t.name[0] == 'm';
                    const int n = matrix ? (t.width == 4 ? 2 : t.width == 9 ? 3 : 4) : t.width;
                    out.assign(static_cast<size_t>(t.width), 0.0);
                    for (int k = 0; k < n; ++k) {
                        const double v = static_cast<int8_t>(bytes[k]);
                        out[static_cast<size_t>(matrix ? k * n + k : k)] = v;
                    }
                } else if (t.scalar == TDouble || t.scalar == TInt64 || t.scalar == TUInt64) {
                    // A double that a float holds exactly: as that float;
                    // 64-bit whole numbers that 32 bits hold: as those.
                    if (t.scalar == TDouble) {
                        float f;
                        std::memcpy(&f, &bits, 4);
                        out.push_back(f);
                    } else if (t.scalar == TInt64) {
                        out.push_back(static_cast<int32_t>(bits));
                    } else {
                        out.push_back(bits);
                    }
                } else {
                    out.push_back(scalarAt(bytes, t.scalar));
                }
            } else {
                size_t at = payload;
                scalars(at, t.scalar, static_cast<size_t>(t.width), out);
            }
        } else if (payload != 0) {
            size_t at = payload;
            const bool integral = t.scalar == TInt || t.scalar == TUInt || t.scalar == TInt64 || t.scalar == TUInt64;
            const bool floating = t.scalar == THalf || t.scalar == TFloat || t.scalar == TDouble;
            // Compressed: whole numbers from 0.5.0, floating point from 0.6.0;
            // arrays of fewer than 16 are stored as they are all the same.
            size_t probe = at;
            const uint64_t length = (rep & kCompressed) && t.width == 1 ? arrayLength(probe) : 0;
            const bool compressed = (rep & kCompressed) && t.width == 1 && length >= 16 &&
                                    ((integral && atLeast(0, 5, 0)) || (floating && atLeast(0, 6, 0)));
            if (compressed && integral) {
                const uint64_t count = arrayLength(at);
                if (count > b_.size() * 8) {
                    fail("an array longer than the crate");
                    return {};
                }
                if (t.scalar == TInt64 || t.scalar == TUInt64) {
                    std::vector<int64_t> ints;
                    compressedInts(at, count, ints);
                    out.assign(ints.begin(), ints.end());
                    if (t.scalar == TUInt64) {
                        for (size_t k = 0; k < ints.size(); ++k) out[k] = static_cast<double>(static_cast<uint64_t>(ints[k]));
                    }
                } else {
                    std::vector<int32_t> ints;
                    compressedInts(at, count, ints);
                    out.reserve(count);
                    for (const int32_t v : ints) {
                        out.push_back(t.scalar == TUInt ? static_cast<double>(static_cast<uint32_t>(v)) : v);
                    }
                }
            } else if (compressed && floating) {
                const uint64_t count = arrayLength(at);
                if (count > b_.size() * 8) {
                    fail("an array longer than the crate");
                    return {};
                }
                const char code = static_cast<char>(get<int8_t>(at));
                if (code == 'i') {
                    std::vector<int32_t> ints;
                    compressedInts(at, count, ints);
                    out.assign(ints.begin(), ints.end());
                } else if (code == 't') {
                    const uint32_t lutSize = get<uint32_t>(at);
                    std::vector<double> lut;
                    scalars(at, t.scalar, lutSize, lut);
                    std::vector<uint32_t> indexes;
                    compressedInts(at, count, indexes);
                    out.reserve(count);
                    for (const uint32_t k : indexes) {
                        if (k >= lut.size()) {
                            fail("a compressed array naming a value that is not there");
                            return {};
                        }
                        out.push_back(lut[k]);
                    }
                } else {
                    fail("a compressed array of an unknown kind");
                    return {};
                }
            } else {
                const uint64_t count = arrayLength(at);
                if (count > b_.size() / scalarSize(t.scalar) / static_cast<size_t>(t.width) + 1) {
                    fail("an array longer than the crate");
                    return {};
                }
                scalars(at, t.scalar, static_cast<size_t>(count) * static_cast<size_t>(t.width), out);
            }
        }
        Value v = Value::makeNumbers(t.name, t.width, std::move(out), (rep & kArray) != 0);
        return v;
    }

    /// Strings stored as numbers: tokens, strings, assets, paths.
    template <class Name>
    std::vector<std::string> names(size_t& at, uint64_t count, Name&& name) {
        std::vector<std::string> out;
        if (count > b_.size() / 4 + 1 || !has(at, count * 4)) {
            fail("a list that does not fit the crate");
            return out;
        }
        out.reserve(count);
        for (uint64_t k = 0; k < count; ++k) out.push_back(name(get<uint32_t>(at)));
        return out;
    }

    /// Tokens, strings, assets: inlined as a token (a string: a string), in
    /// arrays as tokens (strings and assets: strings).
    Value texts(uint64_t rep, const char* type, bool isString, bool stringsInArrays) {
        const uint64_t payload = rep & kPayload;
        if (!(rep & kArray)) return Value::makeString(type, isString ? string(payload) : token(payload));
        auto name = [&](uint64_t index) -> std::string {
            return isString || stringsInArrays ? string(index) : token(index);
        };
        std::vector<std::string> out;
        if (payload != 0) {
            size_t at = payload;
            const uint64_t count = arrayLength(at);
            out = names(at, count, name);
        }
        return Value::makeStrings(type, std::move(out), true);
    }

    Dictionary dictionaryAt(size_t& at) {
        Dictionary d;
        const uint64_t count = get<uint64_t>(at);
        if (count > b_.size()) {
            fail("a dictionary longer than the crate");
            return d;
        }
        for (uint64_t k = 0; k < count && error_.empty(); ++k) {
            std::string key = string(get<uint32_t>(at));
            d.emplace_back(std::move(key), valueBehind(at));
        }
        return d;
    }

    /// A value written in its own run: an int64 from here past the value's
    /// data, then its rep.
    Value valueBehind(size_t& at) {
        const size_t start = at;
        const int64_t offset = get<int64_t>(at);
        if (offset < 8 || !has(start, static_cast<size_t>(offset) + 8)) {
            fail("a value that does not fit the crate");
            at = b_.size();
            return {};
        }
        at = start + static_cast<size_t>(offset);
        const uint64_t rep = get<uint64_t>(at);
        if (++depth_ > 64) {
            fail("values nested too deep");
            return {};
        }
        Value v = unpack(rep);
        --depth_;
        return v;
    }

    ListItem pathItem(size_t& at) {
        ListItem i;
        i.text = path(get<uint32_t>(at));
        return i;
    }

    ListItem referenceItem(size_t& at, bool payload) {
        ListItem i;
        i.text = string(get<uint32_t>(at));
        i.path = path(get<uint32_t>(at));
        if (!payload || atLeast(0, 8, 0)) {
            i.offset = get<double>(at);
            i.scale = get<double>(at);
        }
        if (!payload) dictionaryAt(at);  // customData
        return i;
    }

    Value listOp(uint64_t rep, uint8_t type) {
        size_t at = rep & kPayload;
        ListOp op;
        const uint8_t header = get<uint8_t>(at);
        op.isExplicit = header & 1u;
        auto items = [&](std::vector<ListItem>& out) {
            const uint64_t count = get<uint64_t>(at);
            if (count > b_.size()) {
                fail("a list longer than the crate");
                return;
            }
            for (uint64_t k = 0; k < count && error_.empty(); ++k) {
                ListItem i;
                switch (type) {
                    case TTokenListOp: i.text = token(get<uint32_t>(at)); break;
                    case TStringListOp: i.text = string(get<uint32_t>(at)); break;
                    case TPathListOp: i = pathItem(at); break;
                    case TReferenceListOp: i = referenceItem(at, false); break;
                    case TPayloadListOp: i = referenceItem(at, true); break;
                    case TIntListOp:
                    case TUIntListOp: i.text = std::to_string(get<int32_t>(at)); break;
                    default: i.text = std::to_string(get<int64_t>(at)); break;
                }
                out.push_back(std::move(i));
            }
        };
        if (header & 2u) items(op.explicitItems);
        if (header & 4u) items(op.added);
        if (header & 32u) items(op.prepended);
        if (header & 64u) items(op.appended);
        if (header & 8u) items(op.deleted);
        if (header & 16u) items(op.ordered);
        return Value::makeList(std::move(op));
    }

    Value unpack(uint64_t rep) {
        const uint8_t type = typeOf(rep);
        const uint64_t payload = rep & kPayload;
        NumericType t;
        if (numeric(type, t)) return numbers(rep, t);
        switch (type) {
            case TToken: return texts(rep, "token", false, false);
            case TString: return texts(rep, "string", true, true);
            case TAsset: return texts(rep, "asset", false, true);
            case TPathExpression: return texts(rep, "pathExpression", true, true);
            case TSpecifier: return Value::makeNumber("specifier", static_cast<double>(payload));
            case TVariability: return Value::makeNumber("variability", static_cast<double>(payload));
            case TPermission: return Value::makeNumber("permission", static_cast<double>(payload));
            case TValueBlock:
            case TAnimationBlock: return Value::makeBlocked();
            case TDictionary: {
                if (rep & kInlined) return Value::makeDictionary({});
                size_t at = payload;
                return Value::makeDictionary(dictionaryAt(at));
            }
            case TTokenListOp:
            case TStringListOp:
            case TPathListOp:
            case TReferenceListOp:
            case TPayloadListOp:
            case TIntListOp:
            case TInt64ListOp:
            case TUIntListOp:
            case TUInt64ListOp: return listOp(rep, type);
            case TPathVector:
            case TTokenVector:
            case TStringVector: {
                size_t at = payload;
                const uint64_t count = get<uint64_t>(at);
                std::vector<std::string> out;
                if (type == TPathVector) out = names(at, count, [&](uint64_t i) { return path(i); });
                else if (type == TTokenVector) out = names(at, count, [&](uint64_t i) { return token(i); });
                else out = names(at, count, [&](uint64_t i) { return string(i); });
                return Value::makeStrings(type == TPathVector ? "path" : type == TTokenVector ? "token" : "string",
                                          std::move(out), true);
            }
            case TDoubleVector:
            case TLayerOffsetVector: {
                size_t at = payload;
                const uint64_t count = get<uint64_t>(at);
                if (count > b_.size() / 8) {
                    fail("a list longer than the crate");
                    return {};
                }
                const int width = type == TDoubleVector ? 1 : 2;
                std::vector<double> out;
                scalars(at, TDouble, static_cast<size_t>(count) * static_cast<size_t>(width), out);
                return Value::makeNumbers(type == TDoubleVector ? "double" : "double2", width, std::move(out), true);
            }
            case TVariantSelectionMap: {
                size_t at = payload;
                const uint64_t count = get<uint64_t>(at);
                Dictionary d;
                for (uint64_t k = 0; k < count && error_.empty(); ++k) {
                    std::string set = string(get<uint32_t>(at));
                    d.emplace_back(std::move(set), Value::makeString("string", string(get<uint32_t>(at))));
                }
                return Value::makeDictionary(std::move(d));
            }
            case TPayload: {
                size_t at = payload;
                ListOp op;
                op.isExplicit = true;
                op.explicitItems.push_back(referenceItem(at, true));
                return Value::makeList(std::move(op));
            }
            case TValue: {
                size_t at = payload;
                return valueBehind(at);
            }
            default: return {};  // unregistered values, relocates, splines: not read
        }
    }

    /// Time samples: their times as a value in its own run (valueBehind);
    /// then an int64 past the values' data, their number and a rep each.
    void timeSamples(uint64_t rep, Property& p) {
        size_t at = rep & kPayload;
        const Value times = valueBehind(at);
        const size_t start = at;
        const int64_t offset = get<int64_t>(at);
        if (offset < 8 || !has(start, static_cast<size_t>(offset) + 8)) {
            fail("time samples that do not fit the crate");
            return;
        }
        at = start + static_cast<size_t>(offset);
        const uint64_t count = get<uint64_t>(at);
        if (count != times.numbers.size() || !has(at, count * 8)) {
            fail("time samples whose times and values differ in number");
            return;
        }
        p.hasSamples = true;
        p.times = times.numbers;
        p.samples.clear();
        p.samples.reserve(count);
        for (uint64_t k = 0; k < count && error_.empty(); ++k) p.samples.push_back(unpack(get<uint64_t>(at)));
        if (p.samples.size() != p.times.size()) return;
        // In order of time, once each, none NaN -- as USD's map of them is;
        // a file that says otherwise is not trusted to.
        const bool nan = std::any_of(p.times.begin(), p.times.end(), [](double t) { return std::isnan(t); });
        if (nan || !std::is_sorted(p.times.begin(), p.times.end()) ||
            std::adjacent_find(p.times.begin(), p.times.end()) != p.times.end()) {
            std::vector<size_t> order;
            for (size_t k = 0; k < p.times.size(); ++k) {
                if (!std::isnan(p.times[k])) order.push_back(k);
            }
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p.times[a] < p.times[b]; });
            std::vector<double> times;
            std::vector<Value> samples;
            for (const size_t k : order) {
                if (!times.empty() && times.back() == p.times[k]) continue;
                times.push_back(p.times[k]);
                samples.push_back(std::move(p.samples[k]));
            }
            p.times = std::move(times);
            p.samples = std::move(samples);
        }
    }

    // --- Specs into the layer ------------------------------------------------------------------

    /// The prim spec at path entry `index`, made along the way.
    PrimSpec* primAt(Layer& layer, uint32_t index, int depth = 0) {
        if (index >= paths_.size() || depth > 4096) return nullptr;
        const PathEntry& e = paths_[index];
        if (e.root || e.parent < 0) return &layer.root;
        PrimSpec* parent = primAt(layer, static_cast<uint32_t>(e.parent), depth + 1);
        if (!parent) return nullptr;
        switch (e.kind) {
            case PathElement::Kind::Prim: return &parent->ensureChild(e.name);
            case PathElement::Kind::Variant: return &parent->ensureVariant(e.name, e.variant);
            default: return nullptr;
        }
    }

    template <class Fn>
    void eachField(uint32_t set, Fn&& fn) {
        for (size_t k = set; k < fieldSets_.size() && fieldSets_[k] != ~0u && error_.empty(); ++k) {
            const uint32_t f = fieldSets_[k];
            if (f >= fields_.size()) {
                fail("a spec naming a field that is not there");
                return;
            }
            fn(tokens_[fields_[f].name], fields_[f].rep);
        }
    }

    static void order(std::vector<std::unique_ptr<PrimSpec>>& items, const std::vector<std::string>& names) {
        std::vector<std::unique_ptr<PrimSpec>> sorted;
        for (const std::string& n : names) {
            for (auto& i : items) {
                if (i && i->name == n) sorted.push_back(std::move(i));
            }
        }
        for (auto& i : items) {
            if (i) sorted.push_back(std::move(i));
        }
        items = std::move(sorted);
    }

    void primFields(PrimSpec& p, uint32_t set, bool variant) {
        std::vector<std::string> children, properties;
        eachField(set, [&](const std::string& name, uint64_t rep) {
            if (name == "specifier") {
                const auto s = static_cast<int>(rep & kPayload);
                p.specifier = s == 0 ? Specifier::Def : s == 2 ? Specifier::Class : Specifier::Over;
            } else if (name == "typeName") {
                p.typeName = unpack(rep).text();
            } else if (name == "primChildren") {
                children = unpack(rep).strings;
            } else if (name == "properties") {
                properties = unpack(rep).strings;
            } else if (name == "variantSetChildren" || name == "variantChildren") {
            } else if (name == "variantSelection") {
                const Value v = unpack(rep);
                if (v.dictionary) {
                    for (const auto& [s, sel] : *v.dictionary) p.variantSelections.emplace_back(s, sel.text());
                }
            } else if (name == "references" || name == "payload" || name == "inheritPaths" || name == "specializes" ||
                       name == "apiSchemas" || name == "variantSetNames") {
                const Value v = unpack(rep);
                if (!v.list) return;
                ListOp& op = name == "references"     ? p.references
                             : name == "payload"      ? p.payloads
                             : name == "inheritPaths" ? p.inherits
                             : name == "specializes"  ? p.specializes
                             : name == "apiSchemas"   ? p.apiSchemas
                                                      : p.variantSetNames;
                op = *v.list;
            } else {
                p.metadata.emplace_back(name, unpack(rep));
            }
        });
        (void)variant;
        order(p.children, children);
        if (!properties.empty()) {
            std::vector<Property> sorted;
            for (const std::string& n : properties) {
                for (Property& q : p.properties) {
                    if (q.name == n && !q.name.empty()) {
                        sorted.push_back(std::move(q));
                        q.name.clear();
                    }
                }
            }
            for (Property& q : p.properties) {
                if (!q.name.empty()) sorted.push_back(std::move(q));
            }
            p.properties = std::move(sorted);
        }
    }

    void propertyFields(Property& a, uint32_t set) {
        eachField(set, [&](const std::string& name, uint64_t rep) {
            if (name == "typeName") {
                a.typeName = unpack(rep).text();
            } else if (name == "default") {
                a.value = unpack(rep);
                a.hasDefault = true;
            } else if (name == "timeSamples") {
                timeSamples(rep, a);
            } else if (name == "variability") {
                a.uniform = (rep & kPayload) != 0;  // 2, "config", is uniform too
            } else if (name == "custom") {
                a.custom = unpack(rep).number() != 0.0;
            } else if (name == "connectionPaths" || name == "targetPaths") {
                const Value v = unpack(rep);
                if (v.list) a.targets = *v.list;
            } else {
                a.metadata.emplace_back(name, unpack(rep));
            }
        });
    }

    bool buildLayer(Layer& layer) {
        // Properties before the ordering of their prims: prims first, so a
        // prim's fields order the properties already there.
        std::vector<size_t> prims, properties;
        for (size_t i = 0; i < specs_.size(); ++i) {
            const uint32_t kind = specs_[i].type;
            if (kind == 1 || kind == 8) properties.push_back(i);
            else if (kind == 6 || kind == 7 || kind == 10) prims.push_back(i);
        }
        for (const size_t i : properties) {
            const Spec& s = specs_[i];
            if (s.path >= paths_.size() || paths_[s.path].kind != PathElement::Kind::Property) continue;
            PrimSpec* owner = primAt(layer, static_cast<uint32_t>(paths_[s.path].parent));
            if (!owner) continue;
            Property& a = owner->ensureProperty(paths_[s.path].name);
            a.relationship = s.type == 8;
            propertyFields(a, s.fieldSet);
        }
        for (const size_t i : prims) {
            const Spec& s = specs_[i];
            if (s.type == 7) {
                pseudoRootFields(layer, s.fieldSet);
                continue;
            }
            PrimSpec* p = primAt(layer, s.path);
            if (p) primFields(*p, s.fieldSet, s.type == 10);
        }
        return error_.empty();
    }

    void pseudoRootFields(Layer& layer, uint32_t set) {
        std::vector<std::string> children;
        eachField(set, [&](const std::string& name, uint64_t rep) {
            if (name == "primChildren") {
                children = unpack(rep).strings;
            } else if (name == "subLayers") {
                layer.subLayers = unpack(rep).strings;
            } else if (name == "subLayerOffsets") {
                const Value v = unpack(rep);
                layer.subLayerOffsets.clear();
                for (size_t k = 0; k + 1 < v.numbers.size(); k += 2) {
                    layer.subLayerOffsets.emplace_back(v.numbers[k], v.numbers[k + 1]);
                }
            } else {
                layer.metadata.emplace_back(name, unpack(rep));
            }
        });
        layer.subLayerOffsets.resize(layer.subLayers.size(), {0.0, 1.0});
        order(layer.root.children, children);
    }

    struct Field {
        uint32_t name = 0;
        uint64_t rep = 0;
    };
    struct Spec {
        uint32_t path = 0, fieldSet = 0, type = 0;
    };

    std::span<const uint8_t> b_;
    int major_ = 0, minor_ = 0, patch_ = 0;
    uint64_t toc_ = 0;
    std::vector<std::string> tokens_;
    std::vector<uint32_t> strings_;
    std::vector<Field> fields_;
    std::vector<uint32_t> fieldSets_;
    std::vector<PathEntry> paths_;
    std::vector<Spec> specs_;
    int depth_ = 0;
    std::string error_;
};

}  // namespace

bool readCrate(std::span<const uint8_t> bytes, Layer& out, std::string& error) {
    Crate crate(bytes);
    return crate.read(out, error);
}

}  // namespace pg::usd
