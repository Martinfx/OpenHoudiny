// The interpreter: a typed program (Check.cpp) run element after element.
#include "pg/lang/Check.h"
#include "pg/lang/Math.h"
#include "pg/lang/Query.h"

#include "pg/core/Parallel.h"

#include <cstdio>
#include <cstdlib>

namespace pg::lang {

Env::Env(const Run& r) : run(&r) {
    auto n = [&](Type t) { return static_cast<size_t>(r.slotCounts[static_cast<size_t>(t)]); };
    i.resize(n(Type::Int));
    f.resize(n(Type::Float));
    v2.resize(n(Type::Vec2));
    v3.resize(n(Type::Vec3));
    v4.resize(n(Type::Vec4));
    m3.resize(n(Type::Mat3));
    m4.resize(n(Type::Mat4));
    s.resize(n(Type::String));
    ia.resize(n(Type::IntArray));
    fa.resize(n(Type::FloatArray));
    v2a.resize(n(Type::Vec2Array));
    v3a.resize(n(Type::Vec3Array));
    v4a.resize(n(Type::Vec4Array));
    sa.resize(n(Type::StringArray));
}

namespace {

// --- bindings --------------------------------------------------------------------------------

inline size_t mapped(const Binding& b, const Env& e) {
    switch (b.map) {
        case Binding::Map::Same: return e.elem;
        case Binding::Map::VertexPoint: return e.run->geo->vertexPoint(e.elem);
        case Binding::Map::VertexPrim: return e.elem < e.run->vertexPrim.size() ? e.run->vertexPrim[e.elem] : 0;
        case Binding::Map::Detail: return 0;
    }
    return e.elem;
}

template <class T> T readBinding(const Binding& b, Env& e) {
    using K = Binding::K;
    const Run& r = *e.run;
    switch (b.kind) {
        case K::Attr: {
            const size_t idx = mapped(b, e);
            if (idx >= b.count || !b.read) return T{};
            if constexpr (std::is_same_v<T, std::string>) {
                return b.readArray->stringValue(reinterpret_cast<const int32_t*>(b.read)[idx]);
            } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float> || std::is_same_v<T, Vec2> ||
                                 std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>) {
                return reinterpret_cast<const T*>(b.read)[idx];
            }
            return T{};
        }
        case K::Missing: return T{};
        default: break;
    }
    if constexpr (std::is_same_v<T, int32_t>) {
        const auto elem = static_cast<int32_t>(e.elem);
        switch (b.kind) {
            case K::Group:
                return b.readMask && e.elem < b.count ? (b.readMask[e.elem] ? 1 : 0) : 0;
            case K::ElemNum: return elem;
            case K::PtNum:
                return r.cls == AttrClass::Point ? elem : r.cls == AttrClass::Vertex ? static_cast<int32_t>(r.geo->vertexPoint(e.elem))
                     : r.cls == AttrClass::Detail ? 0 : -1;
            case K::PrimNum:
                return r.cls == AttrClass::Primitive ? elem
                     : r.cls == AttrClass::Vertex ? static_cast<int32_t>(e.elem < r.vertexPrim.size() ? r.vertexPrim[e.elem] : 0)
                     : r.cls == AttrClass::Detail ? 0 : -1;
            case K::VtxNum: return r.cls == AttrClass::Vertex ? elem : r.cls == AttrClass::Detail ? 0 : -1;
            case K::NumElem: return static_cast<int32_t>(r.count);
            case K::NumPt: return static_cast<int32_t>(r.geo->pointCount());
            case K::NumPrim: return static_cast<int32_t>(r.geo->primitiveCount());
            case K::NumVtx: return static_cast<int32_t>(r.geo->vertexCount());
            default: return 0;
        }
    } else if constexpr (std::is_same_v<T, float>) {
        switch (b.kind) {
            case K::Time: return static_cast<float>(r.ctx.time);
            case K::Frame: return static_cast<float>(r.ctx.frame);
            case K::TimeInc: return r.ctx.fps > 0.0 ? static_cast<float>(1.0 / r.ctx.fps) : 0.0f;
            default: return 0.0f;
        }
    } else if constexpr (std::is_same_v<T, Vec3>) {
        if (b.kind == K::Centroid) return e.elem < r.centroids.size() ? r.centroids[e.elem] : Vec3();
    }
    return T{};
}

int32_t intern(const Binding& b, const std::string& v) {
    if (b.strings) {
        const auto it = b.strings->find(v);
        if (it != b.strings->end()) return it->second;
        const int32_t id = b.array->addString(v);
        (*b.strings)[v] = id;
        return id;
    }
    return b.array->internString(v);
}

template <class T> void writeBinding(const Binding& b, Env& e, const T& v) {
    using K = Binding::K;
    const size_t idx = e.elem;
    if (b.kind == K::Group) {
        if constexpr (std::is_same_v<T, int32_t>) {
            if (b.mask && idx < b.count) b.mask[idx] = v != 0 ? 1 : 0;
        }
        return;
    }
    if (b.kind != K::Attr || !b.write || idx >= b.count) return;
    if constexpr (std::is_same_v<T, std::string>) {
        reinterpret_cast<int32_t*>(b.write)[idx] = intern(b, v);
    } else if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float> || std::is_same_v<T, Vec2> ||
                         std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>) {
        reinterpret_cast<T*>(b.write)[idx] = v;
    }
}

