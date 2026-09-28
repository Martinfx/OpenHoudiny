#include "pg/usd/Layer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace pg::usd {

// --- List edits ------------------------------------------------------------------------------

bool ListOp::empty() const {
    return !isExplicit && explicitItems.empty() && added.empty() && prepended.empty() && appended.empty() &&
           deleted.empty() && ordered.empty();
}

namespace {

void removeAll(std::vector<ListItem>& items, const ListItem& item) {
    items.erase(std::remove(items.begin(), items.end(), item), items.end());
}

bool contains(const std::vector<ListItem>& items, const ListItem& item) {
    return std::find(items.begin(), items.end(), item) != items.end();
}

}  // namespace

void ListOp::apply(std::vector<ListItem>& items) const {
    if (isExplicit) {
        items.clear();
        for (const ListItem& i : explicitItems) {
            if (!contains(items, i)) items.push_back(i);
        }
        return;
    }
    for (const ListItem& i : deleted) removeAll(items, i);
    for (const ListItem& i : added) {
        if (!contains(items, i)) items.push_back(i);
    }
    // Prepended: at the front in their order, wherever they were before.
    std::vector<ListItem> front;
    for (const ListItem& i : prepended) {
        if (!contains(front, i)) front.push_back(i);
    }
    for (const ListItem& i : front) removeAll(items, i);
    items.insert(items.begin(), front.begin(), front.end());
    for (const ListItem& i : appended) {
        removeAll(items, i);
        items.push_back(i);
    }
    if (!ordered.empty()) {
        std::vector<ListItem> sorted;
        for (const ListItem& i : ordered) {
            if (contains(items, i) && !contains(sorted, i)) sorted.push_back(i);
        }
        for (const ListItem& i : items) {
            if (!contains(sorted, i)) sorted.push_back(i);
        }
        items = std::move(sorted);
    }
}

std::vector<ListItem> ListOp::items() const {
    std::vector<ListItem> out;
    apply(out);
    return out;
}

// --- Values ----------------------------------------------------------------------------------

size_t Value::size() const {
    switch (kind) {
        case Kind::Numbers: return width > 0 ? numbers.size() / static_cast<size_t>(width) : 0;
        case Kind::Strings: return strings.size();
        case Kind::Dictionary:
        case Kind::List: return 1;
        default: return 0;
    }
}

const std::string& Value::text() const {
    static const std::string none;
    return isStrings() && !strings.empty() ? strings[0] : none;
}

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Dictionary || !dictionary) return nullptr;
    for (const auto& [name, value] : *dictionary) {
        if (name == key) return &value;
    }
    return nullptr;
}

Value Value::makeNumbers(std::string type, int width, std::vector<double> numbers, bool array) {
    Value v;
    v.kind = Kind::Numbers;
    v.type = std::move(type);
    v.width = std::max(width, 1);
    v.numbers = std::move(numbers);
    v.array = array;
    return v;
}

Value Value::makeString(std::string type, std::string s) {
    Value v;
    v.kind = Kind::Strings;
    v.type = std::move(type);
    v.strings.push_back(std::move(s));
    return v;
}

Value Value::makeStrings(std::string type, std::vector<std::string> s, bool array) {
    Value v;
    v.kind = Kind::Strings;
    v.type = std::move(type);
    v.strings = std::move(s);
    v.array = array;
    return v;
}

Value Value::makeDictionary(Dictionary d) {
    Value v;
    v.kind = Kind::Dictionary;
    v.type = "dictionary";
    v.dictionary = std::make_shared<const Dictionary>(std::move(d));
    return v;
}

Value Value::makeList(ListOp op) {
    Value v;
    v.kind = Kind::List;
    v.type = "listOp";
    v.list = std::make_shared<const ListOp>(std::move(op));
    return v;
}

Value Value::makeBlocked() {
    Value v;
    v.kind = Kind::Blocked;
    return v;
}

int widthOf(std::string_view type) {
    if (type.size() > 2 && type.substr(type.size() - 2) == "[]") type.remove_suffix(2);
    static const std::pair<std::string_view, int> kTypes[] = {
        {"bool", 1}, {"uchar", 1}, {"int", 1}, {"uint", 1}, {"int64", 1}, {"uint64", 1}, {"half", 1},
        {"float", 1}, {"double", 1}, {"timecode", 1},
        {"string", 0}, {"token", 0}, {"asset", 0}, {"opaque", 0}, {"group", 0}, {"pathExpression", 0},
        {"dictionary", 0},
        {"matrix2d", 4}, {"matrix3d", 9}, {"matrix4d", 16}, {"frame4d", 16},
        {"quatd", 4}, {"quatf", 4}, {"quath", 4},
        {"double2", 2}, {"float2", 2}, {"half2", 2}, {"int2", 2},
        {"double3", 3}, {"float3", 3}, {"half3", 3}, {"int3", 3},
        {"double4", 4}, {"float4", 4}, {"half4", 4}, {"int4", 4},
        {"point3d", 3}, {"point3f", 3}, {"point3h", 3},
        {"normal3d", 3}, {"normal3f", 3}, {"normal3h", 3},
        {"vector3d", 3}, {"vector3f", 3}, {"vector3h", 3},
        {"color3d", 3}, {"color3f", 3}, {"color3h", 3},
        {"color4d", 4}, {"color4f", 4}, {"color4h", 4},
        {"texCoord2d", 2}, {"texCoord2f", 2}, {"texCoord2h", 2},
        {"texCoord3d", 3}, {"texCoord3f", 3}, {"texCoord3h", 3},
    };
    for (const auto& [name, width] : kTypes) {
        if (name == type) return width;
    }
    return -1;
}

