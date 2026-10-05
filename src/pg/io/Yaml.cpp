#include "pg/io/Yaml.h"

#include <cstring>

namespace pg::io {
namespace {

bool space(char c) { return c == ' ' || c == '\t'; }

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (space(s.front()) || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (space(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

/// A line without its comment: from a '#' at its start or after a space,
/// not in quotes.
std::string_view uncommented(std::string_view s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            // A quote opens a scalar where one can begin.
            if (i == 0 || space(s[i - 1]) || std::strchr("[{,:-", s[i - 1])) quote = c;
            continue;
        }
        if (c == '#' && (i == 0 || space(s[i - 1]))) return s.substr(0, i);
    }
    return s;
}

/// How deep in brackets the end of `s` is, quotes left out.
int depthAfter(std::string_view s, int depth) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return depth;
}

class Parser {
public:
    explicit Parser(std::string_view text) {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            lines_.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
    }

    bool document(YamlNode& out) {
        skipBlank();
        // A document marker or a directive at the start.
        while (at_ < lines_.size()) {
            const std::string_view t = trimmed(lines_[at_]);
            if (t.rfind("---", 0) == 0 || t.rfind("%", 0) == 0) {
                ++at_;
                skipBlank();
            } else {
                break;
            }
        }
        if (!block(-1, out)) return false;
        skipBlank();
        if (at_ < lines_.size()) return fail("not read: '" + std::string(trimmed(lines_[at_])) + "'");
        return true;
    }

    std::string error;

private:
    std::vector<std::string> lines_;
    size_t at_ = 0;

    bool fail(const std::string& why) {
        if (error.empty()) error = "line " + std::to_string(at_ + 1) + ": " + why;
        return false;
    }

    /// The line's indentation and its content, its comment taken off.
    static void content(const std::string& line, int& indent, std::string_view& text) {
        indent = 0;
        while (static_cast<size_t>(indent) < line.size() && line[static_cast<size_t>(indent)] == ' ') ++indent;
        text = trimmed(uncommented(std::string_view(line).substr(static_cast<size_t>(indent))));
    }

    void skipBlank() {
        while (at_ < lines_.size()) {
            int indent = 0;
            std::string_view text;
            content(lines_[at_], indent, text);
            if (!text.empty()) return;
            ++at_;
        }
    }

    /// The next line with something on it: false at the end.
    bool peek(int& indent, std::string_view& text) {
        skipBlank();
        if (at_ >= lines_.size()) return false;
        content(lines_[at_], indent, text);
        return true;
    }

    static bool isItem(std::string_view text) { return !text.empty() && text[0] == '-' && (text.size() == 1 || space(text[1])); }

    /// Splits "key: value" -- the key plain or quoted. False where the line is not one.
    static bool splitKey(std::string_view text, std::string& key, std::string_view& rest) {
        if (text.empty() || text[0] == '-' || text[0] == '[' || text[0] == '{' || text[0] == '!' || text[0] == '|' ||
            text[0] == '>') {
            return false;
        }
        size_t i = 0;
        if (text[0] == '"' || text[0] == '\'') {
            const char q = text[0];
            std::string k;
            for (i = 1; i < text.size(); ++i) {
                if (text[i] == q) {
                    if (q != '\'' || i + 1 >= text.size() || text[i + 1] != '\'') break;
                    ++i;
                } else if (q == '"' && text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                }
                k += text[i];
            }
            if (i >= text.size()) return false;
            ++i;
            while (i < text.size() && space(text[i])) ++i;
            if (i >= text.size() || text[i] != ':') return false;
            key = k;
        } else {
            for (i = 0; i < text.size(); ++i) {
                if (text[i] == ':' && (i + 1 == text.size() || space(text[i + 1]))) break;
            }
            if (i >= text.size()) return false;
            key = std::string(trimmed(text.substr(0, i)));
        }
        rest = trimmed(text.substr(i + 1));
        return true;
    }

    /// The node on the lines after the current one, indented more than
    /// `parent` -- or, a sequence, as much (YAML lets a sequence under a key
    /// start where the key does). Null where there is none.
    bool block(int parent, YamlNode& out, bool sameIndentSequence = false) {
        int indent = 0;
        std::string_view text;
        if (!peek(indent, text)) return true;
        if (indent < parent || (indent == parent && !(sameIndentSequence && isItem(text)))) return true;
        if (isItem(text)) return sequence(indent, out);
        std::string key;
        std::string_view rest;
        if (splitKey(text, key, rest)) return mapping(indent, out);
        // A scalar or a flow node on its own lines.
        std::string whole(text);
        ++at_;
        if (!gather(whole)) return false;
        return value(whole, out);
    }