// --- constants, casts, construction ----------------------------------------------------------------

template <class T> T constOf(const TNode& n) {
    if constexpr (std::is_same_v<T, int32_t>) return n.ci;
    else if constexpr (std::is_same_v<T, float>) return n.cf;
    else if constexpr (std::is_same_v<T, Vec2>) return Vec2(n.cv.x, n.cv.y);
    else if constexpr (std::is_same_v<T, Vec3>) return Vec3(n.cv.x, n.cv.y, n.cv.z);
    else if constexpr (std::is_same_v<T, Vec4>) return n.cv;
    else if constexpr (std::is_same_v<T, std::string>) return n.cs;
    else return T{};
}

template <class V> std::string vecText(const V& v) {
    std::string out = "{";
    for (int i = 0; i < widthOf<V>(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(comp(v, i)));
        out += (i ? ", " : "") + std::string(buf);
    }
    return out + "}";
}

template <class To, class V> To widenVec(const V& v) {
    To r{};
    for (int i = 0; i < std::min(widthOf<To>(), widthOf<V>()); ++i) compRef(r, i) = comp(v, i);
    return r;
}

template <class T> T castOf(const TNode& n, Env& e) {
    const TNode& k = *n.kids[0];
    const Type from = n.sub;
    if constexpr (std::is_same_v<T, int32_t>) {
        if (from == Type::Float) return toInt(ev<float>(k, e));
        if (from == Type::String) return static_cast<int32_t>(std::strtol(ev<std::string>(k, e).c_str(), nullptr, 10));
    } else if constexpr (std::is_same_v<T, float>) {
        if (from == Type::Int) return static_cast<float>(ev<int32_t>(k, e));
        if (from == Type::String) return static_cast<float>(std::strtod(ev<std::string>(k, e).c_str(), nullptr));
    } else if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>) {
        switch (from) {
            case Type::Int: { const auto s = static_cast<float>(ev<int32_t>(k, e)); return mapv(T{}, [s](float) { return s; }); }
            case Type::Float: { const float s = ev<float>(k, e); return mapv(T{}, [s](float) { return s; }); }
            case Type::Vec2: return widenVec<T>(ev<Vec2>(k, e));
            case Type::Vec3: return widenVec<T>(ev<Vec3>(k, e));
            case Type::Vec4: return widenVec<T>(ev<Vec4>(k, e));
            default: break;
        }
    } else if constexpr (std::is_same_v<T, Mat3>) {
        switch (from) {
            case Type::Int: return diag3(static_cast<float>(ev<int32_t>(k, e)));
            case Type::Float: return diag3(ev<float>(k, e));
            case Type::Mat4: return upper(ev<Mat4>(k, e));
            case Type::Vec4: return quatToMat3(ev<Vec4>(k, e));
            default: break;
        }
    } else if constexpr (std::is_same_v<T, Mat4>) {
        switch (from) {
            case Type::Int: return diag4(static_cast<float>(ev<int32_t>(k, e)));
            case Type::Float: return diag4(ev<float>(k, e));
            case Type::Mat3: return widen(ev<Mat3>(k, e));
            default: break;
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        char buf[64];
        switch (from) {
            case Type::Int: return std::to_string(ev<int32_t>(k, e));
            case Type::Float: std::snprintf(buf, sizeof buf, "%g", static_cast<double>(ev<float>(k, e))); return buf;
            case Type::Vec2: return vecText(ev<Vec2>(k, e));
            case Type::Vec3: return vecText(ev<Vec3>(k, e));
            case Type::Vec4: return vecText(ev<Vec4>(k, e));
            default: break;
        }
    }
    return T{};
}

