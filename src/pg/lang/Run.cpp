// Running a program over geometry, and an expression without it: the
// public face of the language (Lang.h).
#include "pg/lang/Check.h"
#include "pg/lang/Math.h"
#include "pg/lang/Query.h"

#include "pg/core/Parallel.h"
#include "pg/core/Selection.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace pg::lang {

// --- hosts -----------------------------------------------------------------------------------------

bool Host::variable(std::string_view, double&) const { return false; }

bool Host::channel(std::string_view path, int, double&, std::string& error) const {
    error = "no parameter '" + std::string(path) + "'";
    return false;
}

bool Host::channelText(std::string_view path, std::string&, std::string& error) const {
    error = "no parameter '" + std::string(path) + "'";
    return false;
}

bool TimeHost::variable(std::string_view name, double& out) const {
    if (name == "F" || name == "SF") out = ctx_.frame;
    else if (name == "FF") out = ctx_.frame;
    else if (name == "T") out = ctx_.time;
    else if (name == "FPS") out = ctx_.fps;
    else return false;
    return true;
}

bool isNumber(std::string_view text) {
    std::string s(text);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    if (start == s.size()) return false;
    const char* begin = s.c_str() + start;
    char* end = nullptr;
    std::strtod(begin, &end);
    return end && *end == '\0' && end != begin;
}

// --- applying what an ordered run asked for --------------------------------------------------------

