// .usda: USD's text form, read by a scanner and a descent through its
// grammar -- the layer's metadata, then prims, each with its metadata, its
// properties, its children and its variant sets.
#include "pg/usd/Layer.h"

#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <sstream>

namespace pg::usd {

namespace {

// --- Numbers ---------------------------------------------------------------------------------

/// A number as C writes it, whatever the locale: exact where the digits and
/// the exponent are few enough for one rounding (Clinger's fast path),
/// else by the stream in the classic locale.
bool toNumber(std::string_view text, double& out) {
    static const double kPowers[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                     1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false, exact = true;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        any = true;
        if (mantissa == 0 && text[i] == '0') continue;
        if (digits < 19) {
            mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
            ++digits;
        } else {
            ++exponent;
            exact = false;
        }
    }
    if (i < text.size() && text[i] == '.') {
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            any = true;
            if (mantissa == 0 && text[i] == '0') {
                --exponent;
                continue;
            }
            if (digits < 19) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(text[i] - '0');
                ++digits;
                --exponent;
            } else if (text[i] != '0') {
                exact = false;
            }
        }
    }
    if (!any) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool negativeExponent = false;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) negativeExponent = text[i++] == '-';
        int e = 0;
        bool eDigits = false;
        for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
            e = std::min(e * 10 + (text[i] - '0'), 100000);
            eDigits = true;
        }
        if (!eDigits) return false;
        exponent += negativeExponent ? -e : e;
    }
    if (i != text.size()) return false;
    if (mantissa == 0) {
        out = negative ? -0.0 : 0.0;
        return true;
    }
    if (exact && mantissa <= (uint64_t{1} << 53) && exponent >= -22 && exponent <= 22) {
        double v = static_cast<double>(mantissa);
        v = exponent >= 0 ? v * kPowers[exponent] : v / kPowers[-exponent];
        out = negative ? -v : v;
        return true;
    }
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    double v = 0.0;
    in >> v;
    if (in.fail()) {
        // Out of range: the stream gives up where C would give infinity or 0.
        v = exponent > 0 ? std::numeric_limits<double>::infinity() : 0.0;
        if (negative) v = -v;
    }
    out = v;
    return true;
}

// --- Scanner ---------------------------------------------------------------------------------

enum class Tok : uint8_t { End, Word, Number, String, Asset, Path, Punct };

struct Token {
    Tok kind = Tok::End;
    std::string_view text;  ///< a word, a number, punctuation; a string's, asset's, path's text (escapes: `value`)
    std::string value;      ///< a string with its escapes undone
    double number = 0.0;
    int line = 1;
};

class Scanner {
public:
    explicit Scanner(std::string_view text) : s_(text) {
        // The first line is the header: #usda 1.0
        const size_t eol = s_.find('\n');
        pos_ = eol == std::string_view::npos ? s_.size() : eol;
    }

    const Token& peek() {
        if (!peeked_) {
            next_ = scan();
            peeked_ = true;
        }
        return next_;
    }
    Token take() {
        peek();
        peeked_ = false;
        return std::move(next_);
    }
    bool is(char punct) {
        const Token& t = peek();
        return t.kind == Tok::Punct && t.text.size() == 1 && t.text[0] == punct;
    }
    bool isWord(std::string_view w) {
        const Token& t = peek();
        return t.kind == Tok::Word && t.text == w;
    }
    bool accept(char punct) {
        if (!is(punct)) return false;
        take();
        return true;
    }
    int line() { return peek().line; }
    std::string error;
    bool failed() const { return !error.empty(); }
    void fail(const std::string& why, int line) {
        if (error.empty()) error = "line " + std::to_string(line) + ": " + why;
    }

private:
    static bool wordStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool wordChar(char c) { return wordStart(c) || (c >= '0' && c <= '9') || c == ':'; }