template <class T> T makeOf(const TNode& n, Env& e) {
    if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>) {
        T r{};
        for (int i = 0; i < widthOf<T>() && i < static_cast<int>(n.kids.size()); ++i) {
            compRef(r, i) = ev<float>(*n.kids[static_cast<size_t>(i)], e);
        }
        return r;
    } else if constexpr (std::is_same_v<T, Mat3>) {
        Mat3 r;  // VEX's rows, GLM's columns
        for (size_t i = 0; i < 9 && i < n.kids.size(); ++i) r[static_cast<int>(i / 3)][static_cast<int>(i % 3)] = ev<float>(*n.kids[i], e);
        return r;
    } else if constexpr (std::is_same_v<T, Mat4>) {
        Mat4 r;
        for (size_t i = 0; i < 16 && i < n.kids.size(); ++i) r[static_cast<int>(i / 4)][static_cast<int>(i % 4)] = ev<float>(*n.kids[i], e);
        return r;
    } else if constexpr (IsArray<T>::value) {
        T r;
        r.reserve(n.kids.size());
        for (const TPtr& k : n.kids) r.push_back(ev<typename T::value_type>(*k, e));
        return r;
    }
    return T{};
}

// --- indexing --------------------------------------------------------------------------------------

/// An index counting from the end when negative, as VEX's; -1 when outside.
inline int64_t wrapIndex(int32_t i, size_t size) {
    int64_t k = i;
    if (k < 0) k += static_cast<int64_t>(size);
    return k >= 0 && k < static_cast<int64_t>(size) ? k : -1;
}

template <class T> T indexOf(const TNode& n, Env& e) {
    const TNode& base = *n.kids[0];
    if constexpr (std::is_same_v<T, float>) {
        if (isVector(n.sub)) {
            const int32_t i = ev<int32_t>(*n.kids[1], e);
            switch (n.sub) {
                case Type::Vec2: { const Vec2 v = ev<Vec2>(base, e); return i >= 0 && i < 2 ? comp(v, i) : 0.0f; }
                case Type::Vec3: { const Vec3 v = ev<Vec3>(base, e); return i >= 0 && i < 3 ? v[i] : 0.0f; }
                case Type::Vec4: { const Vec4 v = ev<Vec4>(base, e); return i >= 0 && i < 4 ? comp(v, i) : 0.0f; }
                default: return 0.0f;
            }
        }
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (n.sub == Type::String) {
            std::string scratch;
            const std::string& s = evRef<std::string>(base, e, scratch);
            const int64_t k = wrapIndex(ev<int32_t>(*n.kids[1], e), s.size());
            return k < 0 ? std::string() : std::string(1, s[static_cast<size_t>(k)]);
        }
    }
    if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float> || std::is_same_v<T, Vec2> ||
                  std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4> || std::is_same_v<T, std::string>) {
        using A = std::vector<T>;
        A scratch;
        const A& a = evRef<A>(base, e, scratch);
        const int64_t k = wrapIndex(ev<int32_t>(*n.kids[1], e), a.size());
        return k < 0 ? T{} : a[static_cast<size_t>(k)];
    }
    return T{};
}

// --- comparison ----------------------------------------------------------------------------------

template <class T> int32_t compareAs(Op op, const TNode& a, const TNode& b, Env& e) {
    T sa{}, sb{};
    const T& x = evRef<T>(a, e, sa);
    const T& y = evRef<T>(b, e, sb);
    if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float> || std::is_same_v<T, std::string>) {
        switch (op) {
            case Op::Lt: return x < y;
            case Op::Le: return x <= y;
            case Op::Gt: return x > y;
            case Op::Ge: return x >= y;
            default: break;
        }
    }
    const bool eq = opEq(x, y);
    return op == Op::Ne ? !eq : eq;
}

int32_t compare(const TNode& n, Env& e) {
    const TNode& a = *n.kids[0];
    const TNode& b = *n.kids[1];
    switch (n.sub) {
        case Type::Int: return compareAs<int32_t>(n.op, a, b, e);
        case Type::Float: return compareAs<float>(n.op, a, b, e);
        case Type::Vec2: return compareAs<Vec2>(n.op, a, b, e);
        case Type::Vec3: return compareAs<Vec3>(n.op, a, b, e);
        case Type::Vec4: return compareAs<Vec4>(n.op, a, b, e);
        case Type::Mat3: return compareAs<Mat3>(n.op, a, b, e);
        case Type::Mat4: return compareAs<Mat4>(n.op, a, b, e);
        case Type::String: return compareAs<std::string>(n.op, a, b, e);
        case Type::IntArray: return compareAs<IntArr>(n.op, a, b, e);
        case Type::FloatArray: return compareAs<FloatArr>(n.op, a, b, e);
        case Type::Vec2Array: return compareAs<Vec2Arr>(n.op, a, b, e);
        case Type::Vec3Array: return compareAs<Vec3Arr>(n.op, a, b, e);
        case Type::Vec4Array: return compareAs<Vec4Arr>(n.op, a, b, e);
        case Type::StringArray: return compareAs<StrArr>(n.op, a, b, e);
        default: return 0;
    }
}