    /// More lines onto a flow node that does not end on its first.
    bool gather(std::string& text) {
        int depth = depthAfter(text, 0);
        while (depth > 0) {
            if (at_ >= lines_.size()) return fail("a [ or { not closed");
            int indent = 0;
            std::string_view more;
            content(lines_[at_], indent, more);
            ++at_;
            text += ' ';
            text += more;
            depth = depthAfter(more, depth);
        }
        return true;
    }

    bool sequence(int indent, YamlNode& out) {
        out.kind = YamlNode::Kind::Sequence;
        for (;;) {
            int at = 0;
            std::string_view text;
            if (!peek(at, text) || at != indent || !isItem(text)) return true;
            std::string_view rest = trimmed(text.substr(1));
            out.items.emplace_back();
            YamlNode& item = out.items.back();
            // Where the rest begins: a mapping started on the dash's line
            // goes on at that column.
            const int column = indent + static_cast<int>(text.size() - rest.size());
            if (rest.empty()) {
                ++at_;
                if (!block(indent, item)) return false;
                continue;
            }
            std::string tag;
            if (!takeTag(rest, tag)) return false;
            if (!tag.empty() && rest.empty()) {
                ++at_;
                if (!block(indent, item)) return false;
                item.tag = tag;
                continue;
            }
            std::string key;
            std::string_view after;
            if (tag.empty() && splitKey(rest, key, after)) {
                // "- key: value": the line read again as a mapping's first.
                lines_[at_] = std::string(static_cast<size_t>(column), ' ') + std::string(rest);
                if (!mapping(column, item)) return false;
                continue;
            }
            ++at_;
            std::string whole(rest);
            if (!gather(whole)) return false;
            if (!value(whole, item)) return false;
            if (!tag.empty()) item.tag = tag;
        }
    }

    bool mapping(int indent, YamlNode& out) {
        out.kind = YamlNode::Kind::Mapping;
        for (;;) {
            int at = 0;
            std::string_view text;
            if (!peek(at, text) || at != indent) return true;
            std::string key;
            std::string_view rest;
            if (!splitKey(text, key, rest)) {
                if (isItem(text)) return true;  // a sequence after: the caller's
                return fail("not a key: '" + std::string(text) + "'");
            }
            ++at_;
            out.pairs.emplace_back(key, YamlNode{});
            YamlNode& v = out.pairs.back().second;
            std::string tag;
            if (!takeTag(rest, tag)) return false;
            if (rest.empty()) {
                if (!block(indent, v, true)) return false;
            } else if (rest[0] == '|' || rest[0] == '>') {
                blockScalar(indent, rest[0] == '>', rest.find('-') != std::string_view::npos, v);
            } else {
                std::string whole(rest);
                if (!gather(whole)) return false;
                if (!value(whole, v)) return false;
            }
            if (!tag.empty()) v.tag = tag;
        }
    }

    /// The lines of a | or > block: those indented more than the key.
    void blockScalar(int indent, bool folded, bool strip, YamlNode& out) {
        out.kind = YamlNode::Kind::Scalar;
        int inner = -1;
        std::vector<std::string> body;
        while (at_ < lines_.size()) {
            const std::string& line = lines_[at_];
            int i = 0;
            while (static_cast<size_t>(i) < line.size() && line[static_cast<size_t>(i)] == ' ') ++i;
            const bool blank = trimmed(line).empty();
            if (!blank && i <= indent) break;
            if (!blank && inner < 0) inner = i;
            body.push_back(blank ? std::string() : line.substr(static_cast<size_t>(std::min(i, inner < 0 ? i : inner))));
            ++at_;
        }
        while (!body.empty() && body.back().empty()) body.pop_back();
        std::string s;
        for (size_t k = 0; k < body.size(); ++k) {
            if (k) s += folded && !body[k].empty() && !body[k - 1].empty() ? ' ' : '\n';
            s += body[k];
        }
        if (!strip && !s.empty()) s += '\n';
        out.scalar = s;
    }

    /// A tag at the start of `s` -- !<Name> or !Name -- taken off it.
    bool takeTag(std::string_view& s, std::string& tag) {
        tag.clear();
        if (s.empty() || s[0] != '!') return true;
        size_t end = 0;
        if (s.size() > 1 && s[1] == '<') {
            end = s.find('>');
            if (end == std::string_view::npos) return fail("a tag not closed: " + std::string(s));
            tag = std::string(s.substr(2, end - 2));
            ++end;
        } else {
            end = 1;
            while (end < s.size() && !space(s[end]) && s[end] != '{' && s[end] != '[') ++end;
            tag = std::string(s.substr(1, end - 1));
        }
        s = trimmed(s.substr(end));
        return true;
    }

