#include "pg/grammar/Grammar.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

namespace pg::grammar {
namespace {

// --- lexer -----------------------------------------------------------------

enum class Tok { End, Symbol, Number, Arrow, LBrace, RBrace, LParen, RParen, Colon, Pipe,
                 Star, Tilde, Quote, Comma };

struct Token {
    Tok kind = Tok::End;
    std::string text;
    float number = 0.0f;
    int line = 1;
    int col = 1;
};

std::string where(int line, int col) {
    return "line " + std::to_string(line) + ", col " + std::to_string(col) + ": ";
}

std::string describe(const Token& t) {
    switch (t.kind) {
        case Tok::End:    return "the end of the text";
        case Tok::Symbol: return "'" + t.text + "'";
        case Tok::Number: return "the number " + t.text;
        case Tok::Arrow:  return "'-->'";
        default:          return "'" + t.text + "'";
    }
}

bool lex(const std::string& src, std::vector<Token>& out, std::string& error) {
    const size_t n = src.size();
    size_t i = 0, lineStart = 0;
    int line = 1;
    auto isDigit = [&](size_t k) {
        return k < n && std::isdigit(static_cast<unsigned char>(src[k]));
    };

    while (i < n) {
        const char c = src[i];
        if (c == '\n') {
            ++line;
            lineStart = ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && src[i + 1] == '/')) {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }

        Token t;
        t.line = line;
        t.col = static_cast<int>(i - lineStart) + 1;
        const size_t start = i;

        if (src.compare(i, 3, "-->") == 0 || src.compare(i, 2, "->") == 0) {
            t.kind = Tok::Arrow;
            i += src[i + 1] == '-' ? 3 : 2;
        } else if (isDigit(i) || ((c == '.' || c == '-') && (isDigit(i + 1) ||
                   (src.compare(i, 2, "-.") == 0 && isDigit(i + 2))))) {
            if (c == '-') ++i;
            while (isDigit(i)) ++i;
            if (i < n && src[i] == '.') ++i;
            while (isDigit(i)) ++i;
            t.kind = Tok::Number;
            t.number = static_cast<float>(std::strtod(src.substr(start, i - start).c_str(), nullptr));
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < n && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) ++i;
            t.kind = Tok::Symbol;
        } else {
            switch (c) {
                case '{':  t.kind = Tok::LBrace; break;
                case '}':  t.kind = Tok::RBrace; break;
                case '(':  t.kind = Tok::LParen; break;
                case ')':  t.kind = Tok::RParen; break;
                case ':':  t.kind = Tok::Colon; break;
                case '|':  t.kind = Tok::Pipe; break;
                case '*':  t.kind = Tok::Star; break;
                case '~':  t.kind = Tok::Tilde; break;
                case '\'': t.kind = Tok::Quote; break;
                case ',':  t.kind = Tok::Comma; break;
                default:
                    error = where(t.line, t.col) + "unexpected character '" + std::string(1, c) + "'";
                    return false;
            }
            ++i;
        }
        t.text = src.substr(start, i - start);
        out.push_back(std::move(t));
    }

    Token end;
    end.line = line;
    end.col = static_cast<int>(n - lineStart) + 1;
    out.push_back(end);
    return true;
}

// --- parser ----------------------------------------------------------------

constexpr struct { const char* name; OpKind kind; } kOps[] = {
    {"extrude", OpKind::Extrude},
    {"roofHip", OpKind::RoofHip},
    {"roofGable", OpKind::RoofGable},
};

class Parser {
public:
    Parser(std::vector<Token> toks, std::vector<std::string>& attributes)
        : toks_(std::move(toks)), attributes_(attributes) {}

    const std::string& error() const { return error_; }

    bool grammar(std::vector<Rule>& rules, std::unordered_map<std::string, size_t>& index) {
        while (peek().kind != Tok::End) {
            if (peek().kind != Tok::Symbol) return fail("expected a rule, like 'Name --> ...'");
            const Token name = next();
            if (!accept(Tok::Arrow)) return fail("expected '-->' after '" + name.text + "'");

            Rule r;
            r.predecessor = name.text;
            r.line = name.line;
            if (!body(r)) return false;
            if (index.count(r.predecessor)) {
                return failAt(name, "'" + name.text + "' already has a rule");
            }
            index.emplace(r.predecessor, rules.size());
            rules.push_back(std::move(r));
        }
        return true;
    }

