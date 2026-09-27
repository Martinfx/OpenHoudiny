// The builtins that read and change geometry, and those the checker types
// itself: ch(), $F, point() ..., printf().
#include "pg/lang/Builtins.h"
#include "pg/lang/Check.h"
#include "pg/lang/Math.h"
#include "pg/lang/Query.h"

#include <cstdio>

namespace pg::lang {
namespace {

template <class T> T arg(Env& e, const TNode& c, size_t i) { return ev<T>(*c.kids[i], e); }
template <class T> void put(void* out, T v) { *static_cast<T*>(out) = std::move(v); }

constexpr size_t kNone = static_cast<size_t>(-1);

void warn(Env& e, const std::string& w) {
    if (e.warnings.size() < 50 && std::find(e.warnings.begin(), e.warnings.end(), w) == e.warnings.end()) {
        e.warnings.push_back(w);
    }
}

const Geometry* inputOf(Env& e, int32_t k) { return k >= 0 && k < 4 ? e.run->inputs[static_cast<size_t>(k)] : nullptr; }

// --- reading an attribute as any type ----------------------------------------------------------------

template <class T> T fromNumbers(const Vec4& v, int n) {
    if constexpr (std::is_same_v<T, int32_t>) return toInt(v.x);
    else if constexpr (std::is_same_v<T, float>) return v.x;
    else if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Vec3> || std::is_same_v<T, Vec4>) {
        T r{};
        for (int i = 0; i < widthOf<T>(); ++i) compRef(r, i) = n == 1 ? v.x : (i < n ? comp(v, i) : 0.0f);
        return r;
    } else if constexpr (std::is_same_v<T, std::string>) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v.x));
        return buf;
    }
    return T{};
}

template <class T> T readAs(const AttributeArray& a, size_t idx) {
    if (idx >= a.size()) return T{};
    switch (a.type()) {
        case AttrType::Int: {
            const int32_t v = a.read<int32_t>()[idx];
            if constexpr (std::is_same_v<T, int32_t>) return v;
            else if constexpr (std::is_same_v<T, std::string>) return std::to_string(v);
            else return fromNumbers<T>(Vec4(static_cast<float>(v), 0, 0, 0), 1);
        }
        case AttrType::Float: return fromNumbers<T>(Vec4(a.read<float>()[idx], 0, 0, 0), 1);
        case AttrType::Vec2: { const Vec2 v = a.read<Vec2>()[idx]; return fromNumbers<T>(Vec4(v.x, v.y, 0, 0), 2); }
        case AttrType::Vec3: { const Vec3 v = a.read<Vec3>()[idx]; return fromNumbers<T>(Vec4(v.x, v.y, v.z, 0), 3); }
        case AttrType::Vec4: return fromNumbers<T>(a.read<Vec4>()[idx], 4);
        case AttrType::String:
            if constexpr (std::is_same_v<T, std::string>) return a.stringValue(a.read<int32_t>()[idx]);
            return T{};
    }
    return T{};
}

/// The element a point(), prim(), vertex() reads: its index, from the
/// arguments at `first` on; kNone if there is none.
size_t elementIndex(Env& e, const TNode& n, const Geometry* g, size_t first, AttrClass cls, bool viaPoint) {
    if (!g) return kNone;
    if (cls == AttrClass::Detail) return 0;
    if (cls == AttrClass::Vertex) {
        size_t linear;
        if (n.kids.size() - first == 2) {
            const int32_t prim = arg<int32_t>(e, n, first), v = arg<int32_t>(e, n, first + 1);
            if (prim < 0 || static_cast<size_t>(prim) >= g->primitiveCount() || v < 0 ||
                static_cast<size_t>(v) >= g->primitiveVertexCount(static_cast<size_t>(prim))) {
                return kNone;
            }
            linear = g->primitiveVertexStart(static_cast<size_t>(prim)) + static_cast<size_t>(v);
        } else {
            const int32_t v = arg<int32_t>(e, n, first);
            if (v < 0 || static_cast<size_t>(v) >= g->vertexCount()) return kNone;
            linear = static_cast<size_t>(v);
        }
        return viaPoint ? g->vertexPoint(linear) : linear;
    }
    const int32_t i = arg<int32_t>(e, n, first);
    return i < 0 ? kNone : static_cast<size_t>(i);
}