    Token scan() {
        Token t;
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
                if (s_[pos_] == '\n') ++line_;
                ++pos_;
            }
            if (pos_ < s_.size() && s_[pos_] == '#') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            if (pos_ + 1 < s_.size() && s_[pos_] == '/' && s_[pos_ + 1] == '*') {
                const size_t end = s_.find("*/", pos_ + 2);
                const size_t stop = end == std::string_view::npos ? s_.size() : end + 2;
                for (size_t k = pos_; k < stop; ++k) line_ += s_[k] == '\n';
                pos_ = stop;
                continue;
            }
            break;
        }
        t.line = line_;
        if (pos_ >= s_.size()) return t;
        const char c = s_[pos_];
        const size_t start = pos_;
        if (wordStart(c) || ((c == '-' || c == '+') && pos_ + 1 < s_.size() && wordStart(s_[pos_ + 1]))) {
            ++pos_;
            while (pos_ < s_.size() && wordChar(s_[pos_])) ++pos_;
            t.kind = Tok::Word;
            t.text = s_.substr(start, pos_ - start);
            return t;
        }
        if ((c >= '0' && c <= '9') || ((c == '-' || c == '+' || c == '.') && pos_ + 1 < s_.size() &&
                                       ((s_[pos_ + 1] >= '0' && s_[pos_ + 1] <= '9') || s_[pos_ + 1] == '.'))) {
            ++pos_;
            while (pos_ < s_.size()) {
                const char d = s_[pos_];
                if ((d >= '0' && d <= '9') || d == '.') {
                    ++pos_;
                } else if ((d == 'e' || d == 'E') && pos_ + 1 < s_.size()) {
                    ++pos_;
                    if (s_[pos_] == '-' || s_[pos_] == '+') ++pos_;
                } else {
                    break;
                }
            }
            t.kind = Tok::Number;
            t.text = s_.substr(start, pos_ - start);
            if (!toNumber(t.text, t.number)) fail("not a number: " + std::string(t.text), t.line);
            return t;
        }
        if (c == '"' || c == '\'') {
            t.kind = Tok::String;
            const bool triple = pos_ + 2 < s_.size() && s_[pos_ + 1] == c && s_[pos_ + 2] == c;
            pos_ += triple ? 3 : 1;
            for (;;) {
                if (pos_ >= s_.size()) {
                    fail("a string that does not end", t.line);
                    break;
                }
                const char d = s_[pos_];
                if (d == '\\' && pos_ + 1 < s_.size()) {
                    const char e = s_[pos_ + 1];
                    pos_ += 2;
                    switch (e) {
                        case 'n': t.value += '\n'; break;
                        case 't': t.value += '\t'; break;
                        case 'r': t.value += '\r'; break;
                        case '0': t.value += '\0'; break;
                        case 'x': {
                            int v = 0, n = 0;
                            while (n < 2 && pos_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[pos_]))) {
                                const char h = s_[pos_++];
                                v = v * 16 + (h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                                ++n;
                            }
                            t.value += static_cast<char>(v);
                            break;
                        }
                        case '\n': ++line_; break;
                        default: t.value += e;
                    }
                    continue;
                }
                if (triple ? (d == c && pos_ + 2 < s_.size() && s_[pos_ + 1] == c && s_[pos_ + 2] == c) : d == c) {
                    pos_ += triple ? 3 : 1;
                    break;
                }
                if (d == '\n') {
                    if (!triple) {
                        fail("a line break in a string", t.line);
                        break;
                    }
                    ++line_;
                }
                t.value += d;
                ++pos_;
            }
            return t;
        }
        if (c == '@') {
            t.kind = Tok::Asset;
            if (s_.substr(pos_, 3) == "@@@") {
                pos_ += 3;
                for (;;) {
                    if (pos_ >= s_.size()) {
                        fail("an asset path that does not end", t.line);
                        break;
                    }
                    if (s_[pos_] == '\\' && s_.substr(pos_ + 1, 3) == "@@@") {
                        t.value += "@@@";
                        pos_ += 4;
                        continue;
                    }
                    if (s_.substr(pos_, 3) == "@@@") {
                        pos_ += 3;
                        break;
                    }
                    t.value += s_[pos_++];
                }
            } else {
                const size_t end = s_.find('@', pos_ + 1);
                if (end == std::string_view::npos) {
                    fail("an asset path that does not end", t.line);
                    pos_ = s_.size();
                } else {
                    t.value = std::string(s_.substr(pos_ + 1, end - pos_ - 1));
                    pos_ = end + 1;
                }
            }
            return t;
        }
        if (c == '<') {
            t.kind = Tok::Path;
            const size_t end = s_.find('>', pos_ + 1);
            if (end == std::string_view::npos) {
                fail("a path that does not end", t.line);
                pos_ = s_.size();
            } else {
                t.value = std::string(s_.substr(pos_ + 1, end - pos_ - 1));
                pos_ = end + 1;
            }
            return t;
        }
        t.kind = Tok::Punct;
        t.text = s_.substr(pos_, 1);
        ++pos_;
        return t;
    }

    std::string_view s_;
    size_t pos_ = 0;
    int line_ = 1;
    Token next_;
    bool peeked_ = false;
};