// --- lvalues ---------------------------------------------------------------------------------------

template <class V> void setComponent(V& v, int32_t i, float x) {
    if (i >= 0 && i < widthOf<V>()) compRef(v, i) = x;
}

template <class V> float getComponent(const V& v, int32_t i) { return i >= 0 && i < widthOf<V>() ? comp(v, i) : 0.0f; }

/// A component of a vector, local or attribute, written.
void writeComponent(const LValue& lv, int32_t c, float x, Env& e) {
    if (lv.kind == LValue::K::Local) {
        switch (lv.base) {
            case Type::Vec2: setComponent(e.v2[static_cast<size_t>(lv.slot)], c, x); break;
            case Type::Vec3: setComponent(e.v3[static_cast<size_t>(lv.slot)], c, x); break;
            case Type::Vec4: setComponent(e.v4[static_cast<size_t>(lv.slot)], c, x); break;
            default: break;
        }
        return;
    }
    const Binding& b = e.run->bindings[static_cast<size_t>(lv.slot)];
    switch (lv.base) {
        case Type::Vec2: { Vec2 v = readBinding<Vec2>(b, e); setComponent(v, c, x); writeBinding(b, e, v); break; }
        case Type::Vec3: { Vec3 v = readBinding<Vec3>(b, e); setComponent(v, c, x); writeBinding(b, e, v); break; }
        case Type::Vec4: { Vec4 v = readBinding<Vec4>(b, e); setComponent(v, c, x); writeBinding(b, e, v); break; }
        default: break;
    }
}

float readComponent(const LValue& lv, int32_t c, Env& e) {
    if (lv.kind == LValue::K::Local) {
        switch (lv.base) {
            case Type::Vec2: return getComponent(e.v2[static_cast<size_t>(lv.slot)], c);
            case Type::Vec3: return getComponent(e.v3[static_cast<size_t>(lv.slot)], c);
            case Type::Vec4: return getComponent(e.v4[static_cast<size_t>(lv.slot)], c);
            default: return 0.0f;
        }
    }
    const Binding& b = e.run->bindings[static_cast<size_t>(lv.slot)];
    switch (lv.base) {
        case Type::Vec2: return getComponent(readBinding<Vec2>(b, e), c);
        case Type::Vec3: return getComponent(readBinding<Vec3>(b, e), c);
        case Type::Vec4: return getComponent(readBinding<Vec4>(b, e), c);
        default: return 0.0f;
    }
}

template <class T> void assignTo(const LValue& lv, const T& v, Env& e) {
    if constexpr (std::is_same_v<T, float>) {
        // A component of an attribute of the element run on, written in place.
        if (lv.comp >= 0 && lv.kind == LValue::K::Attr) {
            const Binding& b = e.run->bindings[static_cast<size_t>(lv.slot)];
            if (b.kind == Binding::K::Attr && b.write && b.map == Binding::Map::Same && e.elem < b.count) {
                reinterpret_cast<float*>(b.write)[e.elem * static_cast<size_t>(width(lv.base)) + static_cast<size_t>(lv.comp)] = v;
                return;
            }
        }
    }
    if (lv.comp >= 0 || (lv.index && isVector(lv.base))) {
        if constexpr (std::is_same_v<T, float>) {
            const int32_t c = lv.comp >= 0 ? lv.comp : ev<int32_t>(*lv.index, e);
            writeComponent(lv, c, v, e);
        }
        return;
    }
    if (lv.index) {  // an element of an array variable
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float> || std::is_same_v<T, Vec2> ||
                      std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4> || std::is_same_v<T, std::string>) {
            auto& a = e.store<std::vector<T>>()[static_cast<size_t>(lv.slot)];
            int64_t k = ev<int32_t>(*lv.index, e);
            if (k < 0) k += static_cast<int64_t>(a.size());
            if (k < 0 || k > (1 << 26)) return;
            if (static_cast<size_t>(k) >= a.size()) a.resize(static_cast<size_t>(k) + 1);
            a[static_cast<size_t>(k)] = v;
        }
        return;
    }
    switch (lv.kind) {
        case LValue::K::Local: e.store<T>()[static_cast<size_t>(lv.slot)] = v; return;
        case LValue::K::Attr:
        case LValue::K::Group: writeBinding(e.run->bindings[static_cast<size_t>(lv.slot)], e, v); return;
    }
}

