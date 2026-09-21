#include "pg/nodes/Expression.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace pg::expr {
namespace {

// --- lexer -----------------------------------------------------------------

enum class Tok { End, Number, Ident, At, Dot, Assign, Plus, Minus, Star, Slash,
                 LParen, RParen, Comma, Semi, Bad };

struct Token {
    Tok kind = Tok::End;
    double number = 0.0;
    std::string text;
    size_t pos = 0;
};

std::vector<Token> lex(const std::string& src, std::string& error) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < src.size()) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        Token t;
        t.pos = i;
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < src.size() &&
             std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t end = i;
            while (end < src.size() &&
                   (std::isdigit(static_cast<unsigned char>(src[end])) || src[end] == '.')) ++end;
            t.kind = Tok::Number;
            t.number = std::strtod(src.substr(i, end - i).c_str(), nullptr);
            i = end;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = i;
            while (end < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[end])) || src[end] == '_')) ++end;
            t.kind = Tok::Ident;
            t.text = src.substr(i, end - i);
            i = end;
        } else {
            switch (c) {
                case '@': t.kind = Tok::At; break;
                case '.': t.kind = Tok::Dot; break;
                case '=': t.kind = Tok::Assign; break;
                case '+': t.kind = Tok::Plus; break;
                case '-': t.kind = Tok::Minus; break;
                case '*': t.kind = Tok::Star; break;
                case '/': t.kind = Tok::Slash; break;
                case '(': t.kind = Tok::LParen; break;
                case ')': t.kind = Tok::RParen; break;
                case ',': t.kind = Tok::Comma; break;
                case ';': t.kind = Tok::Semi; break;
                default:
                    error = "unexpected character '" + std::string(1, c) + "' at " +
                            std::to_string(i);
                    return {};
            }
            ++i;
        }
        out.push_back(std::move(t));
    }
    out.push_back(Token{Tok::End, 0, {}, src.size()});
    return out;
}

// --- AST -------------------------------------------------------------------

enum class Fn { Sin, Cos, Abs, Sqrt, Floor, Pow, Min, Max, Clamp, Length, Noise,
                Fit, MakeVec };

struct FnInfo { const char* name; Fn fn; int minArgs; int maxArgs; };

constexpr FnInfo kBuiltins[] = {
    {"sin", Fn::Sin, 1, 1},     {"cos", Fn::Cos, 1, 1},
    {"abs", Fn::Abs, 1, 1},     {"sqrt", Fn::Sqrt, 1, 1},
    {"floor", Fn::Floor, 1, 1}, {"pow", Fn::Pow, 2, 2},
    {"min", Fn::Min, 2, 2},     {"max", Fn::Max, 2, 2},
    {"clamp", Fn::Clamp, 3, 3}, {"length", Fn::Length, 1, 1},
    {"noise", Fn::Noise, 1, 1}, {"fit", Fn::Fit, 5, 5},
    {"vec3", Fn::MakeVec, 3, 3},
};

enum class Kind { Number, Read, Binary, Unary, Call };

struct AstNode {
    Kind kind = Kind::Number;
    float number = 0.0f;
    int slot = -1;
    int comp = -1;  ///< -1 = whole value, 0/1/2 = .x/.y/.z
    char op = 0;
    Fn fn = Fn::Sin;
    std::vector<std::unique_ptr<AstNode>> args;
};

struct Stmt {
    int slot = -1;
    int comp = -1;
    std::unique_ptr<AstNode> rhs;
};

enum class Ty { Float, Vec };

// --- noise -----------------------------------------------------------------

inline float hashLattice(int32_t x, int32_t y, int32_t z) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(z) * 1274126177u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

/// Trilinear value noise in [0,1). Deterministic and lattice-hashed, so it is
/// independent of evaluation order -- a requirement, not a nicety (I5).
float valueNoise(const Vec3& p) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const auto ix = static_cast<int32_t>(fx);
    const auto iy = static_cast<int32_t>(fy);
    const auto iz = static_cast<int32_t>(fz);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float sz = tz * tz * (3.0f - 2.0f * tz);

    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c000 = hashLattice(ix, iy, iz),       c100 = hashLattice(ix + 1, iy, iz);
    const float c010 = hashLattice(ix, iy + 1, iz),   c110 = hashLattice(ix + 1, iy + 1, iz);
    const float c001 = hashLattice(ix, iy, iz + 1),   c101 = hashLattice(ix + 1, iy, iz + 1);
    const float c011 = hashLattice(ix, iy + 1, iz + 1), c111 = hashLattice(ix + 1, iy + 1, iz + 1);

    return lerp(lerp(lerp(c000, c100, sx), lerp(c010, c110, sx), sy),
                lerp(lerp(c001, c101, sx), lerp(c011, c111, sx), sy), sz);
}