// --- Values as written -------------------------------------------------------------------

/// A value as the text has it, before the type it is read as.
struct Parsed {
    enum class Kind : uint8_t { None, Number, String, Asset, Path, Word, Tuple, List, Dictionary, Reference };
    Kind kind = Kind::None;
    double number = 0.0;
    std::string text;   ///< a string, an asset, a path, a word; a reference's asset
    std::string path;   ///< a reference's prim
    double offset = 0.0, scale = 1.0;
    std::vector<Parsed> items;
    Dictionary dictionary;
};

class Parser {
public:
    Parser(std::string_view text, Layer& layer) : in_(text), layer_(layer) {}

    bool run(std::string& error) {
        if (in_.accept('(')) layerMetadata();
        while (!in_.failed() && in_.peek().kind != Tok::End) {
            prim(layer_.root, "/");
        }
        if (in_.failed()) {
            error = in_.error;
            return false;
        }
        return true;
    }

private:
    bool expect(char punct, const std::string& what) {
        if (in_.accept(punct)) return true;
        in_.fail(std::string("expected '") + punct + "' " + what + describeNext(), in_.line());
        return false;
    }

    std::string describeNext() {
        const Token& t = in_.peek();
        switch (t.kind) {
            case Tok::End: return ", found the end";
            case Tok::String: return ", found \"" + t.value + "\"";
            case Tok::Asset: return ", found @" + t.value + "@";
            case Tok::Path: return ", found <" + t.value + ">";
            default: return ", found '" + std::string(t.text) + "'";
        }
    }

    /// Skips a balanced (), [] or {} whose opening bracket is next.
    void skipBalanced() {
        int depth = 0;
        do {
            const Token t = in_.take();
            if (t.kind == Tok::End) {
                in_.fail("brackets that do not close", t.line);
                return;
            }
            if (t.kind == Tok::Punct) {
                const char c = t.text[0];
                if (c == '(' || c == '[' || c == '{') ++depth;
                if (c == ')' || c == ']' || c == '}') --depth;
            }
        } while (depth > 0 && !in_.failed());
    }

    // A value of any form.
    Parsed value() {
        Parsed p;
        struct Depth {
            int& d;
            explicit Depth(int& x) : d(++x) {}
            ~Depth() { --d; }
        } depth(depth_);
        if (depth_ > 256) {
            in_.fail("values nested too deep", in_.line());
            return p;
        }
        const Token& t = in_.peek();
        switch (t.kind) {
            case Tok::Number:
                p.kind = Parsed::Kind::Number;
                p.number = in_.take().number;
                return p;
            case Tok::String:
                p.kind = Parsed::Kind::String;
                p.text = in_.take().value;
                return p;
            case Tok::Asset: {
                p.kind = Parsed::Kind::Asset;
                p.text = in_.take().value;
                return p;
            }
            case Tok::Path:
                p.kind = Parsed::Kind::Path;
                p.text = in_.take().value;
                return p;
            case Tok::Word: {
                const Token w = in_.take();
                if (w.text == "None") return p;
                p.kind = Parsed::Kind::Word;
                p.text = std::string(w.text);
                if (w.text == "inf" || w.text == "+inf") {
                    p.kind = Parsed::Kind::Number;
                    p.number = std::numeric_limits<double>::infinity();
                } else if (w.text == "-inf") {
                    p.kind = Parsed::Kind::Number;
                    p.number = -std::numeric_limits<double>::infinity();
                } else if (w.text == "nan" || w.text == "-nan") {
                    p.kind = Parsed::Kind::Number;
                    p.number = std::numeric_limits<double>::quiet_NaN();
                }
                return p;
            }
            case Tok::Punct:
                if (in_.is('(')) {
                    in_.take();
                    p.kind = Parsed::Kind::Tuple;
                    while (!in_.failed() && !in_.is(')')) {
                        p.items.push_back(value());
                        if (!in_.accept(',')) break;
                    }
                    expect(')', "to end a tuple");
                    return p;
                }
                if (in_.is('[')) {
                    in_.take();
                    p.kind = Parsed::Kind::List;
                    while (!in_.failed() && !in_.is(']')) {
                        p.items.push_back(listItem());
                        if (!in_.accept(',')) break;
                    }
                    expect(']', "to end a list");
                    return p;
                }
                if (in_.is('{')) {
                    p.kind = Parsed::Kind::Dictionary;
                    p.dictionary = dictionary();
                    return p;
                }
                break;
            default: break;
        }
        in_.fail("expected a value" + describeNext(), t.line);
        in_.take();
        return p;
    }