template <class T> T readFrom(const LValue& lv, Env& e) {
    if (lv.comp >= 0 || (lv.index && isVector(lv.base))) {
        if constexpr (std::is_same_v<T, float>) {
            const int32_t c = lv.comp >= 0 ? lv.comp : ev<int32_t>(*lv.index, e);
            return readComponent(lv, c, e);
        }
        return T{};
    }
    if (lv.index) {
        if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float>) {
            const auto& a = e.store<std::vector<T>>()[static_cast<size_t>(lv.slot)];
            const int64_t k = wrapIndex(ev<int32_t>(*lv.index, e), a.size());
            return k < 0 ? T{} : a[static_cast<size_t>(k)];
        }
        return T{};
    }
    if (lv.kind == LValue::K::Local) return e.store<T>()[static_cast<size_t>(lv.slot)];
    return readBinding<T>(e.run->bindings[static_cast<size_t>(lv.slot)], e);
}

template <class T> T incdec(const TNode& n, Env& e) {
    if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, float>) {
        const T before = readFrom<T>(*n.lv, e);
        const T after = std::is_same_v<T, int32_t> ? static_cast<T>(wrapAdd(static_cast<int32_t>(before), n.a))
                                                    : static_cast<T>(before + static_cast<T>(n.a));
        assignTo(*n.lv, after, e);
        return n.b ? before : after;
    }
    return T{};
}

// --- user functions --------------------------------------------------------------------------------

void copySlot(Type t, int from, int to, Env& e) {
    const auto f = static_cast<size_t>(from), d = static_cast<size_t>(to);
    switch (t) {
        case Type::Int: e.i[d] = e.i[f]; break;
        case Type::Float: e.f[d] = e.f[f]; break;
        case Type::Vec2: e.v2[d] = e.v2[f]; break;
        case Type::Vec3: e.v3[d] = e.v3[f]; break;
        case Type::Vec4: e.v4[d] = e.v4[f]; break;
        case Type::Mat3: e.m3[d] = e.m3[f]; break;
        case Type::Mat4: e.m4[d] = e.m4[f]; break;
        case Type::String: e.s[d] = e.s[f]; break;
        case Type::IntArray: e.ia[d] = e.ia[f]; break;
        case Type::FloatArray: e.fa[d] = e.fa[f]; break;
        case Type::Vec2Array: e.v2a[d] = e.v2a[f]; break;
        case Type::Vec3Array: e.v3a[d] = e.v3a[f]; break;
        case Type::Vec4Array: e.v4a[d] = e.v4a[f]; break;
        case Type::StringArray: e.sa[d] = e.sa[f]; break;
        case Type::Void: break;
    }
}

void clearSlot(Type t, int slot, Env& e) {
    const auto s = static_cast<size_t>(slot);
    switch (t) {
        case Type::Int: e.i[s] = 0; break;
        case Type::Float: e.f[s] = 0.0f; break;
        case Type::Vec2: e.v2[s] = Vec2(); break;
        case Type::Vec3: e.v3[s] = Vec3(); break;
        case Type::Vec4: e.v4[s] = Vec4(); break;
        case Type::Mat3: e.m3[s] = Mat3(); break;
        case Type::Mat4: e.m4[s] = Mat4(); break;
        case Type::String: e.s[s].clear(); break;
        case Type::IntArray: e.ia[s].clear(); break;
        case Type::FloatArray: e.fa[s].clear(); break;
        case Type::Vec2Array: e.v2a[s].clear(); break;
        case Type::Vec3Array: e.v3a[s].clear(); break;
        case Type::Vec4Array: e.v4a[s].clear(); break;
        case Type::StringArray: e.sa[s].clear(); break;
        case Type::Void: break;
    }
}

void invoke(const TNode& n, Env& e) {
    const TFunction& f = (*e.run->functions)[static_cast<size_t>(n.a)];
    // The arguments into places of their own, then into the parameters.
    for (const TPtr& k : n.kids) evalInto(*k, e, nullptr);
    for (size_t i = 0; i < n.kids.size() && i < f.params.size(); ++i) {
        copySlot(f.paramTypes[i], n.kids[i]->lv->slot, f.params[i], e);
    }
    if (f.ret != Type::Void) clearSlot(f.ret, f.result, e);
    exec(*f.body, e);
    for (size_t i = 0; i < n.refs.size() && i < f.params.size(); ++i) {
        if (n.refs[i]) copySlot(f.paramTypes[i], f.params[i], n.refs[i]->slot, e);
    }
}