namespace {

/// A deferred attribute value onto an element, in the attribute's own type.
void applyValue(AttributeArray& arr, size_t idx, const Deferred::SetAttr& s) {
    if (idx >= arr.size()) return;
    auto mix = [&](float old, float v) {
        switch (s.mode) {
            case 1: return old + v;
            case 2: return std::min(old, v);
            case 3: return std::max(old, v);
            case 4: return old * v;
            default: return v;
        }
    };
    switch (arr.type()) {
        case AttrType::Int: {
            auto w = arr.write<int32_t>();
            const int32_t v = s.type == Type::Int ? static_cast<int32_t>(s.value.x) : toInt(s.value.x);
            switch (s.mode) {
                case 1: w[idx] = wrapAdd(w[idx], v); break;
                case 2: w[idx] = std::min(w[idx], v); break;
                case 3: w[idx] = std::max(w[idx], v); break;
                case 4: w[idx] = wrapMul(w[idx], v); break;
                default: w[idx] = v; break;
            }
            break;
        }
        case AttrType::Float: { auto w = arr.write<float>(); w[idx] = mix(w[idx], s.value.x); break; }
        case AttrType::Vec2: {
            auto w = arr.write<Vec2>();
            w[idx] = Vec2(mix(w[idx].x, s.value.x), mix(w[idx].y, s.value.y));
            break;
        }
        case AttrType::Vec3: {
            auto w = arr.write<Vec3>();
            w[idx] = Vec3(mix(w[idx].x, s.value.x), mix(w[idx].y, s.value.y), mix(w[idx].z, s.value.z));
            break;
        }
        case AttrType::Vec4: {
            auto w = arr.write<Vec4>();
            w[idx] = Vec4(mix(w[idx].x, s.value.x), mix(w[idx].y, s.value.y), mix(w[idx].z, s.value.z),
                          mix(w[idx].w, s.value.w));
            break;
        }
        case AttrType::String: {
            const int32_t id = arr.internString(s.text);
            arr.write<int32_t>()[idx] = id;
            break;
        }
    }
}

void copyPoint(Geometry& geo, size_t from, size_t to) {
    for (auto& [name, arr] : geo.points()) {
        if (from >= arr.size() || to >= arr.size()) continue;
        const size_t size = attrSize(arr.type());
        std::byte* data = arr.rawWrite();
        std::memcpy(data + to * size, data + from * size, size);
    }
}

bool apply(Deferred& d, Geometry& geo, std::vector<std::string>& warnings) {
    auto warn = [&](const std::string& w) {
        if (warnings.size() < 50 && std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w);
    };
    // New points, then primitives over them.
    if (!d.points.empty()) {
        const size_t first = geo.addPoints(d.points.size());
        for (size_t k = 0; k < d.points.size(); ++k) {
            const auto& p = d.points[k];
            if (p.from >= 0 && static_cast<size_t>(p.from) < first) copyPoint(geo, static_cast<size_t>(p.from), first + k);
        }
        auto P = geo.positionsForWrite();
        for (size_t k = 0; k < d.points.size(); ++k) P[first + k] = d.points[k].P;
    }
    const size_t np = geo.pointCount();
    for (const auto& prim : d.prims) {
        if (prim.points.empty()) continue;
        std::vector<uint32_t> pts;
        pts.reserve(prim.points.size());
        bool ok = true;
        for (int32_t p : prim.points) {
            if (p < 0 || static_cast<size_t>(p) >= np) {
                ok = false;
                break;
            }
            pts.push_back(static_cast<uint32_t>(p));
        }
        if (!ok) {
            warn("addprim(): a point that is not there -- the primitive is left out");
            continue;
        }
        geo.addPrimitive(pts, prim.closed);
    }
    // Attribute values.
    for (const auto& s : d.attrs) {
        AttributeSet& set = geo.attributes(s.cls);
        AttributeArray* arr = set.find(s.name);
        if (!arr) {
            AttrType t;
            if (!toAttr(s.type, t)) continue;
            arr = &set.create(s.name, t);
        }
        const Type have = fromAttr(arr->type());
        if (have != s.type && !(isScalar(have) && isScalar(s.type)) && !(isVector(have) && isVector(s.type))) {
            warn("set" + std::string(s.cls == AttrClass::Point ? "point" : s.cls == AttrClass::Primitive ? "prim"
                                     : s.cls == AttrClass::Vertex ? "vertex" : "detail") +
                 "attrib(): @" + s.name + " is a " + typeName(have) + ", the value a " + typeName(s.type));
            continue;
        }
        size_t idx = static_cast<size_t>(std::max(s.elem, 0));
        if (s.elem < 0) continue;
        if (s.cls == AttrClass::Vertex && s.vertexOf >= 0) {
            if (static_cast<size_t>(s.vertexOf) >= geo.primitiveCount() ||
                idx >= geo.primitiveVertexCount(static_cast<size_t>(s.vertexOf))) {
                continue;
            }
            idx += geo.primitiveVertexStart(static_cast<size_t>(s.vertexOf));
        }
        if (s.cls == AttrClass::Detail) idx = 0;
        applyValue(*arr, idx, s);
    }
    // Groups.
    for (const auto& g : d.groups) {
        Group* group = geo.findGroup(g.name);
        if (group && group->classOf() != g.cls) {
            warn("group '" + g.name + "' is of another class");
            continue;
        }
        Group& gr = group ? *group : geo.createGroup(g.name, g.cls);
        if (g.elem >= 0 && static_cast<size_t>(g.elem) < geo.elementCount(g.cls)) gr.set(static_cast<size_t>(g.elem), g.member);
    }
    // What goes: primitives first -- point numbers stay -- then points.
    if (!d.removePrims.empty()) {
        const size_t nprim = geo.primitiveCount();
        std::vector<uint8_t> keep(nprim, 1);
        std::vector<uint8_t> dropPoints;  // points only deleted primitives used, when asked to
        bool andPoints = false;
        for (const auto& [prim, pts] : d.removePrims) {
            if (prim < 0 || static_cast<size_t>(prim) >= nprim) continue;
            keep[static_cast<size_t>(prim)] = 0;
            andPoints = andPoints || pts;
        }
        if (andPoints) {
            std::vector<uint8_t> used(geo.pointCount(), 0);  // 1 kept, 2 by a primitive going with its points
            std::set<int32_t> withPoints;
            for (const auto& [prim, pts] : d.removePrims) {
                if (pts) withPoints.insert(prim);
            }
            for (size_t p = 0; p < nprim; ++p) {
                for (uint32_t pt : geo.primitivePoints(p)) {
                    if (keep[p]) used[pt] = 1;
                    else if (withPoints.count(static_cast<int32_t>(p)) && used[pt] == 0) used[pt] = 2;
                }
            }
            dropPoints.assign(used.size(), 1);
            for (size_t i = 0; i < used.size(); ++i) {
                if (used[i] == 2) dropPoints[i] = 0;
            }
        }
        geo.deletePrimitives(keep, false);
        if (!dropPoints.empty()) {
            for (int32_t p : d.removePoints) {
                if (p >= 0 && static_cast<size_t>(p) < dropPoints.size()) dropPoints[static_cast<size_t>(p)] = 0;
            }
            d.removePoints.clear();
            geo.deletePoints(dropPoints);
        }
    }
    if (!d.removePoints.empty()) {
        std::vector<uint8_t> keep(geo.pointCount(), 1);
        for (int32_t p : d.removePoints) {
            if (p >= 0 && static_cast<size_t>(p) < keep.size()) keep[static_cast<size_t>(p)] = 0;
        }
        geo.deletePoints(keep);
    }
    return true;
}

void merge(RunReport* report, std::string& log, std::vector<std::string>& warnings, const std::string& moreLog,
           const std::vector<std::string>& moreWarnings) {
    (void)report;
    if (log.size() < 64 * 1024) log += moreLog;
    for (const std::string& w : moreWarnings) {
        if (warnings.size() < 50 && std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w);
    }
}

void runMain(const std::vector<TSPtr>& main, Env& e) {
    for (const TSPtr& s : main) {
        if (exec(*s, e) == Flow::Return || e.stop) return;
    }
}

}  // namespace