    /// An item of a list: a value, or a reference -- an asset, a prim, or
    /// both -- with a layer offset in parentheses after it.
    Parsed listItem() {
        Parsed p = value();
        if (p.kind == Parsed::Kind::Asset || p.kind == Parsed::Kind::Path) referenceTail(p);
        return p;
    }

    /// A value that may be a reference, alone or in a list.
    Parsed referenceValue() {
        Parsed p = value();
        if (p.kind == Parsed::Kind::Asset || p.kind == Parsed::Kind::Path) referenceTail(p);
        return p;
    }

    void referenceTail(Parsed& p) {
        if (p.kind == Parsed::Kind::Asset && in_.peek().kind == Tok::Path) {
            p.path = in_.take().value;
            p.kind = Parsed::Kind::Reference;
        }
        if (!in_.is('(')) return;
        if (p.kind == Parsed::Kind::Path) {
            p.path = p.text;
            p.text.clear();
        }
        p.kind = Parsed::Kind::Reference;
        in_.take();
        while (!in_.failed() && !in_.is(')')) {
            const Token key = in_.take();
            if (key.kind != Tok::Word) {
                in_.fail("expected offset, scale or customData", key.line);
                return;
            }
            if (!expect('=', "after " + std::string(key.text))) return;
            const Parsed v = value();
            if (key.text == "offset") p.offset = v.number;
            else if (key.text == "scale") p.scale = v.number;
            in_.accept(';');
        }
        expect(')', "to end a reference's offset");
    }

    Dictionary dictionary() {
        Dictionary d;
        expect('{', "to start a dictionary");
        while (!in_.failed() && !in_.is('}')) {
            const Token type = in_.take();
            if (type.kind != Tok::Word) {
                in_.fail("expected a type in a dictionary" + describeNext(), type.line);
                break;
            }
            std::string typeName(type.text);
            if (in_.is('[')) {
                in_.take();
                expect(']', "after [");
                typeName += "[]";
            }
            const Token key = in_.take();
            if (key.kind != Tok::Word && key.kind != Tok::String) {
                in_.fail("expected a key in a dictionary", key.line);
                break;
            }
            const std::string name = key.kind == Tok::String ? key.value : std::string(key.text);
            if (!expect('=', "after a dictionary's key")) break;
            if (typeName == "dictionary") {
                d.emplace_back(name, Value::makeDictionary(dictionary()));
            } else {
                d.emplace_back(name, convert(value(), typeName));
            }
            in_.accept(';');
            in_.accept(',');
        }
        expect('}', "to end a dictionary");
        return d;
    }

    // --- Values as their types -----------------------------------------------------------

    /// The numbers of `p` -- a number, a word true or false, a tuple, tuples
    /// of tuples (matrices) -- in order.
    static void flatten(const Parsed& p, std::vector<double>& out) {
        switch (p.kind) {
            case Parsed::Kind::Number: out.push_back(p.number); break;
            case Parsed::Kind::Word: out.push_back(p.text == "true" ? 1.0 : 0.0); break;
            case Parsed::Kind::Tuple:
            case Parsed::Kind::List:
                for (const Parsed& i : p.items) flatten(i, out);
                break;
            default: break;
        }
    }

    /// `p` read as a value of `typeName` ("float3[]", "token", ...).
    Value convert(const Parsed& p, const std::string& typeName) {
        if (p.kind == Parsed::Kind::None) return Value::makeBlocked();
        std::string type = typeName;
        bool array = false;
        if (type.size() > 2 && type.substr(type.size() - 2) == "[]") {
            type.resize(type.size() - 2);
            array = true;
        }
        const int width = widthOf(type);
        if (width > 0) {
            std::vector<double> numbers;
            flatten(p, numbers);
            if (numbers.size() % static_cast<size_t>(width) != 0) {
                in_.fail("not a " + typeName + " value", in_.line());
                return {};
            }
            // Quaternions are written real part first: (w, x, y, z).
            if (type.rfind("quat", 0) == 0) {
                for (size_t k = 0; k + 3 < numbers.size(); k += 4) {
                    const double w = numbers[k];
                    numbers[k] = numbers[k + 1];
                    numbers[k + 1] = numbers[k + 2];
                    numbers[k + 2] = numbers[k + 3];
                    numbers[k + 3] = w;
                }
            }
            return Value::makeNumbers(type, width, std::move(numbers), array);
        }
        if (width == 0) {
            if (type == "dictionary" && p.kind == Parsed::Kind::Dictionary) return Value::makeDictionary(p.dictionary);
            std::vector<std::string> strings;
            if (p.kind == Parsed::Kind::List) {
                for (const Parsed& i : p.items) strings.push_back(i.text);
            } else {
                strings.push_back(p.text);
            }
            return Value::makeStrings(type, std::move(strings), array);
        }
        return generic(p);
    }