template <class T> T callUser(const TNode& n, Env& e) {
    invoke(n, e);
    const TFunction& f = (*e.run->functions)[static_cast<size_t>(n.a)];
    if constexpr (!std::is_same_v<T, void>) return e.store<T>()[static_cast<size_t>(f.result)];
}

}  // namespace

// --- the evaluator ---------------------------------------------------------------------------------

template <class T> T ev(const TNode& n, Env& e) {
    constexpr bool isInt = std::is_same_v<T, int32_t>;
    constexpr bool isFloat = std::is_same_v<T, float>;
    constexpr bool isVec = std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>;
    constexpr bool isMat = std::is_same_v<T, Mat3> || std::is_same_v<T, Mat4>;
    constexpr bool isArith = isInt || isFloat || isVec;
    switch (n.op) {
        case Op::Const: return constOf<T>(n);
        case Op::Local: return e.store<T>()[static_cast<size_t>(n.a)];
        case Op::Attr: {
            const Binding& b = e.run->bindings[static_cast<size_t>(n.a)];
            if constexpr (isArith) {
                // The common case first: an attribute of the element run on.
                if (b.kind == Binding::K::Attr && b.map == Binding::Map::Same && e.elem < b.count) {
                    return reinterpret_cast<const T*>(b.read)[e.elem];
                }
            }
            return readBinding<T>(b, e);
        }
        case Op::Comp:
            if constexpr (isFloat) {
                const TNode& k = *n.kids[0];
                if (k.op == Op::Attr) {
                    const Binding& b = e.run->bindings[static_cast<size_t>(k.a)];
                    if (b.kind == Binding::K::Attr && b.map == Binding::Map::Same && e.elem < b.count) {
                        return reinterpret_cast<const float*>(b.read)[e.elem * static_cast<size_t>(width(n.sub)) +
                                                                      static_cast<size_t>(n.a)];
                    }
                }
                switch (n.sub) {
                    case Type::Vec2: return comp(ev<Vec2>(*n.kids[0], e), n.a);
                    case Type::Vec3: return ev<Vec3>(*n.kids[0], e)[n.a];
                    case Type::Vec4: return comp(ev<Vec4>(*n.kids[0], e), n.a);
                    default: return 0.0f;
                }
            }
            break;
        case Op::Index: return indexOf<T>(n, e);
        case Op::Cast: return castOf<T>(n, e);
        case Op::Neg:
            if constexpr (isArith || isMat) return opNeg(ev<T>(*n.kids[0], e));
            break;
        case Op::Not:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) == 0 ? 1 : 0;
            break;
        case Op::BitNot:
            if constexpr (isInt) return ~ev<int32_t>(*n.kids[0], e);
            break;
        case Op::Add:
            if constexpr (isArith || isMat || std::is_same_v<T, std::string>) {
                return opAdd(ev<T>(*n.kids[0], e), ev<T>(*n.kids[1], e));
            }
            break;
        case Op::Sub:
            if constexpr (isArith || isMat) return opSub(ev<T>(*n.kids[0], e), ev<T>(*n.kids[1], e));
            break;
        case Op::Mul:
            if constexpr (isArith) return opMul(ev<T>(*n.kids[0], e), ev<T>(*n.kids[1], e));
            break;
        case Op::Div:
            if constexpr (isArith) return opDiv(ev<T>(*n.kids[0], e), ev<T>(*n.kids[1], e));
            break;
        case Op::Mod:
            if constexpr (isArith) return opMod(ev<T>(*n.kids[0], e), ev<T>(*n.kids[1], e));
            break;
        case Op::VecMat:
            if constexpr (std::is_same_v<T, Vec3>) {
                const Vec3 v = ev<Vec3>(*n.kids[0], e);
                if (n.sub == Type::Mat3) return mul(v, ev<Mat3>(*n.kids[1], e));
                return transformPoint(ev<Mat4>(*n.kids[1], e), v);
            } else if constexpr (std::is_same_v<T, Vec4>) {
                const Vec4 v = ev<Vec4>(*n.kids[0], e);
                return mul(v, ev<Mat4>(*n.kids[1], e));
            }
            break;
        case Op::MatMat:
            // The left first, then the right: VEX's A * B is GLM's B * A.
            if constexpr (std::is_same_v<T, Mat3>) {
                const Mat3 a = ev<Mat3>(*n.kids[0], e);
                return mul(a, ev<Mat3>(*n.kids[1], e));
            } else if constexpr (std::is_same_v<T, Mat4>) {
                const Mat4 a = ev<Mat4>(*n.kids[0], e);
                const Mat4 b = ev<Mat4>(*n.kids[1], e);
                return b * a;
            }
            break;
        case Op::MatScale:
            if constexpr (isMat) return scalem(ev<T>(*n.kids[0], e), ev<float>(*n.kids[1], e));
            break;
        case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Eq: case Op::Ne:
            if constexpr (isInt) return compare(n, e);
            break;
        case Op::And:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) && ev<int32_t>(*n.kids[1], e) ? 1 : 0;
            break;
        case Op::Or:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) || ev<int32_t>(*n.kids[1], e) ? 1 : 0;
            break;
        case Op::BitAnd:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) & ev<int32_t>(*n.kids[1], e);
            break;
        case Op::BitOr:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) | ev<int32_t>(*n.kids[1], e);
            break;
        case Op::BitXor:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) ^ ev<int32_t>(*n.kids[1], e);
            break;
        case Op::Shl:
            if constexpr (isInt) {
                const auto a = static_cast<uint32_t>(ev<int32_t>(*n.kids[0], e));
                return static_cast<int32_t>(a << (static_cast<uint32_t>(ev<int32_t>(*n.kids[1], e)) & 31u));
            }
            break;
        case Op::Shr:
            if constexpr (isInt) return ev<int32_t>(*n.kids[0], e) >> (static_cast<uint32_t>(ev<int32_t>(*n.kids[1], e)) & 31u);
            break;
        case Op::Ternary:
            return ev<int32_t>(*n.kids[0], e) ? ev<T>(*n.kids[1], e) : ev<T>(*n.kids[2], e);
        case Op::Assign: {
            T v = ev<T>(*n.kids[0], e);
            assignTo(*n.lv, v, e);
            return v;
        }
        case Op::IncDec: return incdec<T>(n, e);
        case Op::Builtin: {
            T out{};
            n.fn(e, n, &out);
            return out;
        }
        case Op::Call: return callUser<T>(n, e);
        case Op::Make: return makeOf<T>(n, e);
    }
    return T{};
}