template <class T> void attribResolved(Env& e, const TNode& n, void* o) {
    const auto* arr = static_cast<const AttributeArray*>(n.aux);
    const auto* g = static_cast<const Geometry*>(n.aux2);
    T out{};
    const size_t idx = elementIndex(e, n, g, 0, static_cast<AttrClass>(n.a), n.b == 1);
    if (arr && idx != kNone) out = readAs<T>(*arr, idx);
    put(o, std::move(out));
}

template <class T> void attribDynamic(Env& e, const TNode& n, void* o) {
    const Geometry* g = inputOf(e, arg<int32_t>(e, n, 0));
    const std::string name = arg<std::string>(e, n, 1);
    const auto cls = static_cast<AttrClass>(n.a);
    const AttributeArray* arr = g ? g->attributes(cls).find(name) : nullptr;
    bool viaPoint = false;
    if (!arr && g && cls == AttrClass::Vertex) {
        arr = g->points().find(name);
        viaPoint = arr != nullptr;
    }
    T out{};
    const size_t idx = elementIndex(e, n, g, 2, cls, viaPoint);
    if (arr && idx != kNone) out = readAs<T>(*arr, idx);
    put(o, std::move(out));
}

// --- $F, ch() ----------------------------------------------------------------------------------------

template <class T> void hostVariable(Env& e, const TNode& n, void* o) {
    double v = 0.0;
    if (!e.run->host || !e.run->host->variable(n.cs, v)) warn(e, "unknown variable $" + n.cs);
    if constexpr (std::is_same_v<T, int32_t>) put(o, static_cast<int32_t>(std::lround(v)));
    else put(o, static_cast<float>(v));
}

template <class T> void channelOf(Env& e, const TNode& n, void* o) {
    const std::string path = arg<std::string>(e, n, 0);
    std::string why;
    if constexpr (std::is_same_v<T, std::string>) {
        std::string text;
        if (!e.run->host || !e.run->host->channelText(path, text, why)) warn(e, "chs(\"" + path + "\"): " + why);
        put(o, std::move(text));
    } else {
        Vec4 v;
        const int comps = std::is_same_v<T, int32_t> || std::is_same_v<T, float> ? 1 : widthOf<T>();
        for (int c = 0; c < comps; ++c) {
            double x = 0.0;
            if (!e.run->host || !e.run->host->channel(path, c, x, why)) {
                warn(e, "ch(\"" + path + "\"): " + (why.empty() ? "no such parameter" : why));
                break;
            }
            compRef(v, c) = static_cast<float>(x);
        }
        if constexpr (std::is_same_v<T, int32_t>) put(o, static_cast<int32_t>(std::lround(v.x)));
        else if constexpr (std::is_same_v<T, float>) put(o, v.x);
        else put(o, fromNumbers<T>(v, widthOf<T>()));
    }
}

// --- printf ------------------------------------------------------------------------------------------

template <class V> Vec4 widenTo4(const V& v) {
    Vec4 r;
    for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = comp(v, i);
    return r;
}

template <class A> std::string listText(const A& a) {
    std::string s = "{";
    for (size_t i = 0; i < a.size(); ++i) {
        FormatArg f;
        using E = typename A::value_type;
        if constexpr (std::is_same_v<E, std::string>) s += (i ? ", " : "") + a[i];
        else {
            if constexpr (std::is_same_v<E, int32_t> || std::is_same_v<E, float>) {
                f.type = Type::Float;
                f.number = static_cast<double>(a[i]);
                s += (i ? ", " : "") + format(std::is_same_v<E, int32_t> ? "%d" : "%g", {f});
            } else {
                f.type = TypeOf<E>::value;
                f.v = widenTo4(a[i]);
                s += (i ? ", " : "") + format("%g", {f});
            }
        }
    }
    return s + "}";
}