// --- parser ----------------------------------------------------------------

class Parser {
public:
    Parser(std::vector<Token> toks, std::vector<std::string>& slots)
        : toks_(std::move(toks)), slots_(slots) {}

    bool parseProgram(std::vector<Stmt>& out) {
        while (peek().kind != Tok::End) {
            if (accept(Tok::Semi)) continue;
            Stmt s;
            if (!parseStatement(s)) return false;
            out.push_back(std::move(s));
            if (!accept(Tok::Semi) && peek().kind != Tok::End) {
                return fail("expected ';'");
            }
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    const Token& peek() const { return toks_[i_]; }
    const Token& next() { return toks_[i_++]; }
    bool accept(Tok k) {
        if (peek().kind == k) { ++i_; return true; }
        return false;
    }
    bool fail(const std::string& msg) {
        if (error_.empty()) {
            error_ = msg + " at offset " + std::to_string(peek().pos);
        }
        return false;
    }

    int slotFor(const std::string& name) {
        for (size_t s = 0; s < slots_.size(); ++s) {
            if (slots_[s] == name) return static_cast<int>(s);
        }
        slots_.push_back(name);
        return static_cast<int>(slots_.size() - 1);
    }

    /// '@' ident ('.' [xyz])?
    bool parseAttrRef(int& slot, int& comp) {
        if (!accept(Tok::At)) return fail("expected '@'");
        if (peek().kind != Tok::Ident) return fail("expected attribute name after '@'");
        slot = slotFor(next().text);
        comp = -1;
        if (accept(Tok::Dot)) {
            if (peek().kind != Tok::Ident) return fail("expected .x/.y/.z");
            const std::string& c = next().text;
            if (c == "x") comp = 0;
            else if (c == "y") comp = 1;
            else if (c == "z") comp = 2;
            else return fail("unknown component '" + c + "'");
        }
        return true;
    }

    bool parseStatement(Stmt& s) {
        if (peek().kind != Tok::At) return fail("statement must assign to an @attribute");
        if (!parseAttrRef(s.slot, s.comp)) return false;
        if (!accept(Tok::Assign)) return fail("expected '='");
        s.rhs = parseExpr();
        return s.rhs != nullptr;
    }

    std::unique_ptr<AstNode> parseExpr() {
        auto lhs = parseTerm();
        if (!lhs) return nullptr;
        while (peek().kind == Tok::Plus || peek().kind == Tok::Minus) {
            const char op = peek().kind == Tok::Plus ? '+' : '-';
            next();
            auto rhs = parseTerm();
            if (!rhs) return nullptr;
            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Binary;
            n->op = op;
            n->args.push_back(std::move(lhs));
            n->args.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    std::unique_ptr<AstNode> parseTerm() {
        auto lhs = parseFactor();
        if (!lhs) return nullptr;
        while (peek().kind == Tok::Star || peek().kind == Tok::Slash) {
            const char op = peek().kind == Tok::Star ? '*' : '/';
            next();
            auto rhs = parseFactor();
            if (!rhs) return nullptr;
            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Binary;
            n->op = op;
            n->args.push_back(std::move(lhs));
            n->args.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    std::unique_ptr<AstNode> parseFactor() {
        if (accept(Tok::Plus)) return parseFactor();
        if (accept(Tok::Minus)) {
            auto inner = parseFactor();
            if (!inner) return nullptr;
            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Unary;
            n->op = '-';
            n->args.push_back(std::move(inner));
            return n;
        }
        return parsePrimary();
    }

    std::unique_ptr<AstNode> parsePrimary() {
        if (peek().kind == Tok::Number) {
            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Number;
            n->number = static_cast<float>(next().number);
            return n;
        }
        if (peek().kind == Tok::At) {
            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Read;
            if (!parseAttrRef(n->slot, n->comp)) return nullptr;
            return n;
        }
        if (peek().kind == Tok::Ident) {
            const std::string name = next().text;
            const FnInfo* info = nullptr;
            for (const auto& b : kBuiltins) {
                if (name == b.name) { info = &b; break; }
            }
            if (!info) { fail("unknown function '" + name + "'"); return nullptr; }
            if (!accept(Tok::LParen)) { fail("expected '(' after '" + name + "'"); return nullptr; }

            auto n = std::make_unique<AstNode>();
            n->kind = Kind::Call;
            n->fn = info->fn;
            if (!accept(Tok::RParen)) {
                for (;;) {
                    auto arg = parseExpr();
                    if (!arg) return nullptr;
                    n->args.push_back(std::move(arg));
                    if (accept(Tok::Comma)) continue;
                    if (accept(Tok::RParen)) break;
                    fail("expected ',' or ')'");
                    return nullptr;
                }
            }
            const int argc = static_cast<int>(n->args.size());
            if (argc < info->minArgs || argc > info->maxArgs) {
                fail(name + "() takes " + std::to_string(info->minArgs) + " argument(s), got " +
                     std::to_string(argc));
                return nullptr;
            }
            return n;
        }
        if (accept(Tok::LParen)) {
            auto inner = parseExpr();
            if (!inner) return nullptr;
            if (!accept(Tok::RParen)) { fail("expected ')'"); return nullptr; }
            return inner;
        }
        fail("expected an expression");
        return nullptr;
    }

    std::vector<Token> toks_;
    std::vector<std::string>& slots_;
    size_t i_ = 0;
    std::string error_;
};

// --- static type inference -------------------------------------------------

Ty inferType(const AstNode& n, const std::vector<Ty>& slotTypes) {
    switch (n.kind) {
        case Kind::Number: return Ty::Float;
        case Kind::Read:
            return n.comp >= 0 ? Ty::Float : slotTypes[static_cast<size_t>(n.slot)];
        case Kind::Unary: return inferType(*n.args[0], slotTypes);
        case Kind::Binary:
            return (inferType(*n.args[0], slotTypes) == Ty::Vec ||
                    inferType(*n.args[1], slotTypes) == Ty::Vec)
                       ? Ty::Vec : Ty::Float;
        case Kind::Call:
            switch (n.fn) {
                case Fn::Length: case Fn::Noise: case Fn::Fit: return Ty::Float;
                case Fn::MakeVec: return Ty::Vec;
                case Fn::Pow: case Fn::Min: case Fn::Max:
                    return (inferType(*n.args[0], slotTypes) == Ty::Vec ||
                            inferType(*n.args[1], slotTypes) == Ty::Vec)
                               ? Ty::Vec : Ty::Float;
                default: return inferType(*n.args[0], slotTypes);
            }
    }
    return Ty::Float;
}

// --- binding and evaluation ------------------------------------------------

struct Binding {
    enum class K { Missing, Float, Int, Vec, PointNum, NumPoints, Constant } kind = K::Missing;
    const float* rf = nullptr;
    const int32_t* ri = nullptr;
    const Vec3* rv = nullptr;
    float* wf = nullptr;
    Vec3* wv = nullptr;
    float constant = 0.0f;
};

struct EvalCtx {
    const std::vector<Binding>* bindings;
    size_t pt;
    size_t numPoints;
};

Value evalNode(const AstNode& n, const EvalCtx& e);

Value evalCall(const AstNode& n, const EvalCtx& e) {
    auto a = [&](size_t i) { return evalNode(*n.args[i], e); };
    auto unary = [&](float (*fn)(float)) {
        Value x = a(0);
        if (!x.isVec) return Value::scalar(fn(x.f));
        Vec3 v = x.v;
        return Value::vector(Vec3(fn(v.x), fn(v.y), fn(v.z)));
    };
    auto binary = [&](float (*fn)(float, float)) {
        Value x = a(0), y = a(1);
        if (!x.isVec && !y.isVec) return Value::scalar(fn(x.f, y.f));
        Vec3 u = x.asVec(), w = y.asVec();
        return Value::vector(Vec3(fn(u.x, w.x), fn(u.y, w.y), fn(u.z, w.z)));
    };

    switch (n.fn) {
        case Fn::Sin:   return unary([](float x) { return std::sin(x); });
        case Fn::Cos:   return unary([](float x) { return std::cos(x); });
        case Fn::Abs:   return unary([](float x) { return std::fabs(x); });
        case Fn::Sqrt:  return unary([](float x) { return std::sqrt(x < 0 ? 0 : x); });
        case Fn::Floor: return unary([](float x) { return std::floor(x); });
        case Fn::Pow:   return binary([](float x, float y) { return std::pow(x, y); });
        case Fn::Min:   return binary([](float x, float y) { return x < y ? x : y; });
        case Fn::Max:   return binary([](float x, float y) { return x > y ? x : y; });
        case Fn::Length: return Value::scalar(length(a(0).asVec()));
        case Fn::Noise:  return Value::scalar(valueNoise(a(0).asVec()));
        case Fn::MakeVec:
            return Value::vector(Vec3(a(0).asFloat(), a(1).asFloat(), a(2).asFloat()));
        case Fn::Clamp: {
            Value x = a(0);
            const float lo = a(1).asFloat(), hi = a(2).asFloat();
            if (!x.isVec) return Value::scalar(std::clamp(x.f, lo, hi));
            Vec3 v = x.v;
            return Value::vector(Vec3(std::clamp(v.x, lo, hi), std::clamp(v.y, lo, hi),
                                      std::clamp(v.z, lo, hi)));
        }
        case Fn::Fit: {
            const float x = a(0).asFloat(), oldMin = a(1).asFloat(), oldMax = a(2).asFloat();
            const float newMin = a(3).asFloat(), newMax = a(4).asFloat();
            const float d = oldMax - oldMin;
            const float t = d == 0.0f ? 0.0f : (x - oldMin) / d;
            return Value::scalar(newMin + (newMax - newMin) * std::clamp(t, 0.0f, 1.0f));
        }
    }
    return Value::scalar(0.0f);
}

Value evalNode(const AstNode& n, const EvalCtx& e) {
    switch (n.kind) {
        case Kind::Number: return Value::scalar(n.number);
        case Kind::Read: {
            const Binding& b = (*e.bindings)[static_cast<size_t>(n.slot)];
            Value v;
            switch (b.kind) {
                case Binding::K::Vec:   v = Value::vector(b.rv ? b.rv[e.pt] : Vec3()); break;
                case Binding::K::Float: v = Value::scalar(b.rf ? b.rf[e.pt] : 0.0f); break;
                case Binding::K::Int:
                    v = Value::scalar(b.ri ? static_cast<float>(b.ri[e.pt]) : 0.0f); break;
                case Binding::K::PointNum:  v = Value::scalar(static_cast<float>(e.pt)); break;
                case Binding::K::NumPoints: v = Value::scalar(static_cast<float>(e.numPoints)); break;
                case Binding::K::Constant:  v = Value::scalar(b.constant); break;
                case Binding::K::Missing:   v = Value::scalar(0.0f); break;
            }
            if (n.comp >= 0) return Value::scalar(v.asVec()[n.comp]);
            return v;
        }
        case Kind::Unary: {
            Value x = evalNode(*n.args[0], e);
            if (x.isVec) return Value::vector(x.v * -1.0f);
            return Value::scalar(-x.f);
        }
        case Kind::Binary: {
            Value x = evalNode(*n.args[0], e), y = evalNode(*n.args[1], e);
            if (!x.isVec && !y.isVec) {
                switch (n.op) {
                    case '+': return Value::scalar(x.f + y.f);
                    case '-': return Value::scalar(x.f - y.f);
                    case '*': return Value::scalar(x.f * y.f);
                    default:  return Value::scalar(y.f == 0.0f ? 0.0f : x.f / y.f);
                }
            }
            Vec3 u = x.asVec(), w = y.asVec();
            switch (n.op) {
                case '+': return Value::vector(u + w);
                case '-': return Value::vector(u - w);
                case '*': return Value::vector(u * w);
                default:
                    return Value::vector(Vec3(w.x == 0 ? 0 : u.x / w.x,
                                              w.y == 0 ? 0 : u.y / w.y,
                                              w.z == 0 ? 0 : u.z / w.z));
            }
        }
        case Kind::Call: return evalCall(n, e);
    }
    return Value::scalar(0.0f);
}

}  // namespace

// --- Program ---------------------------------------------------------------

struct Program::Impl {
    std::vector<std::string> slotNames;
    std::vector<Stmt> stmts;
};

Program::Program() : impl_(std::make_unique<Impl>()) {}
Program::~Program() = default;

const std::vector<std::string>& Program::slotNames() const { return impl_->slotNames; }

std::unique_ptr<Program> Program::parse(const std::string& source, std::string& error) {
    error.clear();
    auto toks = lex(source, error);
    if (!error.empty()) return nullptr;

    std::unique_ptr<Program> prog(new Program());
    Parser parser(std::move(toks), prog->impl_->slotNames);
    if (!parser.parseProgram(prog->impl_->stmts)) {
        error = parser.error().empty() ? "parse error" : parser.error();
        return nullptr;
    }
    return prog;
}

bool Program::run(Geometry& geo, const CookContext& ctx, std::string& error) const {
    error.clear();
    const auto& slots = impl_->slotNames;
    const size_t numPoints = geo.pointCount();
    const size_t slotCount = slots.size();

    auto isSpecial = [](const std::string& n) {
        return n == "ptnum" || n == "numpt" || n == "Time" || n == "Frame";
    };

    // 1. Seed slot types from what the geometry already has.
    std::vector<Ty> slotTypes(slotCount, Ty::Float);
    for (size_t s = 0; s < slotCount; ++s) {
        if (isSpecial(slots[s])) continue;
        if (const AttributeArray* a = geo.points().find(slots[s])) {
            slotTypes[s] = a->type() == AttrType::Vec3 ? Ty::Vec : Ty::Float;
        }
    }

    // 2. Walk the statements in order; a write decides the type of everything
    //    that reads the slot afterwards.
    std::vector<bool> written(slotCount, false);
    for (const auto& st : impl_->stmts) {
        const auto slot = static_cast<size_t>(st.slot);
        if (isSpecial(slots[slot])) {
            error = "cannot assign to @" + slots[slot];
            return false;
        }
        if (st.comp >= 0) {
            slotTypes[slot] = Ty::Vec;  // component assignment implies a vector
        } else {
            slotTypes[slot] = inferType(*st.rhs, slotTypes);
        }
        written[slot] = true;
    }

    // 3. Create every written attribute before binding anything: creating one
    //    can replace an existing array and invalidate pointers into it.
    for (size_t s = 0; s < slotCount; ++s) {
        if (!written[s]) continue;
        geo.points().create(slots[s], slotTypes[s] == Ty::Vec ? AttrType::Vec3 : AttrType::Float);
    }

    // 4. Bind.
    std::vector<Binding> bindings(slotCount);
    for (size_t s = 0; s < slotCount; ++s) {
        Binding& b = bindings[s];
        const std::string& name = slots[s];
        if (name == "ptnum") { b.kind = Binding::K::PointNum; continue; }
        if (name == "numpt") { b.kind = Binding::K::NumPoints; continue; }
        if (name == "Time")  { b.kind = Binding::K::Constant; b.constant = static_cast<float>(ctx.time); continue; }
        if (name == "Frame") { b.kind = Binding::K::Constant; b.constant = static_cast<float>(ctx.frame); continue; }

        AttributeArray* a = geo.points().find(name);
        if (!a) { b.kind = Binding::K::Missing; continue; }

        switch (a->type()) {
            case AttrType::Vec3:
                b.kind = Binding::K::Vec;
                if (written[s]) { b.wv = a->write<Vec3>().data(); b.rv = b.wv; }
                else { b.rv = a->read<Vec3>().data(); }
                break;
            case AttrType::Float:
                b.kind = Binding::K::Float;
                if (written[s]) { b.wf = a->write<float>().data(); b.rf = b.wf; }
                else { b.rf = a->read<float>().data(); }
                break;
            case AttrType::Int:
                if (written[s]) { error = "@" + name + " is an int attribute; writing ints is not supported yet"; return false; }
                b.kind = Binding::K::Int;
                b.ri = a->read<int32_t>().data();
                break;
            default:
                b.kind = Binding::K::Missing;
                break;
        }
    }

    // 5. Execute. Chunks are disjoint in the point index, and the language can
    //    only touch the current point, so there is nothing to synchronise.
    const auto& stmts = impl_->stmts;
    parallelFor(numPoints, 4096, [&](size_t begin, size_t end) {
        EvalCtx e{&bindings, 0, numPoints};
        for (size_t i = begin; i < end; ++i) {
            e.pt = i;
            for (const auto& st : stmts) {
                const Value v = evalNode(*st.rhs, e);
                Binding& b = const_cast<Binding&>(bindings[static_cast<size_t>(st.slot)]);
                if (st.comp >= 0) {
                    if (b.wv) b.wv[i][st.comp] = v.asFloat();
                } else if (b.wv) {
                    b.wv[i] = v.asVec();
                } else if (b.wf) {
                    b.wf[i] = v.asFloat();
                }
            }
        }
    });
    return true;
}

}  // namespace pg::expr
