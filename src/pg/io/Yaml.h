#pragma once
//
// YAML, as far as OpenColorIO's configs write it: mappings and sequences in
// blocks, by their indentation, or in flow -- {a: 1, b: [2, 3]} -- across
// lines; tags before a node -- !<ColorSpace> -- that say what it is;
// scalars plain, 'single' or "double" quoted, or as blocks of lines (| and
// >); comments. Anchors, aliases and several documents in one file are not
// read.
//
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pg::io {

struct YamlNode {
    enum class Kind { Null, Scalar, Sequence, Mapping };
    Kind kind = Kind::Null;
    std::string tag;     ///< "ColorSpace" of !<ColorSpace>; "" for none
    std::string scalar;  ///< a scalar's text, quotes and escapes undone
    std::vector<YamlNode> items;                         ///< a sequence's
    std::vector<std::pair<std::string, YamlNode>> pairs;  ///< a mapping's, in order

    bool isNull() const { return kind == Kind::Null; }
    bool isScalar() const { return kind == Kind::Scalar; }
    bool isSequence() const { return kind == Kind::Sequence; }
    bool isMapping() const { return kind == Kind::Mapping; }
    /// A mapping's value for `key`; null for none (or not a mapping).
    const YamlNode* find(std::string_view key) const;
    /// The scalar of `key`, or `fallback`.
    std::string text(std::string_view key, const std::string& fallback = "") const;
};

/// `text` as a node: what its top level is -- a mapping, mostly. False,
/// with where and why, for what it cannot read.
bool parseYaml(std::string_view text, YamlNode& out, std::string& error);

}  // namespace pg::io
