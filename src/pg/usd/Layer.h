#pragma once
//
// A USD layer as it is written, read without the library from any of the
// three forms a layer takes:
//
//   .usda   text (Text.cpp)
//   .usdc   the binary "crate" (Crate.cpp): tokens, paths and fields in
//           tables, the tables compressed (LZ4 and USD's integer coding),
//           large arrays compressed too
//   .usdz   a package: an uncompressed zip whose first file is the layer;
//           the other files are what it refers to (Package.cpp)
//
// A layer is a tree of specs: prims -- their specifier (def, over, class),
// type, metadata, the arcs they compose (references, payloads, inherits,
// specializes, variant sets), their children -- and their properties:
// attributes, with a default and samples in time, and relationships, with
// targets. What the specs of many layers compose to is a stage (Stage.h).
//
// Values keep what reading them needs and no more: numbers as doubles --
// ints, halfs, floats, doubles, vectors, matrices, quaternions, arrays of
// them -- text as strings, and dictionaries and list edits of their own.
// Quaternions are imaginary part first, real last (x, y, z, w) whatever the
// form wrote them in; matrices row by row, as USD keeps them.
//
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pg::usd {

struct Value;
using Dictionary = std::vector<std::pair<std::string, Value>>;

/// An item of a list edit: a token, a string or a path in `text`; for a
/// reference or a payload its asset in `text` ("" for one in the same
/// layer stack), its prim in `path` ("" for the default prim) and the
/// layer offset it maps time through: time in the layer x scale + offset.
struct ListItem {
    std::string text;
    std::string path;
    double offset = 0.0, scale = 1.0;

    bool operator==(const ListItem&) const = default;
};

/// A list as an opinion edits it: explicit items replace what weaker
/// opinions said; otherwise items are deleted, then prepended and appended
/// (added: appended where not there yet), then put in order.
struct ListOp {
    bool isExplicit = false;
    std::vector<ListItem> explicitItems, added, prepended, appended, deleted, ordered;

    bool empty() const;
    /// Edits `items` -- the list weaker opinions made -- as this opinion says.
    void apply(std::vector<ListItem>& items) const;
    /// What this opinion alone makes of an empty list.
    std::vector<ListItem> items() const;
};

struct Value {
    enum class Kind : uint8_t {
        None,        ///< no value
        Blocked,     ///< None in the file: a value block, weaker opinions do not count
        Numbers,     ///< bool, the ints, half, float, double and what is made of them
        Strings,     ///< token, string, asset, path
        Dictionary,
        List,        ///< a list edit
    };
    Kind kind = Kind::None;
    std::string type;  ///< as USD names it, without []: "float3", "point3f", "matrix4d", "token", "asset"
    bool array = false;
    int width = 1;     ///< numbers an element is: 3 for a float3, 16 for a matrix4d, 4 for a quaternion
    std::vector<double> numbers;
    std::vector<std::string> strings;
    std::shared_ptr<const Dictionary> dictionary;
    std::shared_ptr<const ListOp> list;

    bool empty() const { return kind == Kind::None; }
    bool blocked() const { return kind == Kind::Blocked; }
    bool isNumbers() const { return kind == Kind::Numbers; }
    bool isStrings() const { return kind == Kind::Strings; }
    /// Elements: numbers / width, or strings; 1 for a dictionary or a list.
    size_t size() const;
    /// The first number, else `fallback`.
    double number(double fallback = 0.0) const { return isNumbers() && !numbers.empty() ? numbers[0] : fallback; }
    /// The first string, else "".
    const std::string& text() const;
    /// An entry of a dictionary; null if none.
    const Value* find(std::string_view key) const;

    static Value makeNumbers(std::string type, int width, std::vector<double> numbers, bool array);
    static Value makeNumber(std::string type, double x) { return makeNumbers(std::move(type), 1, {x}, false); }
    static Value makeString(std::string type, std::string s);
    static Value makeStrings(std::string type, std::vector<std::string> s, bool array);
    static Value makeDictionary(Dictionary d);
    static Value makeList(ListOp op);
    static Value makeBlocked();
};

/// How many numbers a value of USD `type` is ("float3" 3, "matrix4d" 16,
/// "quatf" 4); 0 for a type that is not numbers (token, string, asset...),
/// -1 for a type USD does not have.
int widthOf(std::string_view type);
/// True for the types USD interpolates between samples: half, float, double
/// and what is made of them; not bools, ints, text.
bool interpolates(std::string_view type);

enum class Specifier : uint8_t { Def, Over, Class };

/// An attribute or a relationship, as one layer has it.
struct Property {
    std::string name;             ///< "points", "xformOp:translate", "primvars:st"
    bool relationship = false;
    std::string typeName;         ///< an attribute's, as declared: "point3f[]", "token"
    bool custom = false;
    bool uniform = false;
    bool hasDefault = false;
    Value value;                  ///< the default, when hasDefault (maybe blocked)
    bool hasSamples = false;      ///< timeSamples written, even if empty
    std::vector<double> times;    ///< in order
    std::vector<Value> samples;   ///< one a time; blocked: no value from there to the next
    ListOp targets;               ///< a relationship's targets; an attribute's connections
    Dictionary metadata;          ///< interpolation, elementSize, ...

    const Value* meta(std::string_view key) const;
};

struct PrimSpec {
    std::string name;
    Specifier specifier = Specifier::Over;
    std::string typeName;
    Dictionary metadata;  ///< active, kind, instanceable, hidden, clips, customData...
    ListOp references, payloads, inherits, specializes, apiSchemas, variantSetNames;
    std::vector<std::pair<std::string, std::string>> variantSelections;  ///< set and variant
    std::vector<std::unique_ptr<PrimSpec>> children;
    std::vector<Property> properties;
    struct VariantSet {
        std::string name;
        std::vector<std::unique_ptr<PrimSpec>> variants;  ///< each a spec named after the variant
    };
    std::vector<VariantSet> variantSets;

    PrimSpec* child(std::string_view name);
    const PrimSpec* child(std::string_view name) const;
    /// The child of that name, made (an over) if there is none.
    PrimSpec& ensureChild(std::string_view name);
    Property* property(std::string_view name);
    const Property* property(std::string_view name) const;
    Property& ensureProperty(std::string_view name);
    const VariantSet* variantSet(std::string_view name) const;
    /// Variant `variant` of set `set`, made if there is none.
    PrimSpec& ensureVariant(std::string_view set, std::string_view variant);
    const Value* meta(std::string_view key) const;
    /// The variant chosen for `set` here; "" if none.
    std::string_view selection(std::string_view set) const;
};

struct Layer {
    std::string identifier;  ///< the file it was read from; "a.usdz[b.usdc]" inside a package
    Dictionary metadata;     ///< defaultPrim, upAxis, metersPerUnit, startTimeCode, timeCodesPerSecond...
    std::vector<std::string> subLayers;
    std::vector<std::pair<double, double>> subLayerOffsets;  ///< offset and scale, one a sublayer
    PrimSpec root;           ///< the pseudo-root: its children are the layer's root prims

    const Value* meta(std::string_view key) const;
    double number(std::string_view key, double fallback) const;
    std::string text(std::string_view key) const;
    /// The prim spec at `path` -- "/World/cam", "/Set{lod=high}Tree/leaves" --
    /// through the variants it names; null if there is none. "/": the root.
    const PrimSpec* prim(std::string_view path) const;
};

// --- Paths -------------------------------------------------------------------------------

/// A path's elements: "/A/B{v=x}C.prop" is A, B, {v=x}, C and .prop.
struct PathElement {
    enum class Kind : uint8_t { Prim, Variant, Property };
    Kind kind = Kind::Prim;
    std::string name;     ///< the prim, the set, the property
    std::string variant;  ///< for a variant selection
};
/// The elements of an absolute path; false for text that is not one.
bool splitPath(std::string_view path, std::vector<PathElement>& out);
/// `parent` with the prim `child` under it: "/" + "A" = "/A", "/A" + "B" =
/// "/A/B", "/A{v=x}" + "B" = "/A{v=x}B".
std::string childPath(std::string_view parent, std::string_view child);
/// The prim part of a path: "/A/B.points" -> "/A/B".
std::string_view primPart(std::string_view path);
/// Without the variant selections: "/A{v=x}B" -> "/A/B".
std::string stripVariants(std::string_view path);
/// `path` made absolute from `anchor` (a prim path): "../x" from "/A/B" is "/A/x".
std::string absolutePath(std::string_view path, std::string_view anchor);

// --- Reading -----------------------------------------------------------------------------

/// Parses .usda text into `out`. False, with the line and why, for text that
/// is not USD.
bool parseText(std::string_view text, Layer& out, std::string& error);
/// Reads a crate (.usdc) from its bytes into `out`. False, with why, for
/// bytes that are not one or that it cannot read.
bool readCrate(std::span<const uint8_t> bytes, Layer& out, std::string& error);
/// Reads a layer from its bytes: text or crate, told by what they start with.
bool readLayerBytes(std::span<const uint8_t> bytes, Layer& out, std::string& error);

/// The bytes of a file; "a.usdz[b.usda]": the file b.usda inside the
/// package a.usdz; "a.usdz" alone: the package's first file (its layer).
/// False, with why, when it cannot be read.
bool readFileBytes(const std::string& identifier, std::vector<uint8_t>& out, std::string& error);
/// A layer from a file (.usda, .usdc, .usd -- either -- or .usdz), as
/// readFileBytes() names it. Null, with why, when it cannot be read.
std::shared_ptr<Layer> readLayer(const std::string& identifier, std::string& error);
/// The identifier a layer's asset path names: relative to the layer's own
/// folder, or inside the package the layer is in; absolute as it is.
std::string resolveAsset(std::string_view asset, std::string_view anchorIdentifier);

/// The files in a package (.usdz) in their order, the layer first. False,
/// with why, for a file that is not a zip USD reads.
bool packageFiles(const std::string& path, std::vector<std::string>& names, std::string& error);

}  // namespace pg::usd