    /// `p` as what it looks like, for metadata of no known type.
    static Value generic(const Parsed& p) {
        switch (p.kind) {
            case Parsed::Kind::None: return Value::makeBlocked();
            case Parsed::Kind::Number: return Value::makeNumber("double", p.number);
            case Parsed::Kind::Word:
                if (p.text == "true" || p.text == "false") return Value::makeNumber("bool", p.text == "true" ? 1.0 : 0.0);
                return Value::makeString("token", p.text);
            case Parsed::Kind::String: return Value::makeString("string", p.text);
            case Parsed::Kind::Asset: return Value::makeString("asset", p.text);
            case Parsed::Kind::Path: return Value::makeString("path", p.text);
            case Parsed::Kind::Dictionary: return Value::makeDictionary(p.dictionary);
            case Parsed::Kind::Tuple: {
                std::vector<double> n;
                flatten(p, n);
                const int w = static_cast<int>(n.size());
                return Value::makeNumbers("double" + std::to_string(w), w, std::move(n), false);
            }
            case Parsed::Kind::List: {
                if (!p.items.empty() && p.items[0].kind != Parsed::Kind::Number && p.items[0].kind != Parsed::Kind::Tuple) {
                    std::vector<std::string> s;
                    for (const Parsed& i : p.items) s.push_back(i.text);
                    return Value::makeStrings(p.items[0].kind == Parsed::Kind::Asset ? "asset" : "string", std::move(s), true);
                }
                std::vector<double> n;
                const int w = p.items.empty() || p.items[0].kind != Parsed::Kind::Tuple
                                  ? 1
                                  : static_cast<int>(p.items[0].items.size());
                flatten(p, n);
                return Value::makeNumbers(w == 1 ? "double" : "double" + std::to_string(w), w, std::move(n), true);
            }
            case Parsed::Kind::Reference: return Value::makeString("asset", p.text);
        }
        return {};
    }

    /// The items of a list edit's value: one item, a list of them, or None.
    static std::vector<ListItem> itemsOf(const Parsed& p) {
        std::vector<ListItem> out;
        auto one = [&](const Parsed& i) {
            ListItem item;
            switch (i.kind) {
                case Parsed::Kind::Reference:
                    item.text = i.text;
                    item.path = i.path;
                    item.offset = i.offset;
                    item.scale = i.scale;
                    break;
                case Parsed::Kind::Asset: item.text = i.text; break;
                case Parsed::Kind::Path: item.path = i.text; break;
                case Parsed::Kind::Number: {
                    // As the crate has a number of a list edit: its text.
                    char text[32];
                    std::snprintf(text, sizeof text, "%.17g", i.number);
                    item.text = text;
                    break;
                }
                default: item.text = i.text;
            }
            out.push_back(std::move(item));
        };
        if (p.kind == Parsed::Kind::List) {
            for (const Parsed& i : p.items) one(i);
        } else if (p.kind != Parsed::Kind::None) {
            one(p);
        }
        return out;
    }

    /// Puts `items` in `op` where `how` ("", "prepend", "append", "add",
    /// "delete", "reorder") says; nothing before them makes them explicit.
    static void edit(ListOp& op, std::string_view how, std::vector<ListItem> items) {
        if (how.empty()) {
            op.isExplicit = true;
            op.explicitItems = std::move(items);
        } else if (how == "prepend") {
            op.prepended = std::move(items);
        } else if (how == "append") {
            op.appended = std::move(items);
        } else if (how == "add") {
            op.added = std::move(items);
        } else if (how == "delete") {
            op.deleted = std::move(items);
        } else if (how == "reorder") {
            op.ordered = std::move(items);
        }
    }