    /// '{' item ('|' item)* '}' '*'?
    bool pattern(SplitPattern& out) {
        if (!expect(Tok::LBrace, "'{'")) return false;
        bool grouped = false;  // seen a nested { ... }* group
        std::vector<SplitPart>* items = &out.head;
        for (;;) {
            if (peek().kind == Tok::LBrace) {
                if (grouped) return fail("a split can have only one repeat group");
                next();
                grouped = true;
                for (;;) {
                    SplitPart p;
                    if (!part(p)) return false;
                    out.repeat.push_back(std::move(p));
                    if (accept(Tok::Pipe)) continue;
                    if (!expect(Tok::RBrace, "'|' or '}'")) return false;
                    break;
                }
                if (!expect(Tok::Star, "'*' -- a nested '{ ... }*' is a repeat group")) return false;
                items = &out.tail;
            } else {
                SplitPart p;
                if (!part(p)) return false;
                items->push_back(std::move(p));
            }
            if (accept(Tok::Pipe)) continue;
            if (!expect(Tok::RBrace, "'|' or '}'")) return false;
            break;
        }
        if (peek().kind == Tok::Star) {
            // `{ ... }*` repeats the whole pattern.
            if (grouped) return fail("a split can have only one repeat group");
            next();
            out.repeat = std::move(out.head);
            out.head.clear();
        }
        return true;
    }

    bool expectEnd() {
        return peek().kind == Tok::End || fail("unexpected " + describe(peek()));
    }

private:
    const Token& peek(size_t ahead = 0) const {
        return toks_[std::min(i_ + ahead, toks_.size() - 1)];
    }
    const Token& next() {
        const Token& t = peek();
        if (i_ + 1 < toks_.size()) ++i_;
        return t;
    }
    bool accept(Tok k) {
        if (peek().kind != k) return false;
        next();
        return true;
    }
    bool expect(Tok k, const std::string& what) {
        return accept(k) || fail("expected " + what + ", found " + describe(peek()));
    }
    bool failAt(const Token& t, const std::string& msg) {
        if (error_.empty()) error_ = where(t.line, t.col) + msg;
        return false;
    }
    bool fail(const std::string& msg) { return failAt(peek(), msg); }

    /// A rule body ends where the next `Name -->` begins.
    bool atRuleStart() const {
        return peek().kind == Tok::Symbol && peek(1).kind == Tok::Arrow;
    }

    bool body(Rule& r) {
        for (;;) {
            if (peek().kind == Tok::End || atRuleStart()) return true;  // no successor
            if (peek().kind != Tok::Symbol) {
                return fail("expected an operation or a successor symbol, found " + describe(peek()));
            }
            const Token word = next();
            if (peek().kind != Tok::LParen) {  // a successor symbol ends the rule
                r.successor = word.text == kNil ? SuccessorKind::Nil : SuccessorKind::Symbol;
                r.symbol = word.text;
                break;
            }
            next();  // '('

            if (word.text == "split") {
                if (!axis(r.axis) || !expect(Tok::RParen, "')'") || !pattern(r.pattern)) return false;
                r.successor = SuccessorKind::Split;
                break;
            }
            if (word.text == "comp") {
                if (peek().kind != Tok::Symbol || peek().text != "f") {
                    return fail("expected 'f' -- only the face split comp(f) is supported");
                }
                next();
                if (!expect(Tok::RParen, "')'") || !compBlock(r.comp)) return false;
                r.successor = SuccessorKind::Comp;
                break;
            }
            const auto* op = std::find_if(std::begin(kOps), std::end(kOps),
                                          [&](const auto& o) { return word.text == o.name; });
            if (op == std::end(kOps)) return failAt(word, "unknown operation '" + word.text + "'");
            Op o;
            o.kind = op->kind;
            if (!arg(o.arg) || !expect(Tok::RParen, "')'")) return false;
            r.ops.push_back(o);
        }
        if (peek().kind != Tok::End && !atRuleStart()) {
            return fail("unexpected " + describe(peek()) + " after the successor of '" +
                        r.predecessor + "'");
        }
        return true;
    }