// --- Program ---------------------------------------------------------------------------------------

struct Program::Impl {
    std::unique_ptr<Ast> ast;
};

Program::Program() : impl_(std::make_unique<Impl>()) {}
Program::~Program() = default;

std::unique_ptr<Program> Program::parse(const std::string& source, std::string& error) {
    auto ast = lang::parse(source, false, error);
    if (!ast) return nullptr;
    std::unique_ptr<Program> p(new Program());
    p->impl_->ast = std::move(ast);
    return p;
}

bool Program::readsTime() const { return impl_->ast->readsTime; }
const std::vector<Channel>& Program::channels() const { return impl_->ast->channels; }
const std::vector<std::string>& Program::slotNames() const { return impl_->ast->attrNames; }

bool Program::run(Geometry& geo, const CookContext& ctx, std::string& error) const {
    RunOptions o;
    o.ctx = ctx;
    return run(geo, o, error, nullptr);
}

bool Program::run(Geometry& geo, const RunOptions& options, std::string& error, RunReport* report) const {
    error.clear();
    // Input 0 as it came in: a copy that shares every buffer, taken before
    // anything is written -- the program's writes clone what they touch.
    GeometryPtr snapshot = options.inputs[0];
    if (!snapshot) snapshot = std::make_shared<Geometry>(geo);

    Run run;
    run.geo = &geo;
    run.cls = options.runOver;
    run.ctx = options.ctx;
    const TimeHost time(options.ctx);
    run.host = options.host ? options.host : &time;
    run.interrupt = options.interrupt;
    run.inputs = {snapshot.get(), options.inputs[1].get(), options.inputs[2].get(), options.inputs[3].get()};
    Queries queries;
    for (size_t k = 0; k < 4; ++k) queries.in[k].geo = run.inputs[k];
    run.queries = &queries;

    Checked checked;
    if (!check(*impl_->ast, run, &geo, false, true, checked, error)) return false;
    run.functions = &checked.functions;
    if (!bindAll(run, geo, checked, error)) return false;
    run.count = options.runOver == AttrClass::Detail ? 1 : geo.elementCount(options.runOver);

    // Only a group's elements, if one is named -- or those a pattern names
    // (pg/core/Selection.h): numbers, ranges, edges, groups, * and ^.
    std::vector<uint8_t> only;
    if (!options.group.empty() && options.runOver != AttrClass::Detail) {
        const Group* g = snapshot->findGroup(options.group);
        const char* cls = options.runOver == AttrClass::Point ? "point" : options.runOver == AttrClass::Primitive ? "primitive" : "vertex";
        bool named = false;
        if (g && g->classOf() == options.runOver) {
            const auto m = g->mask();
            only.assign(m.begin(), m.end());
            named = true;
        } else if (options.runOver != AttrClass::Vertex) {
            only = selectElements(*snapshot, options.runOver, options.group, &named);
        }
        if (!named) {
            error = std::string("no ") + cls + " group '" + options.group + "'";
            return false;
        }
        only.resize(run.count, 0);
    }
    auto skip = [&](size_t i) { return !only.empty() && !only[i]; };

    std::string log;
    std::vector<std::string> warnings = checked.warnings;
    if (checked.ordered || options.runOver == AttrClass::Detail) {
        Env env(run);
        Deferred deferred;
        deferred.basePoints = geo.pointCount();
        deferred.basePrims = geo.primitiveCount();
        deferred.baseVertices = geo.vertexCount();
        env.deferred = &deferred;
        for (size_t i = 0; i < run.count && !env.stop; ++i) {
            if (skip(i)) continue;
            env.elem = i;
            runMain(checked.main, env);
        }
        merge(report, log, warnings, env.log, env.warnings);
        if (env.stop && !env.error.empty()) {
            error = env.error;
            if (report) {
                report->log = std::move(log);
                report->warnings = std::move(warnings);
            }
            return false;
        }
        apply(deferred, geo, warnings);
    } else {
        const auto chunks = chunkRanges(run.count, 4096);
        struct Part {
            std::string log, error;
            std::vector<std::string> warnings;
        };
        std::vector<Part> parts(chunks.size());
        TaskPool::instance().run(chunks.size(), [&](size_t c) {
            Env env(run);
            for (size_t i = chunks[c].first; i < chunks[c].second && !env.stop; ++i) {
                if (skip(i)) continue;
                env.elem = i;
                runMain(checked.main, env);
            }
            parts[c].log = std::move(env.log);
            parts[c].warnings = std::move(env.warnings);
            if (env.stop) parts[c].error = env.error.empty() ? "stopped" : env.error;
        });
        for (Part& p : parts) {
            merge(report, log, warnings, p.log, p.warnings);
            if (!p.error.empty() && error.empty()) error = p.error;
        }
    }
    if (report) {
        report->log = std::move(log);
        report->warnings = std::move(warnings);
    }
    return error.empty();
}