bool interpolates(std::string_view type) {
    if (type.size() > 2 && type.substr(type.size() - 2) == "[]") type.remove_suffix(2);
    for (const std::string_view prefix : {"double", "float", "half", "matrix", "quat"}) {
        if (type.rfind(prefix, 0) == 0) return true;
    }
    if (type == "timecode" || type == "frame4d") return true;
    // Roles: point3f, normal3d, color4h, texCoord2f...
    if (widthOf(type) < 2) return false;
    const char last = type.back();
    return last == 'd' || last == 'f' || last == 'h';
}

// --- Specs -----------------------------------------------------------------------------------

namespace {

const Value* findIn(const Dictionary& d, std::string_view key) {
    for (const auto& [name, value] : d) {
        if (name == key) return &value;
    }
    return nullptr;
}

}  // namespace

const Value* Property::meta(std::string_view key) const { return findIn(metadata, key); }

PrimSpec* PrimSpec::child(std::string_view name) {
    for (auto& c : children) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

const PrimSpec* PrimSpec::child(std::string_view name) const {
    for (const auto& c : children) {
        if (c->name == name) return c.get();
    }
    return nullptr;
}

PrimSpec& PrimSpec::ensureChild(std::string_view name) {
    if (PrimSpec* c = child(name)) return *c;
    children.push_back(std::make_unique<PrimSpec>());
    children.back()->name = std::string(name);
    return *children.back();
}

Property* PrimSpec::property(std::string_view name) {
    for (Property& p : properties) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const Property* PrimSpec::property(std::string_view name) const {
    for (const Property& p : properties) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

Property& PrimSpec::ensureProperty(std::string_view name) {
    if (Property* p = property(name)) return *p;
    properties.emplace_back();
    properties.back().name = std::string(name);
    return properties.back();
}

const PrimSpec::VariantSet* PrimSpec::variantSet(std::string_view name) const {
    for (const VariantSet& s : variantSets) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

PrimSpec& PrimSpec::ensureVariant(std::string_view set, std::string_view variant) {
    VariantSet* s = nullptr;
    for (VariantSet& v : variantSets) {
        if (v.name == set) s = &v;
    }
    if (!s) {
        variantSets.emplace_back();
        s = &variantSets.back();
        s->name = std::string(set);
    }
    for (auto& v : s->variants) {
        if (v->name == variant) return *v;
    }
    s->variants.push_back(std::make_unique<PrimSpec>());
    s->variants.back()->name = std::string(variant);
    return *s->variants.back();
}

const Value* PrimSpec::meta(std::string_view key) const { return findIn(metadata, key); }

std::string_view PrimSpec::selection(std::string_view set) const {
    for (const auto& [name, variant] : variantSelections) {
        if (name == set) return variant;
    }
    return {};
}

const Value* Layer::meta(std::string_view key) const { return findIn(metadata, key); }

double Layer::number(std::string_view key, double fallback) const {
    const Value* v = meta(key);
    return v ? v->number(fallback) : fallback;
}

std::string Layer::text(std::string_view key) const {
    const Value* v = meta(key);
    return v ? v->text() : std::string();
}

const PrimSpec* Layer::prim(std::string_view path) const {
    std::vector<PathElement> elements;
    if (!splitPath(path, elements)) return nullptr;
    const PrimSpec* at = &root;
    for (const PathElement& e : elements) {
        if (e.kind == PathElement::Kind::Property) return nullptr;
        if (e.kind == PathElement::Kind::Prim) {
            at = at->child(e.name);
        } else {
            const PrimSpec::VariantSet* set = at->variantSet(e.name);
            at = nullptr;
            if (set) {
                for (const auto& v : set->variants) {
                    if (v->name == e.variant) at = v.get();
                }
            }
        }
        if (!at) return nullptr;
    }
    return at;
}

// --- Paths -----------------------------------------------------------------------------------

bool splitPath(std::string_view path, std::vector<PathElement>& out) {
    out.clear();
    if (path.empty() || path[0] != '/') return false;
    size_t i = 1;
    bool afterVariant = false;
    while (i < path.size()) {
        const char c = path[i];
        if (c == '/') {
            if (afterVariant || out.empty() || out.back().kind != PathElement::Kind::Prim) return false;
            ++i;
            continue;
        }
        if (c == '{') {
            const size_t close = path.find('}', i);
            const size_t eq = path.find('=', i);
            if (close == std::string_view::npos || eq == std::string_view::npos || eq > close || out.empty()) return false;
            PathElement e;
            e.kind = PathElement::Kind::Variant;
            e.name = std::string(path.substr(i + 1, eq - i - 1));
            e.variant = std::string(path.substr(eq + 1, close - eq - 1));
            out.push_back(std::move(e));
            i = close + 1;
            afterVariant = true;
            continue;
        }
        if (c == '.') {
            PathElement e;
            e.kind = PathElement::Kind::Property;
            e.name = std::string(path.substr(i + 1));
            if (e.name.empty()) return false;
            out.push_back(std::move(e));
            return true;
        }
        size_t end = i;
        while (end < path.size() && path[end] != '/' && path[end] != '{' && path[end] != '.') ++end;
        PathElement e;
        e.name = std::string(path.substr(i, end - i));
        out.push_back(std::move(e));
        i = end;
        afterVariant = false;
    }
    return true;
}

std::string childPath(std::string_view parent, std::string_view child) {
    std::string out(parent);
    if (out.empty() || out == "/") return "/" + std::string(child);
    if (out.back() != '}') out += '/';
    out += child;
    return out;
}

std::string_view primPart(std::string_view path) {
    int depth = 0;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '{') ++depth;
        else if (path[i] == '}') --depth;
        else if (path[i] == '.' && depth == 0) return path.substr(0, i);
    }
    return path;
}

std::string stripVariants(std::string_view path) {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '{') {
            const size_t close = path.find('}', i);
            if (close == std::string_view::npos) break;
            i = close;
            if (i + 1 < path.size() && path[i + 1] != '.' && path[i + 1] != '{' && path[i + 1] != '/') out += '/';
            continue;
        }
        out += path[i];
    }
    return out.empty() ? "/" : out;
}