FormatArg formatArg(const TNode& k, Env& e) {
    FormatArg a;
    a.type = k.type;
    switch (k.type) {
        case Type::Int: a.number = ev<int32_t>(k, e); break;
        case Type::Float: a.number = ev<float>(k, e); break;
        case Type::Vec2: a.v = widenTo4(ev<Vec2>(k, e)); break;
        case Type::Vec3: a.v = widenTo4(ev<Vec3>(k, e)); break;
        case Type::Vec4: a.v = ev<Vec4>(k, e); break;
        case Type::String: a.text = ev<std::string>(k, e); break;
        case Type::IntArray: a.text = listText(ev<IntArr>(k, e)); break;
        case Type::FloatArray: a.text = listText(ev<FloatArr>(k, e)); break;
        case Type::Vec2Array: a.text = listText(ev<Vec2Arr>(k, e)); break;
        case Type::Vec3Array: a.text = listText(ev<Vec3Arr>(k, e)); break;
        case Type::Vec4Array: a.text = listText(ev<Vec4Arr>(k, e)); break;
        case Type::StringArray: a.text = listText(ev<StrArr>(k, e)); break;
        case Type::Mat3: {
            const Mat3 m = ev<Mat3>(k, e);
            char buf[256];
            std::snprintf(buf, sizeof buf, "[[%g, %g, %g], [%g, %g, %g], [%g, %g, %g]]", m.m[0][0], m.m[0][1], m.m[0][2],
                          m.m[1][0], m.m[1][1], m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2]);
            a.text = buf;
            break;
        }
        case Type::Mat4: {
            const Mat4 m = ev<Mat4>(k, e);
            std::string s = "[";
            for (int i = 0; i < 4; ++i) {
                char buf[128];
                std::snprintf(buf, sizeof buf, "%s[%g, %g, %g, %g]", i ? ", " : "", m.m[i][0], m.m[i][1], m.m[i][2], m.m[i][3]);
                s += buf;
            }
            a.text = s + "]";
            break;
        }
        case Type::Void: break;
    }
    if (!a.text.empty() && a.type != Type::String) a.type = Type::String;
    return a;
}

template <int Kind> void formatted(Env& e, const TNode& n, void* o) {
    std::vector<FormatArg> args;
    for (size_t i = 1; i < n.kids.size(); ++i) args.push_back(formatArg(*n.kids[i], e));
    std::string text = format(arg<std::string>(e, n, 0), args);
    if constexpr (Kind == 0) {
        if (e.log.size() < 64 * 1024) e.log += text;
    } else if constexpr (Kind == 1) {
        put(o, std::move(text));
    } else if constexpr (Kind == 2) {
        warn(e, text);
    } else {
        e.error = text.empty() ? "error()" : text;
        e.stop = true;
    }
}

// --- the geometry's queries --------------------------------------------------------------------------

template <class Fn> void withInput(Env& e, const TNode& c, void* o, Fn fn) {
    const int32_t k = arg<int32_t>(e, c, 0);
    InputQuery* q = e.run->queries ? e.run->queries->input(k) : nullptr;
    fn(q && q->geo ? q : nullptr, o);
}

IntArr toArray(std::span<const int32_t> s) { return IntArr(s.begin(), s.end()); }

