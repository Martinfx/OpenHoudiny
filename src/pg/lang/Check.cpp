// The checker: turns the parse into a typed program for one run -- the types
// of the attributes from the geometry it runs on, every @name bound, every
// function call resolved to an overload, every conversion made explicit.
#include "pg/lang/Check.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace pg::lang {

const char* typeName(Type t) {
    switch (t) {
        case Type::Void: return "void";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::Vec2: return "vector2";
        case Type::Vec3: return "vector";
        case Type::Vec4: return "vector4";
        case Type::Mat3: return "matrix3";
        case Type::Mat4: return "matrix";
        case Type::String: return "string";
        case Type::IntArray: return "int[]";
        case Type::FloatArray: return "float[]";
        case Type::Vec2Array: return "vector2[]";
        case Type::Vec3Array: return "vector[]";
        case Type::Vec4Array: return "vector4[]";
        case Type::StringArray: return "string[]";
    }
    return "?";
}

Type elementOf(Type a) {
    switch (a) {
        case Type::IntArray: return Type::Int;
        case Type::FloatArray: return Type::Float;
        case Type::Vec2Array: return Type::Vec2;
        case Type::Vec3Array: return Type::Vec3;
        case Type::Vec4Array: return Type::Vec4;
        case Type::StringArray: return Type::String;
        default: return Type::Void;
    }
}

Type arrayOf(Type e) {
    switch (e) {
        case Type::Int: return Type::IntArray;
        case Type::Float: return Type::FloatArray;
        case Type::Vec2: return Type::Vec2Array;
        case Type::Vec3: return Type::Vec3Array;
        case Type::Vec4: return Type::Vec4Array;
        case Type::String: return Type::StringArray;
        default: return Type::Void;
    }
}

Type fromAttr(AttrType t) {
    switch (t) {
        case AttrType::Int: return Type::Int;
        case AttrType::Float: return Type::Float;
        case AttrType::Vec2: return Type::Vec2;
        case AttrType::Vec3: return Type::Vec3;
        case AttrType::Vec4: return Type::Vec4;
        case AttrType::String: return Type::String;
    }
    return Type::Float;
}

bool toAttr(Type t, AttrType& out) {
    switch (t) {
        case Type::Int: out = AttrType::Int; return true;
        case Type::Float: out = AttrType::Float; return true;
        case Type::Vec2: out = AttrType::Vec2; return true;
        case Type::Vec3: out = AttrType::Vec3; return true;
        case Type::Vec4: out = AttrType::Vec4; return true;
        case Type::String: out = AttrType::String; return true;
        default: return false;
    }
}

namespace {

/// The type an attribute of this name has when nothing says otherwise --
/// Houdini's conventions: P, N, v, Cd are vectors, id an int, name a string.
Type knownType(const std::string& name) {
    static const std::map<std::string, Type, std::less<>> known = {
        {"P", Type::Vec3},      {"N", Type::Vec3},     {"v", Type::Vec3},       {"Cd", Type::Vec3},
        {"up", Type::Vec3},     {"uv", Type::Vec3},    {"force", Type::Vec3},   {"rest", Type::Vec3},
        {"accel", Type::Vec3},  {"scale", Type::Vec3}, {"center", Type::Vec3},  {"pivot", Type::Vec3},
        {"trans", Type::Vec3},  {"w", Type::Vec3},     {"torque", Type::Vec3},  {"orient", Type::Vec4},
        {"rot", Type::Vec4},    {"id", Type::Int},     {"nextid", Type::Int},   {"pstate", Type::Int},
        {"name", Type::String}, {"instance", Type::String}};
    const auto it = known.find(name);
    return it == known.end() ? Type::Void : it->second;
}

const char* className(AttrClass c) {
    switch (c) {
        case AttrClass::Detail: return "detail";
        case AttrClass::Point: return "point";
        case AttrClass::Vertex: return "vertex";
        case AttrClass::Primitive: return "primitive";
    }
    return "?";
}

TPtr mk(Op op, Type t, const Pos& p) {
    auto n = std::make_unique<TNode>();
    n->op = op;
    n->type = t;
    n->pos = p;
    return n;
}

TPtr constInt(int32_t v, const Pos& p) {
    auto n = mk(Op::Const, Type::Int, p);
    n->ci = v;
    return n;
}

TPtr constFloat(float v, const Pos& p) {
    auto n = mk(Op::Const, Type::Float, p);
    n->cf = v;
    return n;
}

TPtr constVec(Type t, const Vec4& v, const Pos& p) {
    auto n = mk(Op::Const, t, p);
    n->cv = v;
    return n;
}

TPtr constStr(std::string v, const Pos& p) {
    auto n = mk(Op::Const, Type::String, p);
    n->cs = std::move(v);
    return n;
}

int component(const std::string& c) {
    if (c == "x" || c == "r" || c == "u") return 0;
    if (c == "y" || c == "g" || c == "v") return 1;
    if (c == "z" || c == "b") return 2;
    if (c == "w" || c == "a") return 3;
    return -1;
}

/// A value 0 of any type, as a constant.
TPtr zeroOf(Type t, const Pos& p) {
    switch (t) {
        case Type::Int: return constInt(0, p);
        case Type::Float: return constFloat(0.0f, p);
        case Type::Vec2:
        case Type::Vec3:
        case Type::Vec4: return constVec(t, Vec4(), p);
        case Type::String: return constStr({}, p);
        default: {
            auto n = mk(Op::Make, t, p);  // an empty array, a zero matrix
            return n;
        }
    }
}

}  // namespace

// --- the checker ---------------------------------------------------------------------------------

class Checker {
public:
    Checker(const Ast& ast, Run& run, Geometry* geo, bool fold, Checked& out)
        : ast_(ast), run_(run), geo_(geo), fold_(fold), out_(out) {
        for (size_t i = 0; i < ast.functions.size(); ++i) fnIndex_[ast.functions[i].name] = static_cast<int>(i);
        out_.functions.resize(ast.functions.size());
        for (size_t i = 0; i < ast.functions.size(); ++i) {
            out_.functions[i].name = ast.functions[i].name;
            out_.functions[i].ret = ast.functions[i].ret;
        }
    }

    bool program() {
        scopes_.emplace_back();
        for (const StmtPtr& s : ast_.main) {
            TSPtr t = stmt(*s);
            if (!t) return false;
            out_.main.push_back(std::move(t));
        }
        // A function no one calls is checked anyway: its errors show.
        for (size_t i = 0; i < ast_.functions.size(); ++i) {
            if (!function(static_cast<int>(i))) return false;
        }
        return error_.empty();
    }