    /// A node written on one line: flow, quoted or plain.
    bool value(std::string_view text, YamlNode& out) {
        text = trimmed(text);
        if (text.empty()) return true;
        if (text[0] == '[' || text[0] == '{' || text[0] == '"' || text[0] == '\'' || text[0] == '!') {
            size_t i = 0;
            if (!flow(text, i, out, true)) return false;
            while (i < text.size() && space(text[i])) ++i;
            if (i < text.size()) return fail("more after a value: '" + std::string(text.substr(i)) + "'");
            return true;
        }
        plain(std::string(text), out);
        return true;
    }

    static void plain(const std::string& s, YamlNode& out) {
        if (s == "~" || s == "null" || s.empty()) {
            out.kind = YamlNode::Kind::Null;
            return;
        }
        out.kind = YamlNode::Kind::Scalar;
        out.scalar = s;
    }

    bool quoted(std::string_view s, size_t& i, std::string& out) {
        const char q = s[i++];
        out.clear();
        while (i < s.size()) {
            if (s[i] == q) {
                // '' in single quotes is one: the scalar goes on.
                if (q != '\'' || i + 1 >= s.size() || s[i + 1] != '\'') break;
                out += '\'';
                i += 2;
                continue;
            }
            if (q == '"' && s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                switch (s[i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    default: out += s[i]; break;
                }
                ++i;
                continue;
            }
            out += s[i++];
        }
        if (i >= s.size()) return fail("a quote not closed");
        ++i;
        return true;
    }

    /// A node in flow, from s[i]; `top`, it may run to the end of the text.
    bool flow(std::string_view s, size_t& i, YamlNode& out, bool top = false) {
        auto skip = [&] {
            while (i < s.size() && space(s[i])) ++i;
        };
        skip();
        if (i >= s.size()) return true;
        if (s[i] == '!') {
            std::string_view rest = s.substr(i);
            std::string tag;
            if (!takeTag(rest, tag)) return false;
            // What follows the tag, where it is in `s`.
            i = rest.empty() ? s.size() : static_cast<size_t>(rest.data() - s.data());
            if (!flow(s, i, out, top)) return false;
            out.tag = tag;
            return true;
        }
        if (s[i] == '[') {
            ++i;
            out.kind = YamlNode::Kind::Sequence;
            for (;;) {
                skip();
                if (i >= s.size()) return fail("a [ not closed");
                if (s[i] == ']') {
                    ++i;
                    return true;
                }
                out.items.emplace_back();
                if (!flow(s, i, out.items.back())) return false;
                skip();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("a sequence's items go apart by commas");
            }
        }
        if (s[i] == '{') {
            ++i;
            out.kind = YamlNode::Kind::Mapping;
            for (;;) {
                skip();
                if (i >= s.size()) return fail("a { not closed");
                if (s[i] == '}') {
                    ++i;
                    return true;
                }
                std::string key;
                if (s[i] == '"' || s[i] == '\'') {
                    if (!quoted(s, i, key)) return false;
                } else {
                    const size_t start = i;
                    while (i < s.size() && !(s[i] == ':' && (i + 1 == s.size() || space(s[i + 1]) || s[i + 1] == ',' ||
                                                             s[i + 1] == '}')) &&
                           s[i] != ',' && s[i] != '}') {
                        ++i;
                    }
                    key = std::string(trimmed(s.substr(start, i - start)));
                }
                skip();
                out.pairs.emplace_back(key, YamlNode{});
                if (i < s.size() && s[i] == ':') {
                    ++i;
                    skip();
                    if (i < s.size() && s[i] != ',' && s[i] != '}') {
                        if (!flow(s, i, out.pairs.back().second)) return false;
                    }
                }
                skip();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("a mapping's pairs go apart by commas");
            }
        }
        if (s[i] == '"' || s[i] == '\'') {
            out.kind = YamlNode::Kind::Scalar;
            return quoted(s, i, out.scalar);
        }
        // Plain: to the end of the text at the top, else to a flow indicator.
        const size_t start = i;
        if (top) {
            i = s.size();
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}') ++i;
        }
        plain(std::string(trimmed(s.substr(start, i - start))), out);
        return true;
    }
};

}  // namespace

const YamlNode* YamlNode::find(std::string_view key) const {
    if (kind != Kind::Mapping) return nullptr;
    for (const auto& [k, v] : pairs) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string YamlNode::text(std::string_view key, const std::string& fallback) const {
    const YamlNode* v = find(key);
    return v && v->isScalar() ? v->scalar : fallback;
}

bool parseYaml(std::string_view text, YamlNode& out, std::string& error) {
    out = YamlNode{};
    Parser p(text);
    if (!p.document(out)) {
        error = p.error;
        return false;
    }
    return true;
}

}  // namespace pg::io