    /// Paths in list items: made absolute from the prim they are said on.
    static void anchorPaths(std::vector<ListItem>& items, const std::string& anchor, bool textIsPath) {
        for (ListItem& i : items) {
            if (textIsPath) {
                if (i.text.empty() && !i.path.empty()) {
                    i.text = i.path;
                    i.path.clear();
                }
                if (!i.text.empty()) i.text = absolutePath(i.text, anchor);
            } else if (!i.path.empty() && i.text.empty()) {
                // An internal reference.
                i.path = absolutePath(i.path, anchor);
            }
        }
    }

    static bool isListOpWord(std::string_view w) {
        return w == "prepend" || w == "append" || w == "add" || w == "delete" || w == "reorder";
    }

    // --- The layer ----------------------------------------------------------------------

    void layerMetadata() {
        while (!in_.failed() && !in_.is(')')) {
            if (in_.peek().kind == Tok::String) {
                layer_.metadata.emplace_back("documentation", Value::makeString("string", in_.take().value));
                continue;
            }
            std::string how;
            if (in_.peek().kind == Tok::Word && isListOpWord(in_.peek().text)) how = std::string(in_.take().text);
            const Token key = in_.take();
            if (key.kind != Tok::Word) {
                in_.fail("expected the layer's metadata" + describeNext(), key.line);
                return;
            }
            if (!expect('=', "after " + std::string(key.text))) return;
            if (key.text == "subLayers") {
                const Parsed p = referenceValue();
                for (const ListItem& i : itemsOf(p)) {
                    layer_.subLayers.push_back(i.text);
                    layer_.subLayerOffsets.emplace_back(i.offset, i.scale);
                }
            } else if (key.text == "doc") {
                layer_.metadata.emplace_back("documentation", generic(value()));
            } else {
                layer_.metadata.emplace_back(std::string(key.text), generic(value()));
            }
            in_.accept(';');
        }
        expect(')', "to end the layer's metadata");
    }

    // --- Prims ---------------------------------------------------------------------------

    void prim(PrimSpec& parent, const std::string& parentPath) {
        if (parentPath.size() > 100000 || ++prims_ > 50000000) {
            in_.fail("prims nested too deep", in_.line());
            in_.take();
            return;
        }
        if (!in_.isWord("def") && !in_.isWord("over") && !in_.isWord("class")) {
            in_.fail("expected def, over or class" + describeNext(), in_.line());
            in_.take();
            return;
        }
        const Token spec = in_.take();
        std::string typeName;
        if (in_.peek().kind == Tok::Word) typeName = std::string(in_.take().text);
        const Token name = in_.take();
        if (name.kind != Tok::String) {
            in_.fail("expected the prim's name in quotes", name.line);
            return;
        }
        PrimSpec& p = parent.ensureChild(name.value);
        p.specifier = spec.text == "def" ? Specifier::Def : spec.text == "over" ? Specifier::Over : Specifier::Class;
        if (!typeName.empty()) p.typeName = typeName;
        const std::string path = childPath(parentPath, name.value);
        if (in_.accept('(')) primMetadata(p, path);
        if (!expect('{', "to start the prim " + name.value)) return;
        primContents(p, path);
        expect('}', "to end the prim " + name.value);
    }

    void primMetadata(PrimSpec& p, const std::string& path) {
        while (!in_.failed() && !in_.is(')')) {
            if (in_.peek().kind == Tok::String) {
                p.metadata.emplace_back("comment", Value::makeString("string", in_.take().value));
                continue;
            }
            std::string how;
            if (in_.peek().kind == Tok::Word && isListOpWord(in_.peek().text)) how = std::string(in_.take().text);
            const Token key = in_.take();
            if (key.kind != Tok::Word) {
                in_.fail("expected the prim's metadata" + describeNext(), key.line);
                return;
            }
            if (!expect('=', "after " + std::string(key.text))) return;
            const std::string_view k = key.text;
            if (k == "references" || k == "payload") {
                std::vector<ListItem> items = itemsOf(referenceValue());
                anchorPaths(items, path, false);
                edit(k == "references" ? p.references : p.payloads, how, std::move(items));
            } else if (k == "inherits" || k == "specializes") {
                std::vector<ListItem> items = itemsOf(value());
                anchorPaths(items, path, true);
                edit(k == "inherits" ? p.inherits : p.specializes, how, std::move(items));
            } else if (k == "apiSchemas") {
                edit(p.apiSchemas, how, itemsOf(value()));
            } else if (k == "variantSets") {
                edit(p.variantSetNames, how, itemsOf(value()));
            } else if (k == "variants") {
                const Parsed v = value();
                for (const auto& [set, selection] : v.dictionary) {
                    p.variantSelections.emplace_back(set, selection.text());
                }
            } else if (k == "doc") {
                p.metadata.emplace_back("documentation", generic(value()));
            } else if (k == "clipSets") {
                ListOp op;
                edit(op, how, itemsOf(value()));
                p.metadata.emplace_back("clipSets", Value::makeList(std::move(op)));
            } else if (!how.empty()) {
                // A list edit of another list -- inactiveIds --: as the
                // crate has it, items of text.
                ListOp op;
                edit(op, how, itemsOf(value()));
                p.metadata.emplace_back(std::string(k), Value::makeList(std::move(op)));
            } else {
                Parsed v = value();
                if (k == "active" || k == "instanceable" || k == "hidden") {
                    p.metadata.emplace_back(std::string(k), convert(v, "bool"));
                } else {
                    p.metadata.emplace_back(std::string(k), generic(v));
                }
            }
            in_.accept(';');
        }
        expect(')', "to end the prim's metadata");
    }