Vec3 newellNormal(const Geometry& g, size_t prim) {
    const auto pts = g.primitivePoints(prim);
    const auto P = g.positions();
    Vec3 n;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec3& a = P[pts[i]];
        const Vec3& b = P[pts[(i + 1) % pts.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

size_t primOfVertex(const Geometry& g, size_t v) {
    // The primitives' first vertices rise: a binary search.
    size_t lo = 0, hi = g.primitiveCount();
    while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g.primitiveVertexStart(mid) <= v) lo = mid;
        else hi = mid;
    }
    return lo;
}

// --- changing geometry ---------------------------------------------------------------------------------

uint8_t modeOf(const std::string& m) {
    if (m == "add") return 1;
    if (m == "min") return 2;
    if (m == "max") return 3;
    if (m == "mult" || m == "multiply") return 4;
    return 0;
}

template <class T> Deferred::SetAttr valueOf(const T& v) {
    Deferred::SetAttr s;
    s.type = TypeOf<T>::value;
    if constexpr (std::is_same_v<T, int32_t>) s.value.x = static_cast<float>(v);
    else if constexpr (std::is_same_v<T, float>) s.value.x = v;
    else if constexpr (std::is_same_v<T, std::string>) s.text = v;
    else s.value = widenTo4(v);
    return s;
}

/// setpointattrib(geo, name, elem, value [, mode]) and its kin.
template <class T, AttrClass Cls> void setAttrib(Env& e, const TNode& c, void* o) {
    if (!e.deferred) return;
    const std::string name = arg<std::string>(e, c, 1);
    size_t at = 2;
    int32_t vertexOf = -1;
    int32_t elem = 0;
    if constexpr (Cls == AttrClass::Vertex) {
        vertexOf = arg<int32_t>(e, c, at++);
        elem = arg<int32_t>(e, c, at++);
    } else if constexpr (Cls != AttrClass::Detail) {
        elem = arg<int32_t>(e, c, at++);
    }
    Deferred::SetAttr s = valueOf(arg<T>(e, c, at++));
    s.cls = Cls;
    s.name = name;
    s.elem = elem;
    s.vertexOf = vertexOf;
    if (at < c.kids.size()) s.mode = modeOf(arg<std::string>(e, c, at));
    e.deferred->attrs.push_back(std::move(s));
    put(o, elem);
}

template <class T> void registerSetters(Builtins& b) {
    const Type t = TypeOf<T>::value;
    using I = Type;
    add(b, "setpointattrib", I::Int, {I::Int, I::String, I::Int, t}, &setAttrib<T, AttrClass::Point>, 0, true);
    add(b, "setpointattrib", I::Int, {I::Int, I::String, I::Int, t, I::String}, &setAttrib<T, AttrClass::Point>, 0, true);
    add(b, "setprimattrib", I::Int, {I::Int, I::String, I::Int, t}, &setAttrib<T, AttrClass::Primitive>, 0, true);
    add(b, "setprimattrib", I::Int, {I::Int, I::String, I::Int, t, I::String}, &setAttrib<T, AttrClass::Primitive>, 0, true);
    add(b, "setvertexattrib", I::Int, {I::Int, I::String, I::Int, I::Int, t}, &setAttrib<T, AttrClass::Vertex>, 0, true);
    add(b, "setvertexattrib", I::Int, {I::Int, I::String, I::Int, I::Int, t, I::String}, &setAttrib<T, AttrClass::Vertex>, 0, true);
    add(b, "setdetailattrib", I::Int, {I::Int, I::String, t}, &setAttrib<T, AttrClass::Detail>, 0, true);
    add(b, "setdetailattrib", I::Int, {I::Int, I::String, t, I::String}, &setAttrib<T, AttrClass::Detail>, 0, true);
}

int32_t newPrim(Env& e, const std::string& type, IntArr points) {
    if (type != "poly" && type != "polyline") warn(e, "addprim(): \"" + type + "\" is made a polygon -- \"poly\" and \"polyline\" are what there is");
    Deferred& d = *e.deferred;
    d.prims.push_back({type != "polyline", std::move(points)});
    return static_cast<int32_t>(d.basePrims + d.prims.size() - 1);
}

}  // namespace

// --- what the checker hands out ----------------------------------------------------------------------

Impl implHostVariable(Type t) { return t == Type::Int ? &hostVariable<int32_t> : &hostVariable<float>; }