    bool arg(Arg& a) {
        if (peek().kind == Tok::Number) {
            a.value = next().number;
            return true;
        }
        if (peek().kind == Tok::Symbol) {
            const std::string& name = next().text;
            auto it = std::find(attributes_.begin(), attributes_.end(), name);
            if (it == attributes_.end()) it = attributes_.insert(attributes_.end(), name);
            a.attribute = static_cast<int>(it - attributes_.begin());
            return true;
        }
        return fail("expected a number or an attribute name, found " + describe(peek()));
    }

    bool axis(int& out) {
        const std::string& a = peek().kind == Tok::Symbol ? peek().text : std::string();
        if (a == "x" || a == "X") out = 0;
        else if (a == "y" || a == "Y") out = 1;
        else if (a == "z" || a == "Z") out = 2;
        else return fail("expected an axis x, y or z, found " + describe(peek()));
        next();
        return true;
    }

    /// size ':' Symbol
    bool part(SplitPart& p) {
        if (accept(Tok::Tilde)) p.mode = SizeMode::Floating;
        else if (accept(Tok::Quote)) p.mode = SizeMode::Relative;
        else p.mode = SizeMode::Absolute;

        if (peek().kind != Tok::Number) {
            return fail("expected a size like 4, '0.5 or ~3, found " + describe(peek()));
        }
        if (peek().number < 0.0f) return fail("a size cannot be negative");
        p.value = next().number;
        if (!expect(Tok::Colon, "':' after the size")) return false;
        if (peek().kind != Tok::Symbol) {
            return fail("expected a successor symbol, found " + describe(peek()));
        }
        p.symbol = next().text;
        return true;
    }

    /// '{' face ':' Symbol ('|' face ':' Symbol)* '}'
    bool compBlock(CompTargets& out) {
        if (!expect(Tok::LBrace, "'{'")) return false;
        bool seen[kFaceCount + 1] = {};
        for (;;) {
            const Token face = peek();
            int slot = -1;
            if (face.kind == Tok::Symbol) {
                for (int f = 0; f < kFaceCount; ++f) {
                    if (face.text == faceName(static_cast<Face>(f))) slot = f;
                }
                if (face.text == "side") slot = kFaceCount;
            }
            if (slot < 0) {
                return fail("expected a face (front, back, left, right, side, top, bottom), found " +
                            describe(face));
            }
            if (seen[slot]) return fail("face '" + face.text + "' is listed twice");
            seen[slot] = true;
            next();
            if (!expect(Tok::Colon, "':' after '" + face.text + "'")) return false;
            if (peek().kind != Tok::Symbol) {
                return fail("expected a successor symbol, found " + describe(peek()));
            }
            (slot == kFaceCount ? out.side : out.face[slot]) = next().text;
            if (accept(Tok::Pipe)) continue;
            return expect(Tok::RBrace, "'|' or '}'");
        }
    }

    std::vector<Token> toks_;
    std::vector<std::string>& attributes_;
    size_t i_ = 0;
    std::string error_;
};

}  // namespace

bool parseSplitPattern(const std::string& text, SplitPattern& out, std::string& error) {
    error.clear();
    out = SplitPattern{};
    std::vector<Token> toks;
    if (!lex(text, toks, error)) return false;
    std::vector<std::string> unused;
    Parser p(std::move(toks), unused);
    if (p.pattern(out) && p.expectEnd()) return true;
    error = p.error();
    return false;
}

float attributeValue(const AttributeArray* a, size_t shape, float fallback) {
    if (!a || shape >= a->size()) return fallback;
    switch (a->type()) {
        case AttrType::Float: return a->read<float>()[shape];
        case AttrType::Int:   return static_cast<float>(a->read<int32_t>()[shape]);
        default:              return fallback;
    }
}

// --- Grammar ---------------------------------------------------------------