template <class T> const T& evRef(const TNode& n, Env& e, T& scratch) {
    if (n.op == Op::Local) return e.store<T>()[static_cast<size_t>(n.a)];
    scratch = ev<T>(n, e);
    return scratch;
}

template <class T> T& refOf(const LValue& lv, Env& e) { return e.store<T>()[static_cast<size_t>(lv.slot)]; }

#define PG_LANG_TYPES(X) X(int32_t) X(float) X(Vec2) X(Vec3) X(Vec4) X(Mat3) X(Mat4) X(std::string) \
    X(IntArr) X(FloatArr) X(Vec2Arr) X(Vec3Arr) X(Vec4Arr) X(StrArr)
#define PG_INSTANTIATE(T) \
    template T ev<T>(const TNode&, Env&); \
    template const T& evRef<T>(const TNode&, Env&, T&); \
    template T& refOf<T>(const LValue&, Env&);
PG_LANG_TYPES(PG_INSTANTIATE)
#undef PG_INSTANTIATE

void evalInto(const TNode& n, Env& e, void* out) {
    auto put = [&](auto v) {
        if (out) *static_cast<decltype(v)*>(out) = std::move(v);
    };
    switch (n.type) {
        case Type::Void:
            if (n.op == Op::Builtin) n.fn(e, n, out);
            else if (n.op == Op::Call) invoke(n, e);
            return;
        case Type::Int: put(ev<int32_t>(n, e)); return;
        case Type::Float: put(ev<float>(n, e)); return;
        case Type::Vec2: put(ev<Vec2>(n, e)); return;
        case Type::Vec3: put(ev<Vec3>(n, e)); return;
        case Type::Vec4: put(ev<Vec4>(n, e)); return;
        case Type::Mat3: put(ev<Mat3>(n, e)); return;
        case Type::Mat4: put(ev<Mat4>(n, e)); return;
        case Type::String: put(ev<std::string>(n, e)); return;
        case Type::IntArray: put(ev<IntArr>(n, e)); return;
        case Type::FloatArray: put(ev<FloatArr>(n, e)); return;
        case Type::Vec2Array: put(ev<Vec2Arr>(n, e)); return;
        case Type::Vec3Array: put(ev<Vec3Arr>(n, e)); return;
        case Type::Vec4Array: put(ev<Vec4Arr>(n, e)); return;
        case Type::StringArray: put(ev<StrArr>(n, e)); return;
    }
}