Impl implChannel(Type t) {
    switch (t) {
        case Type::Int: return &channelOf<int32_t>;
        case Type::Vec2: return &channelOf<Vec2>;
        case Type::Vec3: return &channelOf<Vec3>;
        case Type::Vec4: return &channelOf<Vec4>;
        case Type::String: return &channelOf<std::string>;
        default: return &channelOf<float>;
    }
}

Impl implAttribRead(Type t, bool resolved) {
    switch (t) {
        case Type::Int: return resolved ? &attribResolved<int32_t> : &attribDynamic<int32_t>;
        case Type::Vec2: return resolved ? &attribResolved<Vec2> : &attribDynamic<Vec2>;
        case Type::Vec3: return resolved ? &attribResolved<Vec3> : &attribDynamic<Vec3>;
        case Type::Vec4: return resolved ? &attribResolved<Vec4> : &attribDynamic<Vec4>;
        case Type::String: return resolved ? &attribResolved<std::string> : &attribDynamic<std::string>;
        default: return resolved ? &attribResolved<float> : &attribDynamic<float>;
    }
}

Impl implFormat(int kind) {
    switch (kind) {
        case 0: return &formatted<0>;
        case 1: return &formatted<1>;
        case 2: return &formatted<2>;
        default: return &formatted<3>;
    }
}