std::string absolutePath(std::string_view path, std::string_view anchor) {
    if (!path.empty() && path[0] == '/') return std::string(path);
    std::string property;
    std::string_view prims = path;
    // A property of the anchor: ".prop"; else "a/b.prop".
    if (const size_t dot = path.rfind('.'); dot != std::string_view::npos && dot + 1 < path.size() &&
                                            path[dot + 1] != '.' && path[dot + 1] != '/' &&
                                            (dot == 0 || path[dot - 1] != '.')) {
        property = std::string(path.substr(dot));
        prims = path.substr(0, dot);
    }
    std::vector<std::string> parts;
    std::vector<PathElement> base;
    if (splitPath(stripVariants(anchor), base)) {
        for (const PathElement& e : base) {
            if (e.kind == PathElement::Kind::Prim) parts.push_back(e.name);
        }
    }
    size_t i = 0;
    while (i <= prims.size()) {
        size_t end = prims.find('/', i);
        if (end == std::string_view::npos) end = prims.size();
        const std::string_view part = prims.substr(i, end - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        i = end + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    if (out.empty()) out = "/";
    return out + property;
}

// --- Files -----------------------------------------------------------------------------------

bool readLayerBytes(std::span<const uint8_t> bytes, Layer& out, std::string& error) {
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "PXR-USDC", 8) == 0) return readCrate(bytes, out, error);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.rfind("#usda", 0) == 0) return parseText(text, out, error);
    if (bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4) {
        error = "a package (.usdz): read it by its name";
        return false;
    }
    error = "not a USD layer: neither text (#usda) nor a crate (PXR-USDC)";
    return false;
}

namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
        if (a != suffix[i]) return false;
    }
    return true;
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        error = "cannot read " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

struct ZipEntry {
    std::string name;
    size_t offset = 0, size = 0;
};