    bool expression() {
        scopes_.emplace_back();
        const Stmt& s = *ast_.main.front();
        TPtr e = expr(*s.expr, Type::Void);
        if (!e) return false;
        if (e->type != Type::Int && e->type != Type::Float && !isVector(e->type) && e->type != Type::String) {
            return fail(s.pos, std::string("an expression gives a number, a vector or a string, not ") +
                                   typeName(e->type));
        }
        out_.exprType = e->type;
        auto r = std::make_unique<TStmt>();
        r->kind = TS::Return;
        r->type = e->type;
        r->slot = slot(e->type);
        out_.exprSlot = r->slot;
        r->expr = std::move(e);
        out_.main.push_back(std::move(r));
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool fail(const Pos& p, const std::string& message) {
        if (error_.empty()) error_ = at(p, message);
        return false;
    }
    TPtr failNull(const Pos& p, const std::string& message) {
        fail(p, message);
        return nullptr;
    }

    int slot(Type t) { return run_.slotCounts[static_cast<size_t>(t)]++; }

    // --- variables ---------------------------------------------------------------------------

    struct Var {
        Type type = Type::Void;
        int slot = -1;
    };

    const Var* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    bool declare(const std::string& name, Type t, int s, const Pos& p) {
        if (scopes_.back().count(name)) return fail(p, "'" + name + "' is declared twice here");
        if (builtins().count(name) || fnIndex_.count(name)) {
            // A variable may share a function's name (VEX allows it); calls still find the function.
        }
        scopes_.back()[name] = {t, s};
        return true;
    }

    // --- attributes ----------------------------------------------------------------------------

    const Geometry* inputGeo(int input) const {
        if (input <= 0) return geo_;
        return run_.inputs[static_cast<size_t>(input)];
    }

    /// The binding of an @name, made on first sight: `write` for a write,
    /// `inferred` the type a new attribute takes from what is written to it.
    int bind(const Expr& a, bool write, Type inferred) {
        if (!geo_) {
            fail(a.pos, "@" + a.text + ": an expression of a parameter has no attributes");
            return -1;
        }
        const std::string key = std::to_string(a.input) + ":" + a.text;
        const auto found = bindIndex_.find(key);
        if (found != bindIndex_.end()) {
            Binding& b = run_.bindings[static_cast<size_t>(found->second)];
            if (a.type != Type::Void && a.type != b.type && !(isScalar(a.type) && isScalar(b.type))) {
                fail(a.pos, "@" + a.text + " is a " + typeName(b.type) + ", not a " + typeName(a.type));
                return -1;
            }
            if (write && !markWritten(b, a.pos)) return -1;
            return found->second;
        }

        Binding b;
        b.name = a.text;
        b.input = a.input > 0 ? a.input : -1;
        b.cls = run_.cls;
        const AttrClass cls = run_.cls;

        // What the run knows.
        using K = Binding::K;
        static const std::map<std::string, std::pair<K, Type>, std::less<>> specials = {
            {"elemnum", {K::ElemNum, Type::Int}}, {"ptnum", {K::PtNum, Type::Int}},
            {"primnum", {K::PrimNum, Type::Int}}, {"vtxnum", {K::VtxNum, Type::Int}},
            {"numelem", {K::NumElem, Type::Int}}, {"numpt", {K::NumPt, Type::Int}},
            {"numprim", {K::NumPrim, Type::Int}}, {"numvtx", {K::NumVtx, Type::Int}},
            {"Time", {K::Time, Type::Float}},     {"Frame", {K::Frame, Type::Float}},
            {"TimeInc", {K::TimeInc, Type::Float}}};
        if (a.input == 0) {
            const auto sp = specials.find(a.text);
            const bool centroid = a.text == "P" && cls == AttrClass::Primitive && (a.type == Type::Void || a.type == Type::Vec3);
            if (sp != specials.end() || centroid) {
                if (write) {
                    fail(a.pos, "cannot assign to @" + a.text + ": it is read only" +
                                    (centroid ? " -- in a primitive wrangle @P is the middle of the primitive" : ""));
                    return -1;
                }
                b.kind = centroid ? K::Centroid : sp->second.first;
                b.type = centroid ? Type::Vec3 : sp->second.second;
                if (b.kind == K::PrimNum && cls == AttrClass::Vertex) out_.needVertexPrim = true;
                if (b.kind == K::Centroid) out_.needCentroids = true;
                return add(key, std::move(b));
            }
            if (a.text.rfind("group_", 0) == 0 && a.text.size() > 6) {
                if (cls == AttrClass::Detail) {
                    fail(a.pos, "a detail wrangle has no @group_: setpointgroup() and setprimgroup() set groups");
                    return -1;
                }
                b.kind = K::Group;
                b.type = Type::Int;
                b.name = a.text.substr(6);
                if (write) b.written = true;
                return add(key, std::move(b));
            }
        }

        if (isArray(a.type) || isMatrix(a.type)) {
            fail(a.pos, "@" + a.text + ": attributes of " + typeName(a.type) + " are not supported yet");
            return -1;
        }

        // An attribute: of the class run over, else -- read -- one it maps to.
        const Geometry* g = inputGeo(a.input);
        struct Try {
            AttrClass cls;
            Binding::Map map;
        };
        std::vector<Try> order = {{cls, cls == AttrClass::Detail ? Binding::Map::Same : Binding::Map::Same}};
        if (cls == AttrClass::Vertex) {
            order.push_back({AttrClass::Point, Binding::Map::VertexPoint});
            order.push_back({AttrClass::Primitive, Binding::Map::VertexPrim});
        }
        if (cls != AttrClass::Detail) order.push_back({AttrClass::Detail, Binding::Map::Detail});
        if (g) {
            for (const Try& t : order) {
                const AttributeArray* arr = g->attributes(t.cls).find(a.text);
                if (!arr) continue;
                const Type at = fromAttr(arr->type());
                if (a.type != Type::Void && a.type != at && !(isScalar(a.type) && isScalar(at))) {
                    fail(a.pos, "@" + a.text + " is a " + std::string(typeName(at)) + " attribute, not a " +
                                    typeName(a.type));
                    return -1;
                }
                b.kind = K::Attr;
                b.type = at;
                b.cls = t.cls;
                b.map = t.map;
                if (t.map == Binding::Map::VertexPrim) out_.needVertexPrim = true;
                if (write && !markWritten(b, a.pos)) return -1;
                return add(key, std::move(b));
            }
        }
        // Not there: made if written, zero if read.
        Type t = a.type;
        if (t == Type::Void) t = knownType(a.text);
        if (t == Type::Void && write && inferred != Type::Void) t = inferred == Type::Int ? Type::Float : inferred;
        if (t == Type::Void) t = Type::Float;
        if (isArray(t) || isMatrix(t) || t == Type::Void) {
            fail(a.pos, "@" + a.text + " would be a " + std::string(typeName(t)) + " attribute: not supported yet");
            return -1;
        }
        b.kind = K::Missing;
        b.type = t;
        b.cls = cls;
        b.map = Binding::Map::Same;
        if (write && !markWritten(b, a.pos)) return -1;
        return add(key, std::move(b));
    }

    bool markWritten(Binding& b, const Pos& p) {
        using K = Binding::K;
        if (b.kind != K::Attr && b.kind != K::Missing && b.kind != K::Group) {
            return fail(p, "cannot assign to @" + b.name + ": it is read only");
        }
        if (b.input >= 0) return fail(p, "@opinput" + std::to_string(b.input) + "_" + b.name + " is read only");
        if (b.kind == K::Attr && b.cls != run_.cls) {
            return fail(p, "@" + b.name + " is a " + className(b.cls) + " attribute: a " + className(run_.cls) +
                               " wrangle reads it, and cannot write it");
        }
        b.written = true;
        if (b.type == Type::String) out_.ordered = true;
        return true;
    }

    int add(const std::string& key, Binding b) {
        run_.bindings.push_back(std::move(b));
        const int i = static_cast<int>(run_.bindings.size() - 1);
        bindIndex_[key] = i;
        return i;
    }

    /// The type a new attribute's first write would give it, when it is new,
    /// has no prefix and no known type: Void otherwise.
    bool needsInference(const Expr& target) const {
        const Expr* a = &target;
        while (a->kind == EK::Member || a->kind == EK::Index) a = a->args[0].get();
        if (a->kind != EK::Attr || a->type != Type::Void || a->input != 0 || !geo_) return false;
        if (bindIndex_.count("0:" + a->text)) return false;
        if (knownType(a->text) != Type::Void) return false;
        if (a->text.rfind("group_", 0) == 0) return false;
        for (AttrClass c : {run_.cls, AttrClass::Point, AttrClass::Primitive, AttrClass::Vertex, AttrClass::Detail}) {
            if (geo_->attributes(c).find(a->text)) return false;
        }
        return target.kind == EK::Attr;  // a component written: a vector, below
    }

    // --- conversions ---------------------------------------------------------------------------

    /// `n` as a `to`; null, with why, if it cannot be. `explicitCast`: a
    /// cast the program asked for (vector(x), int(s)).
    TPtr convert(TPtr n, Type to, const Pos& p, bool explicitCast = false) {
        if (!n) return nullptr;
        const Type from = n->type;
        if (from == to) return n;
        bool ok = false;
        if (isScalar(from) && (isScalar(to) || isVector(to) || isMatrix(to))) ok = true;
        if (explicitCast) {
            if (isVector(from) && isVector(to)) ok = true;
            if (isMatrix(from) && isMatrix(to)) ok = true;
            if (from == Type::String && isScalar(to)) ok = true;
            if (isNumeric(from) && !isMatrix(from) && to == Type::String) ok = true;
            if (from == Type::Vec4 && to == Type::Mat3) ok = true;  // a quaternion as a rotation
        }
        if (!ok) {
            return failNull(p, std::string("cannot turn a ") + typeName(from) + " into a " + typeName(to) +
                                   (isVector(from) && isScalar(to) ? " -- take a component: .x, [0], length()" : ""));
        }
        // Constants fold.
        if (n->op == Op::Const && isScalar(from)) {
            const double v = from == Type::Int ? static_cast<double>(n->ci) : static_cast<double>(n->cf);
            if (to == Type::Float) return constFloat(static_cast<float>(v), p);
            if (to == Type::Int) {
                const double c = std::isfinite(v) ? std::clamp(std::trunc(v), -2147483648.0, 2147483647.0) : 0.0;
                return constInt(static_cast<int32_t>(c), p);
            }
            if (isVector(to)) {
                const auto f = static_cast<float>(v);
                return constVec(to, Vec4(f, f, f, f), p);
            }
        }
        auto c = mk(Op::Cast, to, p);
        c->sub = from;
        c->kids.push_back(std::move(n));
        return c;
    }

    /// A condition: nonzero, non-empty.
    TPtr truth(TPtr n, const Pos& p) {
        if (!n) return nullptr;
        if (n->type == Type::Int) return n;
        if (n->type == Type::Float || isVector(n->type) || n->type == Type::String) {
            auto c = mk(Op::Ne, Type::Int, p);
            c->sub = n->type;
            TPtr zero = zeroOf(n->type, p);
            c->kids.push_back(std::move(n));
            c->kids.push_back(std::move(zero));
            return c;
        }
        return failNull(p, std::string("a ") + typeName(n->type) + " is not a condition");
    }

    /// The type both sides of an arithmetic take: Void if none.
    static Type unify(Type a, Type b) {
        if (a == b) return a;
        if (isScalar(a) && isScalar(b)) return Type::Float;
        if (isScalar(a) && isVector(b)) return b;
        if (isVector(a) && isScalar(b)) return a;
        return Type::Void;
    }

    // --- expressions ---------------------------------------------------------------------------

    TPtr expr(const Expr& e, Type hint) {
        switch (e.kind) {
            case EK::Int: {
                if (e.ival > 2147483647LL) return constFloat(static_cast<float>(e.ival), e.pos);
                return constInt(static_cast<int32_t>(e.ival), e.pos);
            }
            case EK::Float: return constFloat(static_cast<float>(e.fval), e.pos);
            case EK::String: return constStr(e.text, e.pos);
            case EK::Braces: return braces(e, hint);
            case EK::Var: return var(e);
            case EK::Dollar: return dollar(e);
            case EK::Attr: {
                const int b = bind(e, false, Type::Void);
                if (b < 0) return nullptr;
                auto n = mk(Op::Attr, run_.bindings[static_cast<size_t>(b)].type, e.pos);
                n->a = b;
                return n;
            }
            case EK::Member: return member(e, hint);
            case EK::Index: return index(e);
            case EK::Call: return call(e, hint);
            case EK::Cast: return cast(e);
            case EK::Unary: return unary(e, hint);
            case EK::Binary: return binary(e, hint);
            case EK::Assign: return assign(e);
            case EK::PreInc:
            case EK::PreDec:
            case EK::PostInc:
            case EK::PostDec: return incdec(e);
            case EK::Ternary: return ternary(e, hint);
        }
        return failNull(e.pos, "cannot use this here");
    }

    TPtr var(const Expr& e) {
        if (const Var* v = lookup(e.text)) {
            auto n = mk(Op::Local, v->type, e.pos);
            n->a = v->slot;
            return n;
        }
        static const std::map<std::string, double, std::less<>> constants = {
            {"M_PI", 3.14159265358979323846},  {"M_TWO_PI", 6.28318530717958647692}, {"M_PI_2", 1.57079632679489661923},
            {"M_E", 2.71828182845904523536},   {"M_SQRT2", 1.41421356237309504880},  {"M_LN2", 0.69314718055994530942}};
        if (auto c = constants.find(e.text); c != constants.end()) return constFloat(static_cast<float>(c->second), e.pos);
        return failNull(e.pos, "unknown variable '" + e.text + "'");
    }

    TPtr dollar(const Expr& e) {
        static const std::set<std::string, std::less<>> ints = {"F", "SF", "EF"};
        const Type t = ints.count(e.text) ? Type::Int : Type::Float;
        if (fold_) {
            double v = 0.0;
            if (!run_.host || !run_.host->variable(e.text, v)) return failNull(e.pos, "unknown variable $" + e.text);
            if (t == Type::Int) return constInt(static_cast<int32_t>(std::lround(v)), e.pos);
            return constFloat(static_cast<float>(v), e.pos);
        }
        auto n = mk(Op::Builtin, t, e.pos);
        n->fn = implHostVariable(t);
        n->cs = e.text;
        return n;
    }

    TPtr braces(const Expr& e, Type hint) {
        const size_t count = e.args.size();
        Type t = Type::Void;
        if (isArray(hint)) {
            t = hint;
        } else if ((isVector(hint) && static_cast<size_t>(width(hint)) == count) || (hint == Type::Mat3 && count == 9) ||
                   (hint == Type::Mat4 && count == 16)) {
            t = hint;
        } else if (count == 2) {
            t = Type::Vec2;
        } else if (count == 3) {
            t = Type::Vec3;
        } else if (count == 4) {
            t = Type::Vec4;
        } else if (count == 9) {
            t = Type::Mat3;
        } else if (count == 16) {
            t = Type::Mat4;
        } else {
            return failNull(e.pos, "{...} of " + std::to_string(count) +
                                       " values: a vector has 2, 3 or 4 -- an array needs a declared type");
        }
        return make(e, t);
    }

    /// {a, b, c}, set(a, b, c), array(a, b): a vector, a matrix, an array.
    TPtr make(const Expr& e, Type t) {
        const Type elem = isArray(t) ? elementOf(t) : Type::Float;
        auto n = mk(Op::Make, t, e.pos);
        bool constant = !isArray(t) && !isMatrix(t);
        for (const auto& a : e.args) {
            TPtr k = convert(expr(*a, elem), elem, a->pos);
            if (!k) return nullptr;
            constant = constant && k->op == Op::Const;
            n->kids.push_back(std::move(k));
        }
        if (constant) {
            Vec4 v;
            for (size_t i = 0; i < n->kids.size() && i < 4; ++i) (&v.x)[i] = n->kids[i]->cf;
            return constVec(t, v, e.pos);
        }
        return n;
    }

    TPtr member(const Expr& e, Type) {
        TPtr base = expr(*e.args[0], Type::Void);
        if (!base) return nullptr;
        const int c = component(e.text);
        if (!isVector(base->type)) {
            return failNull(e.pos, std::string("a ") + typeName(base->type) + " has no components: ." + e.text);
        }
        if (c < 0 || c >= width(base->type)) {
            return failNull(e.pos, std::string("a ") + typeName(base->type) + " has no component '" + e.text + "'");
        }
        if (base->op == Op::Const) return constFloat((&base->cv.x)[c], e.pos);
        auto n = mk(Op::Comp, Type::Float, e.pos);
        n->sub = base->type;
        n->a = c;
        n->kids.push_back(std::move(base));
        return n;
    }

    TPtr index(const Expr& e) {
        TPtr base = expr(*e.args[0], Type::Void);
        TPtr i = convert(expr(*e.args[1], Type::Int), Type::Int, e.args[1]->pos);
        if (!base || !i) return nullptr;
        Type t;
        if (isArray(base->type)) t = elementOf(base->type);
        else if (isVector(base->type)) t = Type::Float;
        else if (base->type == Type::String) t = Type::String;
        else return failNull(e.pos, std::string("a ") + typeName(base->type) + " cannot be indexed");
        auto n = mk(Op::Index, t, e.pos);
        n->sub = base->type;
        n->kids.push_back(std::move(base));
        n->kids.push_back(std::move(i));
        return n;
    }

    TPtr cast(const Expr& e) {
        const Type to = e.type;
        if (e.args.size() == 1) {
            TPtr a = expr(*e.args[0], to);
            if (!a) return nullptr;
            if (isArray(to) || isArray(a->type)) {
                if (a->type == to) return a;
                return failNull(e.pos, std::string("cannot turn a ") + typeName(a->type) + " into a " + typeName(to));
            }
            return convert(std::move(a), to, e.pos, true);
        }
        // vector(1, 2, 3), matrix3(...): made from its components.
        if (isVector(to) || isMatrix(to)) {
            const size_t want = isVector(to) ? static_cast<size_t>(width(to)) : (to == Type::Mat3 ? 9 : 16);
            if (e.args.size() != want) {
                return failNull(e.pos, std::string(typeName(to)) + "() takes 1 or " + std::to_string(want) +
                                           " argument(s), got " + std::to_string(e.args.size()));
            }
            return make(e, to);
        }
        return failNull(e.pos, std::string(typeName(to)) + "() takes one argument");
    }

    TPtr unary(const Expr& e, Type hint) {
        TPtr a = expr(*e.args[0], hint);
        if (!a) return nullptr;
        if (e.op == Tok::Not) {
            TPtr t = truth(std::move(a), e.pos);
            if (!t) return nullptr;
            auto n = mk(Op::Not, Type::Int, e.pos);
            n->kids.push_back(std::move(t));
            return n;
        }
        if (e.op == Tok::Tilde) {
            if (a->type != Type::Int) return failNull(e.pos, "~ takes an int");
            auto n = mk(Op::BitNot, Type::Int, e.pos);
            n->kids.push_back(std::move(a));
            return n;
        }
        if (!isNumeric(a->type)) return failNull(e.pos, std::string("cannot negate a ") + typeName(a->type));
        if (a->op == Op::Const) {
            if (a->type == Type::Int) return constInt(static_cast<int32_t>(0u - static_cast<uint32_t>(a->ci)), e.pos);
            if (a->type == Type::Float) return constFloat(-a->cf, e.pos);
            if (isVector(a->type)) return constVec(a->type, Vec4(-a->cv.x, -a->cv.y, -a->cv.z, -a->cv.w), e.pos);
        }
        auto n = mk(Op::Neg, a->type, e.pos);
        n->kids.push_back(std::move(a));
        return n;
    }

    TPtr binary(const Expr& e, Type hint) {
        const Tok op = e.op;
        if (op == Tok::AndAnd || op == Tok::OrOr) {
            TPtr a = truth(expr(*e.args[0], Type::Void), e.pos);
            TPtr b = truth(expr(*e.args[1], Type::Void), e.pos);
            if (!a || !b) return nullptr;
            auto n = mk(op == Tok::AndAnd ? Op::And : Op::Or, Type::Int, e.pos);
            n->kids.push_back(std::move(a));
            n->kids.push_back(std::move(b));
            return n;
        }
        const bool arithmetic = op == Tok::Plus || op == Tok::Minus || op == Tok::Star || op == Tok::Slash;
        const Type down = arithmetic && (isVector(hint) || hint == Type::Float) ? hint : Type::Void;
        TPtr a = expr(*e.args[0], down);
        TPtr b = expr(*e.args[1], down);
        if (!a || !b) return nullptr;
        return binaryOf(op, std::move(a), std::move(b), e.pos);
    }

    TPtr binaryOf(Tok op, TPtr a, TPtr b, const Pos& p) {
        const Type ta = a->type, tb = b->type;
        auto two = [&](Op o, Type t, Type sub) {
            auto n = mk(o, t, p);
            n->sub = sub;
            n->kids.push_back(std::move(a));
            n->kids.push_back(std::move(b));
            return n;
        };
        auto mismatch = [&](const char* what) {
            return failNull(p, std::string("cannot ") + what + " a " + typeName(ta) + " and a " + typeName(tb));
        };
        switch (op) {
            case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: case Tok::EqEq: case Tok::Ne: {
                const Op o = op == Tok::Lt ? Op::Lt : op == Tok::Le ? Op::Le : op == Tok::Gt ? Op::Gt
                           : op == Tok::Ge ? Op::Ge : op == Tok::EqEq ? Op::Eq : Op::Ne;
                const bool order = o != Op::Eq && o != Op::Ne;
                if (ta == Type::String && tb == Type::String) return two(o, Type::Int, Type::String);
                Type u = unify(ta, tb);
                if (ta == Type::Int && tb == Type::Int) u = Type::Int;
                if (u == Type::Void && ta == tb && (isMatrix(ta) || isArray(ta)) && !order) u = ta;
                if (u == Type::Void || (order && !isScalar(u))) return mismatch("compare");
                a = convert(std::move(a), u, p);
                b = convert(std::move(b), u, p);
                return two(o, Type::Int, u);
            }
            case Tok::Amp: case Tok::Pipe: case Tok::Caret: case Tok::Shl: case Tok::Shr: {
                if (ta != Type::Int || tb != Type::Int) return mismatch("combine the bits of");
                const Op o = op == Tok::Amp ? Op::BitAnd : op == Tok::Pipe ? Op::BitOr : op == Tok::Caret ? Op::BitXor
                           : op == Tok::Shl ? Op::Shl : Op::Shr;
                return two(o, Type::Int, Type::Int);
            }
            case Tok::Plus:
                if (ta == Type::String && tb == Type::String) return two(Op::Add, Type::String, Type::String);
                [[fallthrough]];
            case Tok::Minus: case Tok::Star: case Tok::Slash: case Tok::Percent: {
                const Op o = op == Tok::Plus ? Op::Add : op == Tok::Minus ? Op::Sub : op == Tok::Star ? Op::Mul
                           : op == Tok::Slash ? Op::Div : Op::Mod;
                // Matrices.
                if (isMatrix(ta) || isMatrix(tb)) {
                    if (ta == tb && (o == Op::Add || o == Op::Sub)) return two(o, ta, ta);
                    if (ta == tb && o == Op::Mul) return two(Op::MatMat, ta, ta);
                    if (o == Op::Mul && isMatrix(tb) && ((ta == Type::Vec3) || (ta == Type::Vec4 && tb == Type::Mat4))) {
                        return two(Op::VecMat, ta, tb);
                    }
                    if (o == Op::Mul && isScalar(ta) != isScalar(tb) && (isScalar(ta) || isScalar(tb))) {
                        if (isScalar(ta)) std::swap(a, b);
                        const Type m = a->type;
                        b = convert(std::move(b), Type::Float, p);
                        auto n = mk(Op::MatScale, m, p);
                        n->kids.push_back(std::move(a));
                        n->kids.push_back(std::move(b));
                        return n;
                    }
                    return mismatch(o == Op::Mul ? "multiply" : "combine");
                }
                if (!isNumeric(ta) || !isNumeric(tb)) {
                    return mismatch(o == Op::Add ? "add" : o == Op::Sub ? "subtract" : o == Op::Mul ? "multiply" : "divide");
                }
                Type u = unify(ta, tb);
                if (ta == Type::Int && tb == Type::Int) u = o == Op::Div ? Type::Float : Type::Int;
                if (u == Type::Void) return mismatch("combine");
                // Constant operands fold.
                a = convert(std::move(a), u, p);
                b = convert(std::move(b), u, p);
                if (!a || !b) return nullptr;
                if (a->op == Op::Const && b->op == Op::Const && isScalar(u)) return fold(o, u, *a, *b, p);
                return two(o, u, u);
            }
            default:
                return failNull(p, "not an operator here");
        }
    }

    static TPtr fold(Op o, Type t, const TNode& a, const TNode& b, const Pos& p) {
        if (t == Type::Int) {
            const auto x = static_cast<uint32_t>(a.ci), y = static_cast<uint32_t>(b.ci);
            int32_t r = 0;
            switch (o) {
                case Op::Add: r = static_cast<int32_t>(x + y); break;
                case Op::Sub: r = static_cast<int32_t>(x - y); break;
                case Op::Mul: r = static_cast<int32_t>(x * y); break;
                case Op::Mod:
                    r = (b.ci == 0 || (a.ci == INT32_MIN && b.ci == -1)) ? 0 : a.ci % b.ci;
                    break;
                default: break;
            }
            return constInt(r, p);
        }
        float r = 0.0f;
        switch (o) {
            case Op::Add: r = a.cf + b.cf; break;
            case Op::Sub: r = a.cf - b.cf; break;
            case Op::Mul: r = a.cf * b.cf; break;
            case Op::Div: r = b.cf == 0.0f ? 0.0f : a.cf / b.cf; break;
            case Op::Mod: r = b.cf == 0.0f ? 0.0f : std::fmod(a.cf, b.cf); break;
            default: break;
        }
        return constFloat(r, p);
    }

    TPtr ternary(const Expr& e, Type hint) {
        TPtr c = truth(expr(*e.args[0], Type::Void), e.pos);
        TPtr a = expr(*e.args[1], hint);
        TPtr b = expr(*e.args[2], hint);
        if (!c || !a || !b) return nullptr;
        Type t = a->type == b->type ? a->type : unify(a->type, b->type);
        if (a->type == Type::Int && b->type == Type::Int) t = Type::Int;
        if (t == Type::Void) {
            return failNull(e.pos, std::string("the two sides of ?: are a ") + typeName(a->type) + " and a " +
                                       typeName(b->type));
        }
        a = convert(std::move(a), t, e.pos);
        b = convert(std::move(b), t, e.pos);
        if (!a || !b) return nullptr;
        auto n = mk(Op::Ternary, t, e.pos);
        n->kids.push_back(std::move(c));
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }

    // --- assignment ----------------------------------------------------------------------------

    /// Where `target` is, and its type. `inferred`: what a new attribute is
    /// when this is its first write.
    std::unique_ptr<LValue> lvalue(const Expr& target, Type& type, Type inferred) {
        auto lv = std::make_unique<LValue>();
        switch (target.kind) {
            case EK::Var: {
                const Var* v = lookup(target.text);
                if (!v) {
                    fail(target.pos, "unknown variable '" + target.text + "' -- declare it first: float " +
                                         target.text + " = ...;");
                    return nullptr;
                }
                lv->kind = LValue::K::Local;
                lv->slot = v->slot;
                lv->base = type = v->type;
                return lv;
            }
            case EK::Attr: {
                const int b = bind(target, true, inferred);
                if (b < 0) return nullptr;
                const Binding& bd = run_.bindings[static_cast<size_t>(b)];
                lv->kind = bd.kind == Binding::K::Group ? LValue::K::Group : LValue::K::Attr;
                lv->slot = b;
                lv->base = type = bd.type;
                return lv;
            }
            case EK::Member: {
                const Expr& base = *target.args[0];
                if (base.kind != EK::Var && base.kind != EK::Attr) {
                    fail(target.pos, "cannot assign to a component of this");
                    return nullptr;
                }
                // A component of a new attribute makes it a vector.
                Type baseType;
                auto inner = lvalue(base, baseType, Type::Vec3);
                if (!inner) return nullptr;
                const int c = component(target.text);
                if (!isVector(baseType) || c < 0 || c >= width(baseType)) {
                    fail(target.pos, std::string("a ") + typeName(baseType) + " has no component '" + target.text + "'");
                    return nullptr;
                }
                inner->comp = c;
                type = Type::Float;
                return inner;
            }
            case EK::Index: {
                const Expr& base = *target.args[0];
                if (base.kind != EK::Var && base.kind != EK::Attr) {
                    fail(target.pos, "cannot assign to an element of this");
                    return nullptr;
                }
                Type baseType;
                auto inner = lvalue(base, baseType, Type::Vec3);
                if (!inner) return nullptr;
                TPtr i = convert(expr(*target.args[1], Type::Int), Type::Int, target.args[1]->pos);
                if (!i) return nullptr;
                if (isArray(baseType)) {
                    if (inner->kind != LValue::K::Local) {
                        fail(target.pos, "attributes are not arrays");
                        return nullptr;
                    }
                    inner->index = std::move(i);
                    type = elementOf(baseType);
                    return inner;
                }
                if (isVector(baseType)) {
                    if (i->op == Op::Const) {
                        if (i->ci < 0 || i->ci >= width(baseType)) {
                            fail(target.pos, std::string("a ") + typeName(baseType) + " has no component " +
                                                 std::to_string(i->ci));
                            return nullptr;
                        }
                        inner->comp = i->ci;
                    } else {
                        inner->index = std::move(i);
                    }
                    type = Type::Float;
                    return inner;
                }
                fail(target.pos, std::string("an element of a ") + typeName(baseType) + " cannot be assigned");
                return nullptr;
            }
            default:
                fail(target.pos, "cannot assign to this");
                return nullptr;
        }
    }

    /// The target's value, read -- for +=, ++.
    TPtr readOf(const LValue& lv, Type type, const Pos& p) {
        auto n = mk(Op::Local, lv.base, p);
        if (lv.kind == LValue::K::Local) {
            n->op = Op::Local;
        } else {
            n->op = Op::Attr;
        }
        n->a = lv.slot;
        if (lv.comp >= 0) {
            auto c = mk(Op::Comp, Type::Float, p);
            c->sub = lv.base;
            c->a = lv.comp;
            c->kids.push_back(std::move(n));
            return c;
        }
        if (lv.index) {
            auto c = mk(Op::Index, type, p);
            c->sub = lv.base;
            c->kids.push_back(std::move(n));
            c->kids.push_back(clone(*lv.index));
            return c;
        }
        return n;
    }

    static TPtr clone(const TNode& n) {
        auto c = std::make_unique<TNode>();
        c->op = n.op;
        c->type = n.type;
        c->sub = n.sub;
        c->a = n.a;
        c->b = n.b;
        c->pos = n.pos;
        c->ci = n.ci;
        c->cf = n.cf;
        c->cv = n.cv;
        c->cs = n.cs;
        c->fn = n.fn;
        c->aux = n.aux;
        c->aux2 = n.aux2;
        for (const TPtr& k : n.kids) c->kids.push_back(clone(*k));
        if (n.lv) c->lv = cloneLv(*n.lv);
        for (const auto& r : n.refs) c->refs.push_back(r ? cloneLv(*r) : nullptr);
        return c;
    }

    static std::unique_ptr<LValue> cloneLv(const LValue& lv) {
        auto c = std::make_unique<LValue>();
        c->kind = lv.kind;
        c->slot = lv.slot;
        c->base = lv.base;
        c->comp = lv.comp;
        if (lv.index) c->index = clone(*lv.index);
        return c;
    }

    TPtr assign(const Expr& e) {
        const Expr& target = *e.args[0];
        const Expr& value = *e.args[1];
        Type type = Type::Void;
        std::unique_ptr<LValue> lv;
        TPtr rhs;
        if (e.op == Tok::Assign && needsInference(target)) {
            // A new attribute is what is first written into it.
            rhs = expr(value, Type::Void);
            if (!rhs) return nullptr;
            lv = lvalue(target, type, rhs->type);
        } else {
            lv = lvalue(target, type, Type::Void);
            if (!lv) return nullptr;
            rhs = expr(value, type);
        }
        if (!lv || !rhs) return nullptr;
        if (e.op != Tok::Assign) {
            const Tok op = e.op == Tok::PlusEq ? Tok::Plus : e.op == Tok::MinusEq ? Tok::Minus
                         : e.op == Tok::StarEq ? Tok::Star : e.op == Tok::SlashEq ? Tok::Slash : Tok::Percent;
            rhs = binaryOf(op, readOf(*lv, type, e.pos), std::move(rhs), e.pos);
            if (!rhs) return nullptr;
        }
        rhs = convert(std::move(rhs), type, e.pos);
        if (!rhs) return nullptr;
        auto n = mk(Op::Assign, type, e.pos);
        n->lv = std::move(lv);
        n->kids.push_back(std::move(rhs));
        return n;
    }

    TPtr incdec(const Expr& e) {
        Type type = Type::Void;
        auto lv = lvalue(*e.args[0], type, Type::Void);
        if (!lv) return nullptr;
        if (!isScalar(type)) return failNull(e.pos, std::string("++ and -- take an int or a float, not a ") + typeName(type));
        auto n = mk(Op::IncDec, type, e.pos);
        n->a = (e.kind == EK::PreInc || e.kind == EK::PostInc) ? 1 : -1;
        n->b = (e.kind == EK::PostInc || e.kind == EK::PostDec) ? 1 : 0;
        n->lv = std::move(lv);
        return n;
    }

    // --- calls ---------------------------------------------------------------------------------

    TPtr call(const Expr& e, Type hint);
    TPtr special(const Expr& e, Type hint, bool& handled);
    TPtr userCall(const Expr& e, int fn);
    TPtr channel(const Expr& e, Type t);
    TPtr attribRead(const Expr& e, Type hint, AttrClass cls);
    TPtr formatted(const Expr& e, Impl fn, Type ret);

    // --- statements ----------------------------------------------------------------------------

    TSPtr stmt(const Stmt& s);
    bool function(int index);

    const Ast& ast_;
    Run& run_;
    Geometry* geo_;
    bool fold_;
    Checked& out_;
    std::string error_;
    std::vector<std::map<std::string, Var>> scopes_;
    std::map<std::string, int> fnIndex_;
    std::map<std::string, int> bindIndex_;
    TFunction* current_ = nullptr;
    int loops_ = 0;

};

// --- calls ---------------------------------------------------------------------------------------

namespace {

bool accepts(const Overload& o, size_t n) {
    return o.params.size() == n || (o.variadic && !o.params.empty() && n + 1 >= o.params.size());
}

Type paramType(const Overload& o, size_t i) { return i < o.params.size() ? o.params[i] : o.params.back(); }

/// What passing a `from` as a `to` costs: 0 as it is, more for a
/// conversion, -1 if it cannot be passed. `braces`: {a, b ...} of `count`,
/// typed again as whatever it is passed as.
int cost(Type from, Type to, bool braces, size_t count) {
    if (braces) {
        if ((isVector(to) && static_cast<size_t>(width(to)) == count) || (to == Type::Mat3 && count == 9) ||
            (to == Type::Mat4 && count == 16) || isArray(to)) {
            return 0;
        }
        return from == to ? 0 : -1;
    }
    if (from == to) return 0;
    if (from == Type::Int && to == Type::Float) return 1;
    if (isScalar(from) && isVector(to)) return 4;
    if (from == Type::Float && to == Type::Int) return 10;
    if (isScalar(from) && isMatrix(to)) return 8;
    return -1;
}

bool attributeType(Type t) { return t == Type::Int || t == Type::Float || isVector(t) || t == Type::String; }

}  // namespace

TPtr Checker::call(const Expr& e, Type hint) {
    if (auto f = fnIndex_.find(e.text); f != fnIndex_.end()) return userCall(e, f->second);
    bool handled = false;
    TPtr s = special(e, hint, handled);
    if (handled) return s;
    const auto it = builtins().find(e.text);
    if (it == builtins().end()) return failNull(e.pos, "unknown function '" + e.text + "'");
    const std::vector<Overload>& overloads = it->second;
    const size_t n = e.args.size();

    std::vector<TPtr> args(n);
    std::vector<bool> braces(n);
    for (size_t i = 0; i < n; ++i) {
        // The type every overload agrees on, if one: {1, 0, 0} knows what to be.
        Type common = Type::Void;
        bool agree = true;
        for (const Overload& o : overloads) {
            if (!accepts(o, n)) continue;
            const Type pt = paramType(o, i);
            if (common == Type::Void) common = pt;
            else if (common != pt) agree = false;
        }
        braces[i] = e.args[i]->kind == EK::Braces;
        args[i] = expr(*e.args[i], agree ? common : Type::Void);
        if (!args[i]) return nullptr;
    }

    int best = -1, bestCost = 1 << 30;
    bool bestRet = false;
    for (size_t k = 0; k < overloads.size(); ++k) {
        const Overload& o = overloads[k];
        if (!accepts(o, n)) continue;
        int total = 0;
        bool ok = true;
        for (size_t i = 0; i < n && ok; ++i) {
            const int c = cost(args[i]->type, paramType(o, i), braces[i], e.args[i]->args.size());
            if (c < 0) ok = false;
            total += c;
        }
        if (!ok) continue;
        const bool ret = hint != Type::Void && o.ret == hint;
        if (total < bestCost || (total == bestCost && ret && !bestRet)) {
            best = static_cast<int>(k);
            bestCost = total;
            bestRet = ret;
        }
    }
    if (best < 0) {
        std::string given;
        for (size_t i = 0; i < n; ++i) given += (i ? ", " : "") + std::string(typeName(args[i]->type));
        std::string takes;
        int shown = 0;
        for (const Overload& o : overloads) {
            if (!accepts(o, n) || shown == 4) continue;
            std::string sig;
            for (size_t i = 0; i < o.params.size(); ++i) sig += (i ? ", " : "") + std::string(typeName(o.params[i]));
            takes += std::string(shown++ ? "; " : "") + "(" + sig + (o.variadic ? ", ..." : "") + ")";
        }
        return failNull(e.pos, e.text + "() does not take (" + given + ")" + (takes.empty() ? "" : " -- it takes " + takes));
    }
    const Overload& o = overloads[static_cast<size_t>(best)];
    auto node = mk(Op::Builtin, o.ret, e.pos);
    node->fn = o.fn;
    for (size_t i = 0; i < n; ++i) {
        const Type pt = paramType(o, i);
        if (braces[i] && args[i]->type != pt) args[i] = expr(*e.args[i], pt);
        if (!args[i]) return nullptr;
        TPtr a = convert(std::move(args[i]), pt, e.args[i]->pos);
        if (!a) return nullptr;
        node->kids.push_back(std::move(a));
    }
    if (o.refs > 0) {
        const Expr& target = *e.args[0];
        if (target.kind != EK::Var) {
            return failNull(target.pos, e.text + "() changes its first argument: it must be a variable");
        }
        Type t;
        node->lv = lvalue(target, t, Type::Void);
        if (!node->lv) return nullptr;
    }
    if (o.sideEffect) {
        if (!geo_) return failNull(e.pos, e.text + "() changes geometry: it is for wrangles, not expressions");
        out_.ordered = true;
    }
    return node;
}

TPtr Checker::special(const Expr& e, Type hint, bool& handled) {
    static const std::map<std::string, Type, std::less<>> channels = {
        {"ch", Type::Float}, {"chf", Type::Float}, {"chi", Type::Int}, {"chv", Type::Vec3},
        {"chs", Type::String}, {"chu", Type::Vec2}, {"chp", Type::Vec4}};
    const std::string& name = e.text;
    handled = true;
    if (auto c = channels.find(name); c != channels.end()) return channel(e, c->second);
    if (name == "set") {
        if (e.args.size() == 1) {
            TPtr a = expr(*e.args[0], hint);
            if (!a) return nullptr;
            const Type to = (isVector(hint) || isMatrix(hint)) ? hint : a->type;
            return convert(std::move(a), to, e.pos, true);
        }
        const size_t n = e.args.size();
        const Type t = n == 2 ? Type::Vec2 : n == 3 ? Type::Vec3 : n == 4 ? Type::Vec4 : n == 9 ? Type::Mat3
                     : n == 16 ? Type::Mat4 : Type::Void;
        if (t == Type::Void) return failNull(e.pos, "set() takes 2, 3 or 4 numbers (a vector), 9 or 16 (a matrix)");
        return make(e, t);
    }
    if (name == "array") {
        Type t = isArray(hint) ? hint : Type::Void;
        if (t == Type::Void) {
            if (e.args.empty()) return failNull(e.pos, "array() of what? Declare it: int list[] = array();");
            Type elem = Type::Void;
            for (const auto& a : e.args) {
                TPtr k = expr(*a, Type::Void);
                if (!k) return nullptr;
                elem = elem == Type::Void ? k->type : (elem == k->type ? elem : unify(elem, k->type));
                if (elem == Type::Void) return failNull(a->pos, "array() of values of different types");
            }
            t = arrayOf(elem);
            if (t == Type::Void) return failNull(e.pos, std::string("no arrays of ") + typeName(elem));
        }
        return make(e, t);
    }
    if (name == "point") return attribRead(e, hint, AttrClass::Point);
    if (name == "prim") return attribRead(e, hint, AttrClass::Primitive);
    if (name == "vertex") return attribRead(e, hint, AttrClass::Vertex);
    if (name == "detail") return attribRead(e, hint, AttrClass::Detail);
    if (name == "printf") return formatted(e, implFormat(0), Type::Void);
    if (name == "sprintf") return formatted(e, implFormat(1), Type::String);
    if (name == "warning") return formatted(e, implFormat(2), Type::Void);
    if (name == "error") return formatted(e, implFormat(3), Type::Void);
    handled = false;
    return nullptr;
}

TPtr Checker::channel(const Expr& e, Type t) {
    TPtr path = convert(expr(*e.args[0], Type::String), Type::String, e.args[0]->pos);
    if (!path) return nullptr;
    if (fold_ && path->op == Op::Const) {
        const std::string& p = path->cs;
        std::string why;
        if (t == Type::String) {
            std::string text;
            if (!run_.host || !run_.host->channelText(p, text, why)) {
                out_.warnings.push_back(e.text + "(\"" + p + "\"): " + (why.empty() ? "no such parameter" : why));
            }
            return constStr(text, e.pos);
        }
        Vec4 v;
        const int comps = isScalar(t) ? 1 : width(t);
        for (int c = 0; c < comps; ++c) {
            double x = 0.0;
            if (!run_.host || !run_.host->channel(p, c, x, why)) {
                out_.warnings.push_back(e.text + "(\"" + p + "\"): " + (why.empty() ? "no such parameter" : why));
                break;
            }
            (&v.x)[c] = static_cast<float>(x);
        }
        if (t == Type::Int) return constInt(static_cast<int32_t>(std::lround(v.x)), e.pos);
        if (t == Type::Float) return constFloat(v.x, e.pos);
        return constVec(t, v, e.pos);
    }
    auto n = mk(Op::Builtin, t, e.pos);
    n->fn = implChannel(t);
    n->kids.push_back(std::move(path));
    return n;
}

TPtr Checker::attribRead(const Expr& e, Type hint, AttrClass cls) {
    if (!geo_) return failNull(e.pos, e.text + "() reads geometry: it is for wrangles, not expressions");
    TPtr in = convert(expr(*e.args[0], Type::Int), Type::Int, e.args[0]->pos);
    TPtr name = convert(expr(*e.args[1], Type::String), Type::String, e.args[1]->pos);
    if (!in || !name) return nullptr;
    std::vector<TPtr> indices;
    for (size_t i = 2; i < e.args.size(); ++i) {
        TPtr k = convert(expr(*e.args[i], Type::Int), Type::Int, e.args[i]->pos);
        if (!k) return nullptr;
        indices.push_back(std::move(k));
    }
    if (cls == AttrClass::Detail && !indices.empty()) return failNull(e.pos, "detail() takes an input and a name");
    if (cls != AttrClass::Detail && indices.empty()) return failNull(e.pos, e.text + "() takes an input, a name and an element");
    const Type fallback = attributeType(hint) ? hint : Type::Float;
    if (in->op == Op::Const && name->op == Op::Const) {
        const int k = in->ci;
        if (k < 0 || k > 3) return failNull(e.args[0]->pos, "there are inputs 0 to 3");
        const Geometry* g = run_.inputs[static_cast<size_t>(k)];
        const AttributeArray* arr = g ? g->attributes(cls).find(name->cs) : nullptr;
        // P of the points of the vertices: vertex(0, "P", ...) reads the point's.
        if (!arr && g && cls == AttrClass::Vertex) arr = g->points().find(name->cs);
        const Type t = arr ? fromAttr(arr->type()) : fallback;
        auto n = mk(Op::Builtin, t, e.pos);
        n->fn = implAttribRead(t, true);
        n->aux = arr;
        n->aux2 = g;
        n->a = static_cast<int>(cls);
        n->b = arr && cls == AttrClass::Vertex && !g->vertices().find(name->cs) ? 1 : 0;  // through the point
        for (TPtr& i : indices) n->kids.push_back(std::move(i));
        return n;
    }
    auto n = mk(Op::Builtin, fallback, e.pos);
    n->fn = implAttribRead(fallback, false);
    n->a = static_cast<int>(cls);
    n->kids.push_back(std::move(in));
    n->kids.push_back(std::move(name));
    for (TPtr& i : indices) n->kids.push_back(std::move(i));
    return n;
}

TPtr Checker::formatted(const Expr& e, Impl fn, Type ret) {
    if (!geo_ && ret != Type::String) return failNull(e.pos, e.text + "() is for wrangles, not expressions");
    auto n = mk(Op::Builtin, ret, e.pos);
    n->fn = fn;
    for (size_t i = 0; i < e.args.size(); ++i) {
        TPtr a = expr(*e.args[i], Type::Void);
        if (!a) return nullptr;
        if (i == 0) {
            a = convert(std::move(a), Type::String, e.args[0]->pos);
            if (!a) return nullptr;
        }
        n->kids.push_back(std::move(a));
    }
    return n;
}

TPtr Checker::userCall(const Expr& e, int fi) {
    if (!function(fi)) return nullptr;
    const TFunction& f = out_.functions[static_cast<size_t>(fi)];
    if (e.args.size() != f.paramTypes.size()) {
        return failNull(e.pos, e.text + "() takes " + std::to_string(f.paramTypes.size()) + " argument(s), got " +
                                   std::to_string(e.args.size()));
    }
    auto n = mk(Op::Call, f.ret, e.pos);
    n->a = fi;
    for (size_t i = 0; i < e.args.size(); ++i) {
        const Type pt = f.paramTypes[i];
        TPtr a = expr(*e.args[i], pt);
        if (!a) return nullptr;
        // A variable goes by reference: what the function does to it stays.
        std::unique_ptr<LValue> ref;
        if (e.args[i]->kind == EK::Var && a->type == pt) {
            Type t;
            ref = lvalue(*e.args[i], t, Type::Void);
            if (!ref) return nullptr;
        }
        a = convert(std::move(a), pt, e.args[i]->pos);
        if (!a) return nullptr;
        // Every argument first into a place of its own: f(f(1)) must not
        // overwrite the parameters it is filling.
        auto tmp = mk(Op::Assign, pt, e.args[i]->pos);
        tmp->lv = std::make_unique<LValue>();
        tmp->lv->kind = LValue::K::Local;
        tmp->lv->slot = slot(pt);
        tmp->lv->base = pt;
        tmp->kids.push_back(std::move(a));
        n->kids.push_back(std::move(tmp));
        n->refs.push_back(std::move(ref));
    }
    return n;
}

// --- statements ----------------------------------------------------------------------------------

bool Checker::function(int index) {
    TFunction& tf = out_.functions[static_cast<size_t>(index)];
    const Function& f = ast_.functions[static_cast<size_t>(index)];
    if (tf.checked) return true;
    if (tf.checking) return fail(f.pos, f.name + "() calls itself: functions cannot recurse");
    tf.checking = true;
    auto savedScopes = std::move(scopes_);
    TFunction* savedCurrent = current_;
    const int savedLoops = loops_;
    scopes_.clear();
    scopes_.emplace_back();
    current_ = &tf;
    loops_ = 0;
    tf.params.clear();
    tf.paramTypes.clear();
    bool ok = true;
    for (const Param& p : f.params) {
        const int s = slot(p.type);
        if (!declare(p.name, p.type, s, p.pos)) ok = false;
        tf.params.push_back(s);
        tf.paramTypes.push_back(p.type);
    }
    if (ok && f.ret != Type::Void) tf.result = slot(f.ret);
    if (ok) {
        tf.body = stmt(*f.body);
        ok = tf.body != nullptr;
    }
    scopes_ = std::move(savedScopes);
    current_ = savedCurrent;
    loops_ = savedLoops;
    tf.checking = false;
    tf.checked = ok;
    return ok;
}

TSPtr Checker::stmt(const Stmt& s) {
    auto t = std::make_unique<TStmt>();
    switch (s.kind) {
        case SK::Empty:
            t->kind = TS::Block;
            return t;
        case SK::Expr:
            t->kind = TS::Expr;
            t->expr = expr(*s.expr, Type::Void);
            return t->expr ? std::move(t) : nullptr;
        case SK::Decl: {
            t->kind = TS::Block;
            for (const Declarator& d : s.decls) {
                Type type = s.declType;
                if (d.array) {
                    type = arrayOf(type);
                    if (type == Type::Void) {
                        fail(d.pos, std::string("no arrays of ") + typeName(s.declType));
                        return nullptr;
                    }
                }
                auto decl = std::make_unique<TStmt>();
                decl->kind = TS::Decl;
                decl->type = type;
                if (d.init) {
                    decl->expr = convert(expr(*d.init, type), type, d.init->pos);
                    if (!decl->expr) return nullptr;
                }
                decl->slot = slot(type);
                if (!declare(d.name, type, decl->slot, d.pos)) return nullptr;
                t->body.push_back(std::move(decl));
            }
            return t;
        }
        case SK::Block: {
            t->kind = TS::Block;
            scopes_.emplace_back();
            for (const StmtPtr& b : s.body) {
                TSPtr inner = stmt(*b);
                if (!inner) return nullptr;
                t->body.push_back(std::move(inner));
            }
            scopes_.pop_back();
            return t;
        }
        case SK::If: {
            t->kind = TS::If;
            t->expr = truth(expr(*s.expr, Type::Void), s.pos);
            if (!t->expr) return nullptr;
            for (const StmtPtr& b : s.body) {
                scopes_.emplace_back();
                TSPtr inner = stmt(*b);
                scopes_.pop_back();
                if (!inner) return nullptr;
                t->body.push_back(std::move(inner));
            }
            return t;
        }
        case SK::While:
        case SK::DoWhile: {
            t->kind = s.kind == SK::While ? TS::While : TS::DoWhile;
            t->expr = truth(expr(*s.expr, Type::Void), s.pos);
            if (!t->expr) return nullptr;
            ++loops_;
            scopes_.emplace_back();
            TSPtr body = stmt(*s.body[0]);
            scopes_.pop_back();
            --loops_;
            if (!body) return nullptr;
            t->body.push_back(std::move(body));
            return t;
        }
        case SK::For: {
            t->kind = TS::For;
            scopes_.emplace_back();
            TSPtr init = stmt(*s.body[1]);
            if (!init) return nullptr;
            if (s.expr) {
                t->expr = truth(expr(*s.expr, Type::Void), s.pos);
                if (!t->expr) return nullptr;
            }
            if (s.step) {
                t->step = expr(*s.step, Type::Void);
                if (!t->step) return nullptr;
            }
            ++loops_;
            scopes_.emplace_back();
            TSPtr body = stmt(*s.body[0]);
            scopes_.pop_back();
            --loops_;
            scopes_.pop_back();
            if (!body) return nullptr;
            t->body.push_back(std::move(init));
            t->body.push_back(std::move(body));
            return t;
        }
        case SK::Foreach: {
            t->kind = TS::Foreach;
            TPtr arr = expr(*s.expr, arrayOf(s.valueType));
            if (!arr) return nullptr;
            if (!isArray(arr->type)) {
                fail(s.expr->pos, std::string("foreach goes over an array, not a ") + typeName(arr->type));
                return nullptr;
            }
            const Type elem = elementOf(arr->type);
            if (s.valueType != elem) {
                fail(s.pos, std::string("foreach over a ") + typeName(arr->type) + " takes a " + typeName(elem) +
                                ", not a " + typeName(s.valueType));
                return nullptr;
            }
            if (!s.indexName.empty() && s.indexType != Type::Int) {
                fail(s.pos, "foreach's index is an int");
                return nullptr;
            }
            t->expr = std::move(arr);
            t->type = elem;
            scopes_.emplace_back();
            if (!s.indexName.empty()) {
                t->slot2 = slot(Type::Int);
                if (!declare(s.indexName, Type::Int, t->slot2, s.pos)) return nullptr;
            }
            t->slot = slot(elem);
            if (!declare(s.valueName, elem, t->slot, s.pos)) return nullptr;
            ++loops_;
            TSPtr body = stmt(*s.body[0]);
            --loops_;
            scopes_.pop_back();
            if (!body) return nullptr;
            t->body.push_back(std::move(body));
            return t;
        }
        case SK::Break:
        case SK::Continue:
            if (loops_ == 0) {
                fail(s.pos, std::string(s.kind == SK::Break ? "break" : "continue") + " outside a loop");
                return nullptr;
            }
            t->kind = s.kind == SK::Break ? TS::Break : TS::Continue;
            return t;
        case SK::Return: {
            t->kind = TS::Return;
            if (current_) {
                t->type = current_->ret;
                t->slot = current_->result;
                if (current_->ret == Type::Void) {
                    if (s.expr) {
                        fail(s.pos, current_->name + "() is void: it returns nothing");
                        return nullptr;
                    }
                } else {
                    if (!s.expr) {
                        fail(s.pos, current_->name + "() returns a " + typeName(current_->ret));
                        return nullptr;
                    }
                    t->expr = convert(expr(*s.expr, current_->ret), current_->ret, s.pos);
                    if (!t->expr) return nullptr;
                }
            } else if (s.expr) {
                fail(s.pos, "return in the program takes no value: 'return;' ends the run for this element");
                return nullptr;
            }
            return t;
        }
    }
    return nullptr;
}

// --- the entry points ------------------------------------------------------------------------------

bool check(const Ast& ast, Run& run, Geometry* geo, bool expression, bool fold, Checked& out, std::string& error) {
    Checker c(ast, run, geo, fold, out);
    const bool ok = expression ? c.expression() : c.program();
    if (!ok) {
        error = c.error().empty() ? "does not type" : c.error();
        return false;
    }
    return true;
}

bool bindAll(Run& run, Geometry& geo, const Checked& checked, std::string& error) {
    using K = Binding::K;
    // Made first: making one can move no other, but a pointer taken before
    // a later create() would be one into a map that is still growing.
    for (Binding& b : run.bindings) {
        if (b.input >= 0) continue;
        if ((b.kind == K::Attr || b.kind == K::Missing) && b.written) {
            AttributeSet& set = geo.attributes(b.cls);
            if (!set.find(b.name)) {
                AttrType at;
                if (!toAttr(b.type, at)) {
                    error = "@" + b.name + ": no attributes of " + typeName(b.type);
                    return false;
                }
                AttributeArray& made = set.create(b.name, at);
                // A new string's elements are 0: "" before what is written.
                if (at == AttrType::String) made.internString("");
            }
            b.kind = K::Attr;
        }
        if (b.kind == K::Group && b.written) geo.createGroup(b.name, run.cls);
    }
    for (Binding& b : run.bindings) {
        if (b.input >= 0) {
            // Another input: bound as bind() found it there.
            const Geometry* g = run.inputs[static_cast<size_t>(b.input)];
            const AttributeArray* arr = g && b.kind == K::Attr ? g->attributes(b.cls).find(b.name) : nullptr;
            if (arr && fromAttr(arr->type()) == b.type) {
                b.read = arr->rawRead();
                b.readArray = arr;
                b.count = arr->size();
            } else {
                b.kind = K::Missing;
            }
            continue;
        }
        if (b.kind == K::Attr) {
            AttributeArray* arr = geo.attributes(b.cls).find(b.name);
            if (!arr) {
                b.kind = K::Missing;
                continue;
            }
            b.count = arr->size();
            if (b.written) {
                b.write = arr->rawWrite();
                b.read = b.write;
                b.array = arr;
                b.readArray = arr;
                if (b.type == Type::String) {
                    b.strings = std::make_shared<std::unordered_map<std::string, int32_t>>();
                    const auto table = arr->strings();
                    for (size_t i = table.size(); i-- > 0;) (*b.strings)[table[i]] = static_cast<int32_t>(i);
                }
            } else {
                b.read = arr->rawRead();
                b.readArray = arr;
            }
        } else if (b.kind == K::Missing) {
            // Read and never written: maybe there after all (a class it maps to).
            continue;
        } else if (b.kind == K::Group) {
            Group* g = geo.findGroup(b.name);
            if (g && g->classOf() == run.cls) {
                if (b.written) {
                    g->resize(geo.elementCount(run.cls));
                    b.mask = g->writableMask();
                    b.readMask = b.mask;
                } else {
                    b.readMask = g->mask().data();
                }
                b.count = g->size();
            }
        }
    }
    if (checked.needVertexPrim) {
        run.vertexPrim.assign(geo.vertexCount(), 0);
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            const size_t start = geo.primitiveVertexStart(p), n = geo.primitiveVertexCount(p);
            for (size_t v = start; v < start + n; ++v) run.vertexPrim[v] = static_cast<uint32_t>(p);
        }
    }
    if (checked.needCentroids) {
        const auto P = geo.positions();
        run.centroids.assign(geo.primitiveCount(), Vec3());
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            const auto pts = geo.primitivePoints(p);
            Vec3 c;
            for (uint32_t i : pts) c += P[i];
            if (!pts.empty()) c = c * (1.0f / static_cast<float>(pts.size()));
            run.centroids[p] = c;
        }
    }
    return true;
}

}  // namespace pg::lang