    void primContents(PrimSpec& p, const std::string& path) {
        while (!in_.failed() && !in_.is('}')) {
            const Token& t = in_.peek();
            if (t.kind != Tok::Word) {
                in_.fail("expected a prim, a property or a variant set" + describeNext(), t.line);
                return;
            }
            if (t.text == "def" || t.text == "over" || t.text == "class") {
                prim(p, path);
            } else if (t.text == "variantSet") {
                variantSet(p, path);
            } else if (t.text == "reorder") {
                // reorder nameChildren = [...] / reorder properties = [...]; or a list edit
                in_.take();
                if (in_.isWord("nameChildren") || in_.isWord("properties")) {
                    in_.take();
                    expect('=', "after reorder");
                    value();
                } else {
                    property(p, path, "reorder");
                }
            } else {
                std::string how;
                if (isListOpWord(t.text)) how = std::string(in_.take().text);
                property(p, path, how);
            }
        }
    }

    void variantSet(PrimSpec& p, const std::string& path) {
        in_.take();  // variantSet
        const Token name = in_.take();
        if (name.kind != Tok::String) {
            in_.fail("expected the variant set's name in quotes", name.line);
            return;
        }
        if (!expect('=', "after the variant set's name") || !expect('{', "to start the variant set")) return;
        while (!in_.failed() && !in_.is('}')) {
            const Token variant = in_.take();
            if (variant.kind != Tok::String) {
                in_.fail("expected a variant's name in quotes", variant.line);
                return;
            }
            PrimSpec& v = p.ensureVariant(name.value, variant.value);
            const std::string vpath = path + "{" + name.value + "=" + variant.value + "}";
            if (in_.accept('(')) primMetadata(v, vpath);
            if (!expect('{', "to start the variant " + variant.value)) return;
            primContents(v, vpath);
            expect('}', "to end the variant " + variant.value);
        }
        expect('}', "to end the variant set");
        // The set is also one of the prim's, as a layer that writes it lists it.
    }

    void property(PrimSpec& p, const std::string& path, const std::string& how) {
        bool custom = false, uniform = false;
        for (;;) {
            if (in_.isWord("custom")) {
                in_.take();
                custom = true;
            } else if (in_.isWord("uniform") || in_.isWord("config")) {
                in_.take();
                uniform = true;
            } else if (in_.isWord("varying")) {
                in_.take();
            } else {
                break;
            }
        }
        if (in_.isWord("rel")) {
            in_.take();
            relationship(p, path, how, custom);
            return;
        }
        if (in_.peek().kind != Tok::Word) {
            in_.fail("expected a property" + describeNext(), in_.line());
            in_.take();
            return;
        }
        const Token type = in_.take();
        std::string typeName(type.text);
        if (in_.is('[')) {
            in_.take();
            expect(']', "after [");
            typeName += "[]";
        }
        const Token name = in_.take();
        if (name.kind != Tok::Word && name.kind != Tok::String) {
            in_.fail("expected the name of a " + typeName + " attribute", name.line);
            return;
        }
        const std::string attrName = name.kind == Tok::String ? name.value : std::string(name.text);
        Property& a = p.ensureProperty(attrName);
        a.typeName = typeName;
        a.custom = a.custom || custom;
        a.uniform = a.uniform || uniform;
        if (in_.is('.')) {
            in_.take();
            const Token what = in_.take();
            if (!expect('=', "after ." + std::string(what.text))) return;
            if (what.text == "timeSamples") {
                timeSamples(a);
            } else if (what.text == "connect") {
                std::vector<ListItem> items = itemsOf(value());
                anchorPaths(items, path, true);
                edit(a.targets, how, std::move(items));
            } else {
                // .spline and whatever else: skipped.
                if (in_.is('{') || in_.is('[') || in_.is('(')) skipBalanced();
                else value();
            }
            return;
        }
        if (in_.accept('=')) {
            a.value = convert(value(), typeName);
            a.hasDefault = true;
        }
        if (in_.accept('(')) propertyMetadata(a);
    }