/// The files of a zip as USD writes a package: each stored as it is (no
/// compression), one after the other from the start -- USD reads the local
/// headers in order, not the directory at the end.
bool zipEntries(std::span<const uint8_t> zip, std::vector<ZipEntry>& out, std::string& error) {
    out.clear();
    size_t at = 0;
    while (at + 30 <= zip.size() && le32(zip.data() + at) == 0x04034b50u) {
        const uint8_t* h = zip.data() + at;
        const uint16_t flags = le16(h + 6), method = le16(h + 8);
        uint64_t compressed = le32(h + 18), size = le32(h + 22);
        const uint16_t nameLength = le16(h + 26), extraLength = le16(h + 28);
        const size_t data = at + 30 + nameLength + extraLength;
        if (data > zip.size()) break;
        std::string name(reinterpret_cast<const char*>(h + 30), nameLength);
        // Zip64: the sizes in the extra field.
        if (compressed == 0xffffffffu || size == 0xffffffffu) {
            const uint8_t* e = h + 30 + nameLength;
            for (size_t k = 0; k + 4 <= extraLength;) {
                const uint16_t id = le16(e + k), length = le16(e + k + 2);
                if (id == 1 && length >= 16 && k + 4 + 16 <= extraLength) {
                    size = le32(e + k + 4) | static_cast<uint64_t>(le32(e + k + 8)) << 32;
                    compressed = le32(e + k + 12) | static_cast<uint64_t>(le32(e + k + 16)) << 32;
                }
                k += 4u + length;
            }
        }
        if (method != 0 || compressed != size) {
            error = "the package's " + name + " is compressed: USD reads packages stored as they are";
            return false;
        }
        if (flags & 8u) {
            error = "the package's " + name + " has its size after its data: not a package USD reads";
            return false;
        }
        if (data + size > zip.size()) {
            error = "the package ends inside " + name;
            return false;
        }
        out.push_back({std::move(name), data, static_cast<size_t>(size)});
        at = data + static_cast<size_t>(size);
    }
    if (out.empty()) {
        error = "not a package: no files in it";
        return false;
    }
    return true;
}

/// "a.usdz[b/c.usda]" -> "a.usdz" and "b/c.usda"; false for a plain path.
bool splitPackage(std::string_view identifier, std::string& package, std::string& inner) {
    if (identifier.empty() || identifier.back() != ']') return false;
    const size_t open = identifier.find('[');
    if (open == std::string_view::npos) return false;
    package = std::string(identifier.substr(0, open));
    inner = std::string(identifier.substr(open + 1, identifier.size() - open - 2));
    return true;
}

std::string normalized(const std::string& path) {
    std::string out = std::filesystem::path(path).lexically_normal().generic_string();
    return out;
}

}  // namespace

bool packageFiles(const std::string& path, std::vector<std::string>& names, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes, error)) return false;
    std::vector<ZipEntry> entries;
    if (!zipEntries(bytes, entries, error)) return false;
    names.clear();
    for (const ZipEntry& e : entries) names.push_back(e.name);
    return true;
}

bool readFileBytes(const std::string& identifier, std::vector<uint8_t>& out, std::string& error) {
    std::string package, inner;
    const bool packaged = splitPackage(identifier, package, inner);
    if (!packaged && !endsWith(identifier, ".usdz")) return readWholeFile(identifier, out, error);
    if (!packaged) package = identifier;
    std::vector<uint8_t> zip;
    if (!readWholeFile(package, zip, error)) return false;
    std::vector<ZipEntry> entries;
    if (!zipEntries(zip, entries, error)) {
        error = package + ": " + error;
        return false;
    }
    const ZipEntry* found = packaged ? nullptr : &entries.front();
    for (const ZipEntry& e : entries) {
        if (packaged && normalized(e.name) == normalized(inner)) found = &e;
    }
    if (!found) {
        error = "no " + inner + " in " + package;
        return false;
    }
    out.assign(zip.begin() + static_cast<std::ptrdiff_t>(found->offset),
               zip.begin() + static_cast<std::ptrdiff_t>(found->offset + found->size));
    return true;
}

std::shared_ptr<Layer> readLayer(const std::string& identifier, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readFileBytes(identifier, bytes, error)) return nullptr;
    auto layer = std::make_shared<Layer>();
    layer->identifier = identifier;
    // A package names its layer by the file in it: what it refers to is
    // found beside that file.
    if (endsWith(identifier, ".usdz")) {
        std::vector<std::string> names;
        if (packageFiles(identifier, names, error)) layer->identifier = identifier + "[" + names.front() + "]";
    }
    std::string why;
    if (!readLayerBytes(bytes, *layer, why)) {
        error = identifier + ": " + why;
        return nullptr;
    }
    return layer;
}

std::string resolveAsset(std::string_view asset, std::string_view anchorIdentifier) {
    if (asset.empty()) return {};
    std::string package, inner;
    const bool absolute = asset[0] == '/' || asset[0] == '\\' || (asset.size() > 2 && asset[1] == ':');
    if (splitPackage(anchorIdentifier, package, inner) && !absolute) {
        const std::string folder = std::filesystem::path(inner).parent_path().generic_string();
        const std::string joined = folder.empty() ? std::string(asset) : folder + "/" + std::string(asset);
        return package + "[" + normalized(joined) + "]";
    }
    if (absolute) return normalized(std::string(asset));
    const std::string folder = std::filesystem::path(std::string(anchorIdentifier)).parent_path().generic_string();
    return normalized(folder.empty() ? std::string(asset) : folder + "/" + std::string(asset));
}

}  // namespace pg::usd