namespace {

/// A value into a variable's slot.
void store(Type t, int slot, const TNode& value, Env& e) {
    const auto s = static_cast<size_t>(slot);
    switch (t) {
        case Type::Int: e.i[s] = ev<int32_t>(value, e); break;
        case Type::Float: e.f[s] = ev<float>(value, e); break;
        case Type::Vec2: e.v2[s] = ev<Vec2>(value, e); break;
        case Type::Vec3: e.v3[s] = ev<Vec3>(value, e); break;
        case Type::Vec4: e.v4[s] = ev<Vec4>(value, e); break;
        case Type::Mat3: e.m3[s] = ev<Mat3>(value, e); break;
        case Type::Mat4: e.m4[s] = ev<Mat4>(value, e); break;
        case Type::String: e.s[s] = ev<std::string>(value, e); break;
        case Type::IntArray: e.ia[s] = ev<IntArr>(value, e); break;
        case Type::FloatArray: e.fa[s] = ev<FloatArr>(value, e); break;
        case Type::Vec2Array: e.v2a[s] = ev<Vec2Arr>(value, e); break;
        case Type::Vec3Array: e.v3a[s] = ev<Vec3Arr>(value, e); break;
        case Type::Vec4Array: e.v4a[s] = ev<Vec4Arr>(value, e); break;
        case Type::StringArray: e.sa[s] = ev<StrArr>(value, e); break;
        case Type::Void: break;
    }
}

template <class T> Flow foreachOf(const TStmt& s, Env& e) {
    using A = std::vector<T>;
    const A items = ev<A>(*s.expr, e);  // a copy: the body may change the array
    for (size_t i = 0; i < items.size(); ++i) {
        if (e.halted()) return Flow::Return;
        e.store<T>()[static_cast<size_t>(s.slot)] = items[i];
        if (s.slot2 >= 0) e.i[static_cast<size_t>(s.slot2)] = static_cast<int32_t>(i);
        const Flow f = exec(*s.body[0], e);
        if (f == Flow::Break) break;
        if (f == Flow::Return) return Flow::Return;
    }
    return Flow::Next;
}

}  // namespace

Flow exec(const TStmt& s, Env& e) {
    switch (s.kind) {
        case TS::Expr:
            evalInto(*s.expr, e, nullptr);
            return e.stop ? Flow::Return : Flow::Next;
        case TS::Decl:
            if (s.expr) store(s.type, s.slot, *s.expr, e);
            else clearSlot(s.type, s.slot, e);
            return e.stop ? Flow::Return : Flow::Next;
        case TS::Block:
            for (const TSPtr& b : s.body) {
                const Flow f = exec(*b, e);
                if (f != Flow::Next) return f;
            }
            return Flow::Next;
        case TS::If:
            if (ev<int32_t>(*s.expr, e)) return exec(*s.body[0], e);
            if (s.body.size() > 1) return exec(*s.body[1], e);
            return e.stop ? Flow::Return : Flow::Next;
        case TS::While:
            while (ev<int32_t>(*s.expr, e)) {
                if (e.halted()) return Flow::Return;
                const Flow f = exec(*s.body[0], e);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return Flow::Return;
            }
            return e.stop ? Flow::Return : Flow::Next;
        case TS::DoWhile:
            do {
                if (e.halted()) return Flow::Return;
                const Flow f = exec(*s.body[0], e);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return Flow::Return;
            } while (ev<int32_t>(*s.expr, e));
            return e.stop ? Flow::Return : Flow::Next;
        case TS::For: {
            const Flow init = exec(*s.body[0], e);
            if (init == Flow::Return) return init;
            for (;;) {
                if (s.expr && !ev<int32_t>(*s.expr, e)) break;
                if (e.halted()) return Flow::Return;
                const Flow f = exec(*s.body[1], e);
                if (f == Flow::Break) break;
                if (f == Flow::Return) return Flow::Return;
                if (s.step) evalInto(*s.step, e, nullptr);
            }
            return e.stop ? Flow::Return : Flow::Next;
        }
        case TS::Foreach:
            switch (s.type) {
                case Type::Int: return foreachOf<int32_t>(s, e);
                case Type::Float: return foreachOf<float>(s, e);
                case Type::Vec2: return foreachOf<Vec2>(s, e);
                case Type::Vec3: return foreachOf<Vec3>(s, e);
                case Type::Vec4: return foreachOf<Vec4>(s, e);
                case Type::String: return foreachOf<std::string>(s, e);
                default: return Flow::Next;
            }
        case TS::Break: return Flow::Break;
        case TS::Continue: return Flow::Continue;
        case TS::Return:
            if (s.expr && s.slot >= 0) store(s.type, s.slot, *s.expr, e);
            return Flow::Return;
    }
    return Flow::Next;
}

}  // namespace pg::lang