    void relationship(PrimSpec& p, const std::string& path, const std::string& how, bool custom) {
        const Token name = in_.take();
        if (name.kind != Tok::Word) {
            in_.fail("expected the relationship's name", name.line);
            return;
        }
        Property& r = p.ensureProperty(name.text);
        r.relationship = true;
        r.custom = r.custom || custom;
        if (in_.is('[')) {
            // rel x [</target>] { ... }: attributes of a target, from long ago.
            skipBalanced();
            if (in_.is('{')) skipBalanced();
            return;
        }
        if (in_.is('.')) {
            in_.take();
            in_.take();  // default
            expect('=', "after .default");
            value();
            return;
        }
        if (in_.accept('=')) {
            std::vector<ListItem> items = itemsOf(value());
            anchorPaths(items, path, true);
            edit(r.targets, how, std::move(items));
        }
        if (in_.accept('(')) propertyMetadata(r);
    }

    void propertyMetadata(Property& a) {
        while (!in_.failed() && !in_.is(')')) {
            if (in_.peek().kind == Tok::String) {
                a.metadata.emplace_back("comment", Value::makeString("string", in_.take().value));
                continue;
            }
            if (in_.peek().kind == Tok::Word && isListOpWord(in_.peek().text)) in_.take();
            const Token key = in_.take();
            if (key.kind != Tok::Word) {
                in_.fail("expected the property's metadata" + describeNext(), key.line);
                return;
            }
            if (!expect('=', "after " + std::string(key.text))) return;
            if (key.text == "elementSize") {
                a.metadata.emplace_back("elementSize", convert(value(), "int"));
            } else if (key.text == "interpolation") {
                a.metadata.emplace_back("interpolation", convert(value(), "token"));
            } else if (key.text == "doc") {
                a.metadata.emplace_back("documentation", generic(value()));
            } else {
                a.metadata.emplace_back(std::string(key.text), generic(value()));
            }
            in_.accept(';');
        }
        expect(')', "to end the property's metadata");
    }

    void timeSamples(Property& a) {
        a.hasSamples = true;
        a.times.clear();
        a.samples.clear();
        if (!expect('{', "to start the time samples")) return;
        while (!in_.failed() && !in_.is('}')) {
            const Token time = in_.take();
            if (time.kind != Tok::Number && !(time.kind == Tok::Word && (time.text == "inf" || time.text == "-inf"))) {
                in_.fail("expected a time code", time.line);
                return;
            }
            double t = time.number;
            if (time.kind == Tok::Word) t = time.text == "inf" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
            if (std::isnan(t)) {
                in_.fail("expected a time code", time.line);
                return;
            }
            if (!expect(':', "after a time code")) return;
            Value v = convert(value(), a.typeName);
            // In order: they are written so, but need not be.
            size_t at = a.times.size();
            while (at > 0 && a.times[at - 1] > t) --at;
            if (at > 0 && a.times[at - 1] == t) {
                a.samples[at - 1] = std::move(v);
            } else {
                a.times.insert(a.times.begin() + static_cast<std::ptrdiff_t>(at), t);
                a.samples.insert(a.samples.begin() + static_cast<std::ptrdiff_t>(at), std::move(v));
            }
            if (!in_.accept(',')) in_.accept(';');
        }
        expect('}', "to end the time samples");
    }

    Scanner in_;
    Layer& layer_;
    int depth_ = 0;
    size_t prims_ = 0;
};

}  // namespace

bool parseText(std::string_view text, Layer& out, std::string& error) {
    if (text.rfind("#usda", 0) != 0) {
        error = "not USD text: it does not start with #usda";
        return false;
    }
    Parser parser(text, out);
    return parser.run(error);
}

}  // namespace pg::usd