std::unique_ptr<Grammar> Grammar::parse(const std::string& source, std::string& error) {
    error.clear();
    std::vector<Token> toks;
    if (!lex(source, toks, error)) return nullptr;

    auto g = std::unique_ptr<Grammar>(new Grammar());
    Parser p(std::move(toks), g->attributes_);
    if (!p.grammar(g->rules_, g->index_)) {
        error = p.error();
        return nullptr;
    }
    return g;
}

const Rule* Grammar::find(const std::string& symbol) const {
    auto it = index_.find(symbol);
    return it == index_.end() ? nullptr : &rules_[it->second];
}

void Grammar::apply(const Rule& rule, uint32_t parent, Shape shape,
                    const std::vector<const AttributeArray*>& attributes,
                    std::vector<Successor>& out) const {
    for (const Op& op : rule.ops) {
        const float v = op.arg.attribute < 0
                            ? op.arg.value
                            : attributeValue(attributes[static_cast<size_t>(op.arg.attribute)],
                                             parent, 0.0f);
        switch (op.kind) {
            case OpKind::Extrude:   shape = extrude(shape, v); break;
            case OpKind::RoofHip:   shape = roof(shape, RoofType::Hip, v); break;
            case OpKind::RoofGable: shape = roof(shape, RoofType::Gable, v); break;
        }
    }
    switch (rule.successor) {
        case SuccessorKind::None:   out.push_back(Successor{parent, shape, nullptr, true}); break;
        case SuccessorKind::Symbol: out.push_back(Successor{parent, shape, &rule.symbol, true}); break;
        case SuccessorKind::Nil:    break;
        case SuccessorKind::Split:  emitSplit(parent, shape, rule.axis, rule.pattern, out); break;
        case SuccessorKind::Comp:   emitComp(parent, shape, rule.comp, out); break;
    }
}

GeometryPtr Grammar::derive(const GeometryPtr& shapes, int maxDepth, DeriveStats* stats) const {
    GeometryPtr current = shapes ? shapes : std::make_shared<Geometry>();

    // Rule per symbol id, resolved once per pass instead of once per shape.
    auto rulesById = [&](const ShapeView& view) {
        std::vector<const Rule*> byId(view.symbolCount());
        for (size_t id = 0; id < byId.size(); ++id) {
            byId[id] = find(view.symbolName(static_cast<int32_t>(id)));
        }
        return byId;
    };

    // `active` marks shapes that still have a rule to apply. Shapes a rule has
    // finished with, and terminals, are never rewritten again.
    std::vector<uint8_t> active;
    {
        const ShapeView view(*current);
        const auto byId = rulesById(view);
        active.resize(view.size());
        for (size_t i = 0; i < view.size(); ++i) {
            const int32_t id = view.symbolId(i);
            active[i] = id >= 0 && byId[static_cast<size_t>(id)] ? 1 : 0;
        }
    }

    int passes = 0;
    while (passes < maxDepth && std::find(active.begin(), active.end(), 1) != active.end()) {
        const ShapeView view(*current);
        const auto byId = rulesById(view);
        std::vector<const AttributeArray*> attributes;
        for (const auto& name : attributes_) attributes.push_back(current->points().find(name));

        auto successors = rewrite(view, [&](size_t i, std::vector<Successor>& out) {
            const int32_t id = view.symbolId(i);
            const Rule* rule = active[i] && id >= 0 ? byId[static_cast<size_t>(id)] : nullptr;
            if (!rule) return false;
            apply(*rule, static_cast<uint32_t>(i), view.shape(i), attributes, out);
            return true;
        });

        std::vector<uint8_t> next(successors.size());
        for (size_t j = 0; j < successors.size(); ++j) {
            const Successor& s = successors[j];
            next[j] = s.derived && s.symbol && find(*s.symbol) ? 1 : 0;
        }
        current = materialize(*current, successors);
        active = std::move(next);
        ++passes;
    }

    if (stats) {
        stats->passes = passes;
        stats->unfinished = static_cast<size_t>(std::count(active.begin(), active.end(), 1));
    }
    return current;
}

}  // namespace pg::grammar