void addGeometryBuiltins(Builtins& b) {
    using I = Type;
    add(b, "geoself", I::Int, {}, [](Env&, const TNode&, void* o) { put(o, int32_t(0)); });
    add(b, "npoints", I::Int, {I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, static_cast<int32_t>(g ? g->pointCount() : 0));
    });
    add(b, "nprimitives", I::Int, {I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, static_cast<int32_t>(g ? g->primitiveCount() : 0));
    });
    add(b, "nvertices", I::Int, {I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, static_cast<int32_t>(g ? g->vertexCount() : 0));
    });
    add(b, "primpoints", I::IntArray, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1);
        IntArr out;
        if (g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount()) {
            for (uint32_t pt : g->primitivePoints(static_cast<size_t>(p))) out.push_back(static_cast<int32_t>(pt));
        }
        put(o, std::move(out));
    });
    add(b, "primpoint", I::Int, {I::Int, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1), i = arg<int32_t>(e, c, 2);
        int32_t r = -1;
        if (g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount()) {
            const auto pts = g->primitivePoints(static_cast<size_t>(p));
            if (i >= 0 && static_cast<size_t>(i) < pts.size()) r = static_cast<int32_t>(pts[static_cast<size_t>(i)]);
        }
        put(o, r);
    });
    add(b, "primvertexcount", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1);
        put(o, static_cast<int32_t>(g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount()
                                        ? g->primitiveVertexCount(static_cast<size_t>(p)) : 0));
    });
    add(b, "primvertex", I::Int, {I::Int, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1), i = arg<int32_t>(e, c, 2);
        int32_t r = -1;
        if (g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount() && i >= 0 &&
            static_cast<size_t>(i) < g->primitiveVertexCount(static_cast<size_t>(p))) {
            r = static_cast<int32_t>(g->primitiveVertexStart(static_cast<size_t>(p)) + static_cast<size_t>(i));
        }
        put(o, r);
    });
    add(b, "vertexpoint", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t v = arg<int32_t>(e, c, 1);
        put(o, g && v >= 0 && static_cast<size_t>(v) < g->vertexCount() ? static_cast<int32_t>(g->vertexPoint(static_cast<size_t>(v))) : -1);
    });
    add(b, "vertexprim", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t v = arg<int32_t>(e, c, 1);
        put(o, g && v >= 0 && static_cast<size_t>(v) < g->vertexCount() ? static_cast<int32_t>(primOfVertex(*g, static_cast<size_t>(v))) : -1);
    });
    add(b, "vertexprimindex", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t v = arg<int32_t>(e, c, 1);
        if (!g || v < 0 || static_cast<size_t>(v) >= g->vertexCount()) {
            put(o, int32_t(-1));
            return;
        }
        const size_t p = primOfVertex(*g, static_cast<size_t>(v));
        put(o, static_cast<int32_t>(static_cast<size_t>(v) - g->primitiveVertexStart(p)));
    });
    add(b, "pointprims", I::IntArray, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            put(out, q ? toArray(q->adjacency().primitives(static_cast<size_t>(std::max(arg<int32_t>(e, c, 1), 0)))) : IntArr());
        });
    });
    add(b, "pointvertices", I::IntArray, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            put(out, q ? toArray(q->adjacency().vertices(static_cast<size_t>(std::max(arg<int32_t>(e, c, 1), 0)))) : IntArr());
        });
    });
    add(b, "neighbours", I::IntArray, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            const int32_t pt = arg<int32_t>(e, c, 1);
            put(out, q && pt >= 0 ? toArray(q->adjacency().neighbours(static_cast<size_t>(pt))) : IntArr());
        });
    });
    add(b, "neighbourcount", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            const int32_t pt = arg<int32_t>(e, c, 1);
            put(out, static_cast<int32_t>(q && pt >= 0 ? q->adjacency().neighbours(static_cast<size_t>(pt)).size() : 0));
        });
    });
    add(b, "nearpoints", I::IntArray, {I::Int, I::Vec3, I::Float}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            IntArr r;
            if (q) q->tree().near(arg<Vec3>(e, c, 1), arg<float>(e, c, 2), 0, r);
            put(out, std::move(r));
        });
    });
    add(b, "nearpoints", I::IntArray, {I::Int, I::Vec3, I::Float, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            IntArr r;
            const int32_t max = arg<int32_t>(e, c, 3);
            if (q && max > 0) q->tree().near(arg<Vec3>(e, c, 1), arg<float>(e, c, 2), static_cast<size_t>(max), r);
            put(out, std::move(r));
        });
    });
    add(b, "pcfind", I::IntArray, {I::Int, I::String, I::Vec3, I::Float, I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            IntArr r;
            if (arg<std::string>(e, c, 1) != "P") warn(e, "pcfind() looks by P only");
            const int32_t max = arg<int32_t>(e, c, 4);
            if (q && max > 0) q->tree().near(arg<Vec3>(e, c, 2), arg<float>(e, c, 3), static_cast<size_t>(max), r);
            put(out, std::move(r));
        });
    });
    add(b, "nearpoint", I::Int, {I::Int, I::Vec3}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) { put(out, q ? q->tree().nearest(arg<Vec3>(e, c, 1)) : int32_t(-1)); });
    });
    add(b, "nearpoint", I::Int, {I::Int, I::Vec3, I::Float}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            put(out, q ? q->tree().nearest(arg<Vec3>(e, c, 1), arg<float>(e, c, 2)) : int32_t(-1));
        });
    });
    add(b, "inpointgroup", I::Int, {I::Int, I::String, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        const int32_t i = arg<int32_t>(e, c, 2);
        put(o, int32_t(gr && gr->classOf() == AttrClass::Point && i >= 0 && gr->contains(static_cast<size_t>(i)) ? 1 : 0));
    });
    add(b, "inprimgroup", I::Int, {I::Int, I::String, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        const int32_t i = arg<int32_t>(e, c, 2);
        put(o, int32_t(gr && gr->classOf() == AttrClass::Primitive && i >= 0 && gr->contains(static_cast<size_t>(i)) ? 1 : 0));
    });
    add(b, "expandpointgroup", I::IntArray, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        IntArr out;
        if (gr && gr->classOf() == AttrClass::Point) {
            const auto m = gr->mask();
            for (size_t i = 0; i < m.size(); ++i) {
                if (m[i]) out.push_back(static_cast<int32_t>(i));
            }
        }
        put(o, std::move(out));
    });
    add(b, "expandprimgroup", I::IntArray, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        IntArr out;
        if (gr && gr->classOf() == AttrClass::Primitive) {
            const auto m = gr->mask();
            for (size_t i = 0; i < m.size(); ++i) {
                if (m[i]) out.push_back(static_cast<int32_t>(i));
            }
        }
        put(o, std::move(out));
    });
    add(b, "npointsgroup", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        put(o, static_cast<int32_t>(gr && gr->classOf() == AttrClass::Point ? gr->memberCount() : 0));
    });
    add(b, "nprimitivesgroup", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const Group* gr = g ? g->findGroup(arg<std::string>(e, c, 1)) : nullptr;
        put(o, static_cast<int32_t>(gr && gr->classOf() == AttrClass::Primitive ? gr->memberCount() : 0));
    });
    add(b, "haspointattrib", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, int32_t(g && g->points().find(arg<std::string>(e, c, 1)) ? 1 : 0));
    });
    add(b, "hasprimattrib", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, int32_t(g && g->primitives().find(arg<std::string>(e, c, 1)) ? 1 : 0));
    });
    add(b, "hasvertexattrib", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, int32_t(g && g->vertices().find(arg<std::string>(e, c, 1)) ? 1 : 0));
    });
    add(b, "hasdetailattrib", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        put(o, int32_t(g && g->detail().find(arg<std::string>(e, c, 1)) ? 1 : 0));
    });
    add(b, "getbbox_min", I::Vec3, {I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            Vec3 mn, mx;
            if (q) q->box(mn, mx);
            put(out, mn);
        });
    });
    add(b, "getbbox_max", I::Vec3, {I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            Vec3 mn, mx;
            if (q) q->box(mn, mx);
            put(out, mx);
        });
    });
    add(b, "getbbox_center", I::Vec3, {I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            Vec3 mn, mx;
            if (q) q->box(mn, mx);
            put(out, (mn + mx) * 0.5f);
        });
    });
    add(b, "getbbox_size", I::Vec3, {I::Int}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            Vec3 mn, mx;
            if (q) q->box(mn, mx);
            put(out, mx - mn);
        });
    });
    add(b, "relbbox", I::Vec3, {I::Int, I::Vec3}, [](Env& e, const TNode& c, void* o) {
        withInput(e, c, o, [&](InputQuery* q, void* out) {
            Vec3 mn, mx;
            if (q) q->box(mn, mx);
            const Vec3 p = arg<Vec3>(e, c, 1);
            Vec3 r;
            for (int a = 0; a < 3; ++a) r[a] = mx[a] > mn[a] ? (p[a] - mn[a]) / (mx[a] - mn[a]) : 0.0f;
            put(out, r);
        });
    });
    add(b, "prim_normal", I::Vec3, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1);
        put(o, g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount() ? normalize(newellNormal(*g, static_cast<size_t>(p))) : Vec3());
    });
    add(b, "primarea", I::Float, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1);
        put(o, g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount() && g->primitiveClosed(static_cast<size_t>(p))
                   ? 0.5f * length(newellNormal(*g, static_cast<size_t>(p))) : 0.0f);
    });
    add(b, "primcentroid", I::Vec3, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        const Geometry* g = inputOf(e, arg<int32_t>(e, c, 0));
        const int32_t p = arg<int32_t>(e, c, 1);
        Vec3 r;
        if (g && p >= 0 && static_cast<size_t>(p) < g->primitiveCount()) {
            const auto pts = g->primitivePoints(static_cast<size_t>(p));
            const auto P = g->positions();
            for (uint32_t i : pts) r += P[i];
            if (!pts.empty()) r = r * (1.0f / static_cast<float>(pts.size()));
        }
        put(o, r);
    });

    // Making and deleting.
    add(b, "addpoint", I::Int, {I::Int, I::Vec3}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        Deferred& d = *e.deferred;
        d.points.push_back({arg<Vec3>(e, c, 1), -1});
        put(o, static_cast<int32_t>(d.basePoints + d.points.size() - 1));
    }, 0, true);
    add(b, "addpoint", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        Deferred& d = *e.deferred;
        const int32_t from = arg<int32_t>(e, c, 1);
        Vec3 P;
        if (from >= 0 && static_cast<size_t>(from) < d.basePoints) {
            P = e.run->geo->positions()[static_cast<size_t>(from)];
        } else if (from >= 0 && static_cast<size_t>(from) - d.basePoints < d.points.size()) {
            P = d.points[static_cast<size_t>(from) - d.basePoints].P;
        }
        d.points.push_back({P, from >= 0 && static_cast<size_t>(from) < d.basePoints ? from : -1});
        put(o, static_cast<int32_t>(d.basePoints + d.points.size() - 1));
    }, 0, true);
    add(b, "addprim", I::Int, {I::Int, I::String}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        put(o, newPrim(e, arg<std::string>(e, c, 1), {}));
    }, 0, true);
    add(b, "addprim", I::Int, {I::Int, I::String, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        IntArr pts;
        for (size_t i = 2; i < c.kids.size(); ++i) pts.push_back(arg<int32_t>(e, c, i));
        put(o, newPrim(e, arg<std::string>(e, c, 1), std::move(pts)));
    }, 0, true, true);
    add(b, "addprim", I::Int, {I::Int, I::String, I::IntArray}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        put(o, newPrim(e, arg<std::string>(e, c, 1), arg<IntArr>(e, c, 2)));
    }, 0, true);
    add(b, "addvertex", I::Int, {I::Int, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(-1));
        Deferred& d = *e.deferred;
        const int32_t prim = arg<int32_t>(e, c, 1), pt = arg<int32_t>(e, c, 2);
        const int64_t k = static_cast<int64_t>(prim) - static_cast<int64_t>(d.basePrims);
        if (k < 0 || k >= static_cast<int64_t>(d.prims.size())) {
            warn(e, "addvertex(): only to a primitive this program made with addprim()");
            return put(o, int32_t(-1));
        }
        auto& pts = d.prims[static_cast<size_t>(k)].points;
        pts.push_back(pt);
        put(o, static_cast<int32_t>(pts.size() - 1));
    }, 0, true);
    add(b, "removepoint", I::Int, {I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(0));
        const int32_t pt = arg<int32_t>(e, c, 1);
        e.deferred->removePoints.push_back(pt);
        put(o, int32_t(1));
    }, 0, true);
    add(b, "removepoint", I::Int, {I::Int, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(0));
        const int32_t pt = arg<int32_t>(e, c, 1);
        e.deferred->removePoints.push_back(pt);
        put(o, int32_t(1));
    }, 0, true);
    add(b, "removeprim", I::Int, {I::Int, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(0));
        e.deferred->removePrims.emplace_back(arg<int32_t>(e, c, 1), arg<int32_t>(e, c, 2) != 0);
        put(o, int32_t(1));
    }, 0, true);
    add(b, "setpointgroup", I::Int, {I::Int, I::String, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(0));
        e.deferred->groups.push_back({AttrClass::Point, arg<std::string>(e, c, 1), arg<int32_t>(e, c, 2), arg<int32_t>(e, c, 3) != 0});
        put(o, int32_t(1));
    }, 0, true);
    add(b, "setprimgroup", I::Int, {I::Int, I::String, I::Int, I::Int}, [](Env& e, const TNode& c, void* o) {
        if (!e.deferred) return put(o, int32_t(0));
        e.deferred->groups.push_back({AttrClass::Primitive, arg<std::string>(e, c, 1), arg<int32_t>(e, c, 2), arg<int32_t>(e, c, 3) != 0});
        put(o, int32_t(1));
    }, 0, true);
    registerSetters<int32_t>(b);
    registerSetters<float>(b);
    registerSetters<Vec2>(b);
    registerSetters<Vec3>(b);
    registerSetters<Vec4>(b);
    registerSetters<std::string>(b);
}

}  // namespace pg::lang