// --- Expression --------------------------------------------------------------------------------------

struct Expression::Impl {
    std::unique_ptr<Ast> ast;
    Checked checked;
    Run run;  ///< the slots; each evaluation copies it with its host
};

Expression::Expression() : impl_(std::make_unique<Impl>()) {}
Expression::~Expression() = default;

std::unique_ptr<Expression> Expression::parse(const std::string& text, std::string& error) {
    auto ast = lang::parse(text, true, error);
    if (!ast) return nullptr;
    std::unique_ptr<Expression> x(new Expression());
    x->impl_->ast = std::move(ast);
    x->impl_->run.cls = AttrClass::Detail;
    if (!check(*x->impl_->ast, x->impl_->run, nullptr, true, false, x->impl_->checked, error)) return nullptr;
    return x;
}

Type Expression::type() const { return impl_->checked.exprType; }
bool Expression::readsTime() const { return impl_->ast->readsTime; }
const std::vector<Channel>& Expression::channels() const { return impl_->ast->channels; }

namespace {

/// Runs the expression; its value is then in `env` at checked.exprSlot.
bool evaluate(const Expression::Impl& x, const Host& host, Env& env, std::string& error) {
    runMain(x.checked.main, env);
    (void)host;
    if (env.stop) {
        error = env.error.empty() ? "stopped" : env.error;
        return false;
    }
    if (!env.warnings.empty()) {
        error = env.warnings.front();
        return false;
    }
    return true;
}

}  // namespace

bool Expression::evalFloat(const Host& host, double& out, std::string& error) const {
    Run run = impl_->run;
    run.host = &host;
    run.functions = &const_cast<Checked&>(impl_->checked).functions;
    Env env(run);
    if (!evaluate(*impl_, host, env, error)) return false;
    const auto slot = static_cast<size_t>(impl_->checked.exprSlot);
    switch (impl_->checked.exprType) {
        case Type::Int: out = env.i[slot]; return true;
        case Type::Float: out = env.f[slot]; return true;
        case Type::Vec2: out = env.v2[slot].x; return true;
        case Type::Vec3: out = env.v3[slot].x; return true;
        case Type::Vec4: out = env.v4[slot].x; return true;
        case Type::String: out = std::strtod(env.s[slot].c_str(), nullptr); return true;
        default: error = "not a number"; return false;
    }
}

bool Expression::evalVector(const Host& host, Vec3& out, std::string& error) const {
    Run run = impl_->run;
    run.host = &host;
    run.functions = &const_cast<Checked&>(impl_->checked).functions;
    Env env(run);
    if (!evaluate(*impl_, host, env, error)) return false;
    const auto slot = static_cast<size_t>(impl_->checked.exprSlot);
    switch (impl_->checked.exprType) {
        case Type::Int: out = Vec3(static_cast<float>(env.i[slot])); return true;
        case Type::Float: out = Vec3(env.f[slot]); return true;
        case Type::Vec2: out = Vec3(env.v2[slot].x, env.v2[slot].y, 0.0f); return true;
        case Type::Vec3: out = env.v3[slot]; return true;
        case Type::Vec4: out = Vec3(env.v4[slot].x, env.v4[slot].y, env.v4[slot].z); return true;
        default: error = "not a vector"; return false;
    }
}

bool Expression::evalText(const Host& host, std::string& out, std::string& error) const {
    Run run = impl_->run;
    run.host = &host;
    run.functions = &const_cast<Checked&>(impl_->checked).functions;
    Env env(run);
    if (!evaluate(*impl_, host, env, error)) return false;
    const auto slot = static_cast<size_t>(impl_->checked.exprSlot);
    char buf[64];
    switch (impl_->checked.exprType) {
        case Type::String: out = env.s[slot]; return true;
        case Type::Int: out = std::to_string(env.i[slot]); return true;
        case Type::Float: std::snprintf(buf, sizeof buf, "%g", static_cast<double>(env.f[slot])); out = buf; return true;
        default: error = "not text"; return false;
    }
}

}  // namespace pg::lang
