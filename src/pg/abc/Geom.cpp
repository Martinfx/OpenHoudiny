#include "pg/abc/Geom.h"

#include "pg/core/Half.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>

namespace pg::abc {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Schemas, as AbcGeom names them ------------------------------------------------------------

const char* const kXform = "AbcGeom_Xform_v3";
const char* const kPolyMesh = "AbcGeom_PolyMesh_v1";
const char* const kSubD = "AbcGeom_SubD_v1";
const char* const kPoints = "AbcGeom_Points_v1";
const char* const kCurves = "AbcGeom_Curve_v2";
const char* const kCamera = "AbcGeom_Camera_v1";
const char* const kFaceSet = "AbcGeom_FaceSet_v1";
const char* const kGeomBase = "AbcGeom_GeomBase_v1";

/// A geometry parameter's metadata: its scope, what its numbers are.
MetaData geomParam(const std::string& scope, const char* interpretation, int extent) {
    MetaData m = {{"arrayExtent", "1"}, {"geoScope", scope}, {"isGeomParam", "true"},
                  {"podExtent", std::to_string(extent)}, {"podName", "float32_t"}};
    if (interpretation) m["interpretation"] = interpretation;
    return m;
}

/// The box round `P` -- for none, Imath's empty box: the largest numbers
/// the wrong way round.
std::array<double, 6> boxOf(const std::vector<Vec3>& P) {
    std::array<double, 6> box = {DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (const Vec3& p : P) {
        for (int a = 0; a < 3; ++a) {
            box[static_cast<size_t>(a)] = std::min(box[static_cast<size_t>(a)], static_cast<double>(p[a]));
            box[static_cast<size_t>(3 + a)] = std::max(box[static_cast<size_t>(3 + a)], static_cast<double>(p[a]));
        }
    }
    return box;
}

/// Empty samples first, as many as the others have: a property made after
/// `count` samples of them.
void backfill(PropertyWriter& p, size_t count) {
    while (p.samples() < count) p.array(nullptr, 0);
}

/// An optional array of a sample: made the first time there is one, empty
/// in a sample without it.
template <typename Make>
void optional(PropertyWriter*& p, size_t done, const void* data, size_t count, Make make) {
    if (count > 0 && !p) {
        p = &make();
        backfill(*p, done);
    }
    if (p) p->array(count > 0 ? data : nullptr, count);
}

Vec3 normalized(const Vec3& v) {
    const float l = length(v);
    return l > 1e-20f ? v / l : v;
}

}  // namespace

// --- Matrices --------------------------------------------------------------------------------------

Matrix identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            double s = 0.0;
            for (size_t k = 0; k < 4; ++k) s += a[4 * i + k] * b[4 * k + j];
            out[4 * i + j] = s;
        }
    }
    return out;
}

Vec3 transformPoint(const Matrix& m, const Vec3& p) {
    const double x = p.x, y = p.y, z = p.z;
    double o[4];
    for (size_t j = 0; j < 4; ++j) o[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j];
    const double w = std::fabs(o[3]) > 1e-12 ? o[3] : 1.0;
    return Vec3(static_cast<float>(o[0] / w), static_cast<float>(o[1] / w), static_cast<float>(o[2] / w));
}

Vec3 transformDirection(const Matrix& m, const Vec3& d) {
    const double x = d.x, y = d.y, z = d.z;
    return Vec3(static_cast<float>(x * m[0] + y * m[4] + z * m[8]), static_cast<float>(x * m[1] + y * m[5] + z * m[9]),
                static_cast<float>(x * m[2] + y * m[6] + z * m[10]));
}

Matrix pose(const Vec4& q, const Vec3& at) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    // The rows: where the body's own x, y and z go.
    Matrix m = identity();
    m[0] = 1.0 - 2.0 * (y * y + z * z);
    m[1] = 2.0 * (x * y + z * w);
    m[2] = 2.0 * (x * z - y * w);
    m[4] = 2.0 * (x * y - z * w);
    m[5] = 1.0 - 2.0 * (x * x + z * z);
    m[6] = 2.0 * (y * z + x * w);
    m[8] = 2.0 * (x * z + y * w);
    m[9] = 2.0 * (y * z - x * w);
    m[10] = 1.0 - 2.0 * (x * x + y * y);
    m[12] = at.x;
    m[13] = at.y;
    m[14] = at.z;
    return m;
}

MetaData objectMeta(const std::string& schema, const std::string& compound, bool geometry) {
    MetaData m = {{"schema", schema}, {"schemaObjTitle", schema + ":" + compound}};
    if (geometry) m["schemaBaseType"] = kGeomBase;
    return m;
}

// --- Xform -------------------------------------------------------------------------------------------

XformWriter::XformWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling)
    : object_(parent.child(name, objectMeta(kXform, ".xform", false))) {
    xform_ = &object_.properties().compound(".xform", {{"schema", kXform}});
    inherits_ = &xform_->scalar(".inherits", Pod::Bool, 1, timeSampling);
    ops_ = &xform_->scalar(".ops", Pod::U8, 1, timeSampling);
    vals_ = &xform_->scalar(".vals", Pod::F64, 16, timeSampling);
    visible_ = &object_.properties().scalar("visible", Pod::I8, 1, timeSampling);
}

void XformWriter::sample(const Matrix& m, bool visible) {
    const uint8_t yes = 1, matrixOp = 3 << 4;
    const int8_t seen = visible ? 1 : 0;
    inherits_->scalar(&yes);
    ops_->scalar(&matrixOp);
    vals_->scalar(m.data());
    visible_->scalar(&seen);
    if (m != identity()) identity_ = false;
}

void XformWriter::finish() {
    if (identity_ || finished_) return;
    finished_ = true;
    const uint8_t yes = 1;
    xform_->scalar("isNotConstantIdentity", Pod::Bool, 1, 0).scalar(&yes);
}

// --- PolyMesh ----------------------------------------------------------------------------------------

MeshSample meshSample(const Geometry& geo, const Matrix* m, std::span<const uint32_t> only,
                      std::span<const std::string> floats) {
    MeshSample s;
    const auto P = geo.positions();
    const size_t prims = geo.primitiveCount();
    // The closed polygons, and the points they use -- in the points' order.
    std::vector<uint32_t> faces;
    std::vector<int32_t> index(geo.pointCount(), -1);
    auto take = [&](size_t p) {
        if (p >= prims || !geo.primitiveClosed(p) || geo.primitiveVertexCount(p) < 3) return;
        faces.push_back(static_cast<uint32_t>(p));
        for (const uint32_t q : geo.primitivePoints(p)) index[q] = 0;
    };
    if (only.empty()) {
        for (size_t p = 0; p < prims; ++p) take(p);
    } else {
        for (const uint32_t p : only) take(p);
    }
    std::vector<uint32_t> used;
    for (size_t q = 0; q < index.size(); ++q) {
        if (index[q] < 0) continue;
        index[q] = static_cast<int32_t>(used.size());
        used.push_back(static_cast<uint32_t>(q));
        s.P.push_back(m ? transformPoint(*m, P[q]) : P[q]);
    }
    if (faces.empty()) return s;
    // Faces turned round: clockwise, as Alembic winds them.
    for (const uint32_t f : faces) {
        const auto pts = geo.primitivePoints(f);
        s.counts.push_back(static_cast<int32_t>(pts.size()));
        for (size_t k = pts.size(); k-- > 0;) s.indices.push_back(index[pts[k]]);
    }
    // A value a corner, in the same order.
    auto corners = [&](const AttributeArray* vertex, const AttributeArray* point, auto put) {
        for (const uint32_t f : faces) {
            const size_t start = geo.primitiveVertexStart(f), n = geo.primitiveVertexCount(f);
            for (size_t k = n; k-- > 0;) put(vertex ? start + k : geo.vertexPoint(start + k), vertex != nullptr);
        }
        (void)point;
    };
    auto vec3 = [](const AttributeArray* a) { return a && a->type() == AttrType::Vec3 ? a : nullptr; };
    const AttributeArray* vN = vec3(geo.vertices().find("N"));
    const AttributeArray* pN = vec3(geo.points().find("N"));
    if (vN || pN) {
        const auto values = (vN ? vN : pN)->read<Vec3>();
        corners(vN, pN, [&](size_t i, bool) {
            const Vec3 n = values[i];
            s.N.push_back(m ? normalized(transformDirection(*m, n)) : n);
        });
    }
    auto uvOf = [](const AttributeArray* a) {
        return a && (a->type() == AttrType::Vec2 || a->type() == AttrType::Vec3) ? a : nullptr;
    };
    const AttributeArray* vuv = uvOf(geo.vertices().find("uv"));
    const AttributeArray* puv = uvOf(geo.points().find("uv"));
    if (vuv || puv) {
        const AttributeArray* a = vuv ? vuv : puv;
        corners(vuv, puv, [&](size_t i, bool) {
            if (a->type() == AttrType::Vec2) {
                const Vec2 t = a->read<Vec2>()[i];
                s.uv.push_back({t.x, t.y});
            } else {
                const Vec3 t = a->read<Vec3>()[i];
                s.uv.push_back({t.x, t.y});
            }
        });
    }
    if (const AttributeArray* v = vec3(geo.points().find("v"))) {
        const auto values = v->read<Vec3>();
        for (const uint32_t q : used) s.v.push_back(m ? transformDirection(*m, values[q]) : values[q]);
    }
    if (const AttributeArray* c = vec3(geo.points().find("Cd"))) {
        const auto values = c->read<Vec3>();
        for (const uint32_t q : used) s.Cd.push_back(values[q]);
        s.colorScope = "vtx";
    } else if (const AttributeArray* vc = vec3(geo.vertices().find("Cd"))) {
        const auto values = vc->read<Vec3>();
        corners(vc, nullptr, [&](size_t i, bool) { s.Cd.push_back(values[i]); });
        s.colorScope = "fvr";
    } else if (const AttributeArray* pc = vec3(geo.primitives().find("Cd"))) {
        // A face's colour on each of its corners: what every reader takes
        // (Blender reads no colour per face).
        const auto values = pc->read<Vec3>();
        for (const uint32_t f : faces) s.Cd.insert(s.Cd.end(), geo.primitiveVertexCount(f), values[f]);
        s.colorScope = "fvr";
    } else if (const AttributeArray* dc = vec3(geo.detail().find("Cd")); dc && dc->size() > 0) {
        s.Cd.assign(s.indices.size(), dc->read<Vec3>()[0]);
        s.colorScope = "fvr";
    }
    for (const std::string& name : floats) {
        const AttributeArray* a = geo.points().find(name);
        if (!a || a->type() != AttrType::Float) continue;
        const auto values = a->read<float>();
        std::vector<float> out;
        out.reserve(used.size());
        for (const uint32_t q : used) out.push_back(values[q]);
        s.pointFloats.emplace_back(name, std::move(out));
    }
    return s;
}

MeshWriter::MeshWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling)
    : object_(parent.child(name, objectMeta(kPolyMesh, ".geom", true))),
      geom_(object_.properties().compound(".geom", {{"schema", kPolyMesh}, {"schemaBaseType", kGeomBase}})),
      timeSampling_(timeSampling) {
    bounds_ = &geom_.scalar(".selfBnds", Pod::F64, 6, timeSampling, {{"interpretation", "box"}});
    P_ = &geom_.array("P", Pod::F32, 3, timeSampling, {{"geoScope", "vtx"}, {"interpretation", "point"}});
    indices_ = &geom_.array(".faceIndices", Pod::I32, 1, timeSampling);
    counts_ = &geom_.array(".faceCounts", Pod::I32, 1, timeSampling);
}

PropertyWriter& MeshWriter::arb(PropertyWriter*& p, const std::string& name, Pod pod, uint8_t extent, MetaData meta,
                                CompoundWriter& in) {
    if (!p) p = &in.array(name, pod, extent, timeSampling_, std::move(meta));
    return *p;
}

void MeshWriter::sample(const MeshSample& s) {
    const std::array<double, 6> box = boxOf(s.P);
    bounds_->scalar(box.data());
    P_->array(s.P.data(), s.P.size());
    indices_->array(s.indices.data(), s.indices.size());
    counts_->array(s.counts.data(), s.counts.size());
    const size_t corners = s.indices.size();
    optional(N_, samples_, s.N.data(), s.N.size() == corners ? s.N.size() : 0,
             [&]() -> PropertyWriter& { return arb(N_, "N", Pod::F32, 3, geomParam("fvr", "normal", 3), geom_); });
    optional(uv_, samples_, s.uv.data(), s.uv.size() == corners ? s.uv.size() : 0,
             [&]() -> PropertyWriter& { return arb(uv_, "uv", Pod::F32, 2, geomParam("fvr", "vector", 2), geom_); });
    optional(v_, samples_, s.v.data(), s.v.size() == s.P.size() ? s.v.size() : 0, [&]() -> PropertyWriter& {
        return arb(v_, ".velocities", Pod::F32, 3, {{"interpretation", "vector"}}, geom_);
    });
    // Colours keep the scope they first came in.
    const size_t want = s.colorScope == "fvr" ? corners : s.colorScope == "uni" ? s.counts.size() : s.P.size();
    const bool colours = !s.Cd.empty() && s.Cd.size() == want && (colorScope_.empty() || colorScope_ == s.colorScope);
    optional(Cd_, samples_, s.Cd.data(), colours ? s.Cd.size() : 0, [&]() -> PropertyWriter& {
        colorScope_ = s.colorScope;
        if (!params_) params_ = &geom_.compound(".arbGeomParams");
        return arb(Cd_, "Cd", Pod::F32, 3, geomParam(s.colorScope, "rgb", 3), *params_);
    });
    // The other numbers a point: each its own, empty where a sample lacks it.
    for (auto& [name, p] : floats_) {
        const auto it = std::find_if(s.pointFloats.begin(), s.pointFloats.end(), [&](const auto& f) { return f.first == name; });
        const bool fits = it != s.pointFloats.end() && it->second.size() == s.P.size();
        p->array(fits ? it->second.data() : nullptr, fits ? it->second.size() : 0);
    }
    for (const auto& [name, values] : s.pointFloats) {
        if (values.size() != s.P.size() || name.empty()) continue;
        if (std::any_of(floats_.begin(), floats_.end(), [&](const auto& f) { return f.first == name; })) continue;
        if (!params_) params_ = &geom_.compound(".arbGeomParams");
        PropertyWriter& p = params_->array(name, Pod::F32, 1, timeSampling_, geomParam("vtx", nullptr, 1));
        backfill(p, samples_);
        p.array(values.data(), values.size());
        floats_.emplace_back(name, &p);
    }
    ++samples_;
}

// --- FaceSet ----------------------------------------------------------------------------------------

FaceSetWriter::FaceSetWriter(ObjectWriter& mesh, const std::string& name, uint32_t timeSampling) {
    ObjectWriter& o = mesh.child(name, objectMeta(kFaceSet, ".faceset", true));
    CompoundWriter& set = o.properties().compound(".faceset", {{"schema", kFaceSet}, {"schemaBaseType", kGeomBase}});
    bounds_ = &set.scalar(".selfBnds", Pod::F64, 6, timeSampling, {{"interpretation", "box"}});
    faces_ = &set.array(".faces", Pod::I32, 1, timeSampling);
}

void FaceSetWriter::sample(std::span<const int32_t> faces) {
    const std::array<double, 6> empty = {DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX};
    bounds_->scalar(empty.data());
    faces_->array(faces.data(), faces.size());
}

// --- Points --------------------------------------------------------------------------------------------

PointsSample pointsSample(const Geometry& geo, const Matrix* m) {
    PointsSample s;
    std::vector<uint8_t> used(geo.pointCount(), 0);
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        for (const uint32_t q : geo.primitivePoints(p)) used[q] = 1;
    }
    const auto P = geo.positions();
    const AttributeArray* id = geo.points().find("id");
    const AttributeArray* v = geo.points().find("v");
    const AttributeArray* pscale = geo.points().find("pscale");
    const AttributeArray* cd = geo.points().find("Cd");
    for (size_t q = 0; q < used.size(); ++q) {
        if (used[q]) continue;
        s.P.push_back(m ? transformPoint(*m, P[q]) : P[q]);
        s.ids.push_back(id && id->type() == AttrType::Int ? static_cast<uint64_t>(static_cast<uint32_t>(id->read<int32_t>()[q]))
                                                           : static_cast<uint64_t>(q));
        if (v && v->type() == AttrType::Vec3) s.v.push_back(m ? transformDirection(*m, v->read<Vec3>()[q]) : v->read<Vec3>()[q]);
        if (pscale && pscale->type() == AttrType::Float) s.widths.push_back(2.0f * pscale->read<float>()[q]);
        if (cd && cd->type() == AttrType::Vec3) s.Cd.push_back(cd->read<Vec3>()[q]);
    }
    return s;
}

PointsWriter::PointsWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling)
    : object_(parent.child(name, objectMeta(kPoints, ".geom", true))),
      geom_(object_.properties().compound(".geom", {{"schema", kPoints}, {"schemaBaseType", kGeomBase}})),
      timeSampling_(timeSampling) {
    bounds_ = &geom_.scalar(".selfBnds", Pod::F64, 6, timeSampling, {{"interpretation", "box"}});
    P_ = &geom_.array("P", Pod::F32, 3, timeSampling, {{"geoScope", "var"}, {"interpretation", "point"}});
    ids_ = &geom_.array(".pointIds", Pod::U64, 1, timeSampling, {{"geoScope", "var"}});
}

void PointsWriter::sample(const PointsSample& s) {
    const std::array<double, 6> box = boxOf(s.P);
    bounds_->scalar(box.data());
    P_->array(s.P.data(), s.P.size());
    std::vector<uint64_t> ids = s.ids;
    if (ids.size() != s.P.size()) {
        ids.resize(s.P.size());
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    }
    ids_->array(ids.data(), ids.size());
    const size_t n = s.P.size();
    optional(v_, samples_, s.v.data(), s.v.size() == n ? n : 0, [&]() -> PropertyWriter& {
        return geom_.array(".velocities", Pod::F32, 3, timeSampling_, {{"interpretation", "vector"}});
    });
    optional(widths_, samples_, s.widths.data(), s.widths.size() == n ? n : 0, [&]() -> PropertyWriter& {
        return geom_.array(".widths", Pod::F32, 1, timeSampling_, geomParam("vtx", nullptr, 1));
    });
    optional(Cd_, samples_, s.Cd.data(), s.Cd.size() == n ? n : 0, [&]() -> PropertyWriter& {
        if (!params_) params_ = &geom_.compound(".arbGeomParams");
        return params_->array("Cd", Pod::F32, 3, timeSampling_, geomParam("var", "rgb", 3));
    });
    ++samples_;
}

// --- Curves --------------------------------------------------------------------------------------------

CurvesSample curvesSample(const Geometry& geo, const Matrix* m) {
    CurvesSample s;
    const auto P = geo.positions();
    const AttributeArray* pscale = geo.points().find("pscale");
    const AttributeArray* width = geo.points().find("width");
    const AttributeArray* cd = geo.points().find("Cd");
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        if (geo.primitiveClosed(p) || geo.primitiveVertexCount(p) < 2) continue;
        const auto pts = geo.primitivePoints(p);
        s.counts.push_back(static_cast<int32_t>(pts.size()));
        for (const uint32_t q : pts) {
            s.P.push_back(m ? transformPoint(*m, P[q]) : P[q]);
            if (width && width->type() == AttrType::Float) s.widths.push_back(width->read<float>()[q]);
            else if (pscale && pscale->type() == AttrType::Float) s.widths.push_back(2.0f * pscale->read<float>()[q]);
            if (cd && cd->type() == AttrType::Vec3) s.Cd.push_back(cd->read<Vec3>()[q]);
        }
    }
    return s;
}

CurvesWriter::CurvesWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling)
    : object_(parent.child(name, objectMeta(kCurves, ".geom", true))),
      geom_(object_.properties().compound(".geom", {{"schema", kCurves}, {"schemaBaseType", kGeomBase}})),
      timeSampling_(timeSampling) {
    bounds_ = &geom_.scalar(".selfBnds", Pod::F64, 6, timeSampling, {{"interpretation", "box"}});
    P_ = &geom_.array("P", Pod::F32, 3, timeSampling, {{"geoScope", "vtx"}, {"interpretation", "point"}});
    counts_ = &geom_.array("nVertices", Pod::I32, 1, timeSampling);
    type_ = &geom_.scalar("curveBasisAndType", Pod::U8, 4, timeSampling);
}

void CurvesWriter::sample(const CurvesSample& s) {
    const std::array<double, 6> box = boxOf(s.P);
    bounds_->scalar(box.data());
    P_->array(s.P.data(), s.P.size());
    counts_->array(s.counts.data(), s.counts.size());
    // Linear, not periodic, no basis.
    const uint8_t type[4] = {1, 0, 0, 1};
    type_->scalar(type);
    const size_t n = s.P.size();
    optional(widths_, samples_, s.widths.data(), s.widths.size() == n ? n : 0, [&]() -> PropertyWriter& {
        return geom_.array("width", Pod::F32, 1, timeSampling_, geomParam("vtx", nullptr, 1));
    });
    optional(Cd_, samples_, s.Cd.data(), s.Cd.size() == n ? n : 0, [&]() -> PropertyWriter& {
        if (!params_) params_ = &geom_.compound(".arbGeomParams");
        return params_->array("Cd", Pod::F32, 3, timeSampling_, geomParam("vtx", "rgb", 3));
    });
    ++samples_;
}

// --- Camera ----------------------------------------------------------------------------------------------

std::array<double, 16> Lens::core() const {
    return {focalLength, horizontalAperture, horizontalOffset, verticalAperture, verticalOffset, 1.0, 0.0, 0.0, 0.0, 0.0,
            fStop,       focusDistance,      shutterOpen,      shutterClose,     nearClip,       farClip};
}

Lens Lens::of(const double c[16]) {
    Lens l;
    l.focalLength = c[0];
    l.horizontalAperture = c[1];
    l.horizontalOffset = c[2];
    l.verticalAperture = c[3];
    l.verticalOffset = c[4];
    l.fStop = c[10];
    l.focusDistance = c[11];
    l.shutterOpen = c[12];
    l.shutterClose = c[13];
    l.nearClip = c[14];
    l.farClip = c[15];
    return l;
}

CameraWriter::CameraWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling) {
    ObjectWriter& o = parent.child(name, objectMeta(kCamera, ".geom", false));
    core_ = &o.properties().compound(".geom", {{"schema", kCamera}}).scalar(".core", Pod::F64, 16, timeSampling);
}

void CameraWriter::sample(const Lens& lens) {
    const std::array<double, 16> c = lens.core();
    core_->scalar(c.data());
}

// --- Reading ---------------------------------------------------------------------------------------------

namespace {

std::string schemaOf(const ObjectReader& o) {
    const auto it = o.meta.find("schema");
    return it != o.meta.end() ? it->second : std::string();
}

/// A sample's numbers as doubles, whatever their type; none for strings.
std::vector<double> numbers(const Sample& s, Pod pod) {
    const size_t size = podSize(pod);
    std::vector<double> out;
    if (size == 0) return out;
    const size_t n = s.bytes.size() / size;
    out.resize(n);
    const uint8_t* b = s.bytes.data();
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = b + i * size;
        switch (pod) {
            case Pod::Bool:
            case Pod::U8: out[i] = p[0]; break;
            case Pod::I8: out[i] = static_cast<int8_t>(p[0]); break;
            case Pod::U16: {
                uint16_t v;
                std::memcpy(&v, p, 2);
                out[i] = v;
                break;
            }
            case Pod::I16: {
                int16_t v;
                std::memcpy(&v, p, 2);
                out[i] = v;
                break;
            }
            case Pod::U32: {
                uint32_t v;
                std::memcpy(&v, p, 4);
                out[i] = v;
                break;
            }
            case Pod::I32: {
                int32_t v;
                std::memcpy(&v, p, 4);
                out[i] = v;
                break;
            }
            case Pod::U64: {
                uint64_t v;
                std::memcpy(&v, p, 8);
                out[i] = static_cast<double>(v);
                break;
            }
            case Pod::I64: {
                int64_t v;
                std::memcpy(&v, p, 8);
                out[i] = static_cast<double>(v);
                break;
            }
            case Pod::F16: {
                uint16_t v;
                std::memcpy(&v, p, 2);
                out[i] = floatFromHalf(v);
                break;
            }
            case Pod::F32: {
                float v;
                std::memcpy(&v, p, 4);
                out[i] = v;
                break;
            }
            case Pod::F64: {
                double v;
                std::memcpy(&v, p, 8);
                out[i] = v;
                break;
            }
            default: out[i] = 0.0;
        }
    }
    return out;
}

/// A property's numbers at `time`: between the samples round it, blended
/// when `blend` and the two have as many; else the one at or before it.
bool numbersAt(const ArchiveReader& a, const PropertyReader& p, double time, bool blend, std::vector<double>& out,
               std::string& error) {
    out.clear();
    if (p.header.samples == 0 || p.is(PropertyType::Compound)) return false;
    size_t lo = 0, hi = 0;
    double t = 0.0;
    a.timeSamplingOf(p).bracket(time, p.header.samples, lo, hi, t);
    if (p.header.constant()) lo = hi = 0;
    Sample s;
    if (!a.read(p, lo, s, error)) return false;
    out = numbers(s, p.header.pod);
    if (!blend || hi == lo || t <= 0.0 || p.header.stored(lo) == p.header.stored(hi)) return true;
    if (!a.read(p, hi, s, error)) return true;
    const std::vector<double> next = numbers(s, p.header.pod);
    if (next.size() != out.size()) return true;
    for (size_t i = 0; i < out.size(); ++i) out[i] += (next[i] - out[i]) * t;
    return true;
}

/// The axis-angle rotation Imath's setAxisAngle makes: rows, p x M.
Matrix rotation(double x, double y, double z, double degrees) {
    const double l = std::sqrt(x * x + y * y + z * z);
    Matrix m = identity();
    if (!(l > 0.0)) return m;
    x /= l, y /= l, z /= l;
    const double r = degrees * kPi / 180.0, s = std::sin(r), c = std::cos(r), t = 1.0 - c;
    m[0] = x * x * t + c;
    m[1] = x * y * t + z * s;
    m[2] = x * z * t - y * s;
    m[4] = x * y * t - z * s;
    m[5] = y * y * t + c;
    m[6] = y * z * t + x * s;
    m[8] = x * z * t + y * s;
    m[9] = y * z * t - x * s;
    m[10] = z * z * t + c;
    return m;
}

/// An Xform object's own transform at `time`; whether it takes its
/// parents' too.
Matrix localTransform(const ArchiveReader& a, const ObjectReader& o, double time, bool& inherits) {
    inherits = true;
    const PropertyReader* x = o.properties.find(".xform");
    if (!x || !x->is(PropertyType::Compound)) return identity();
    std::string error;
    std::vector<double> vals, ops, flag;
    if (const PropertyReader* p = x->find(".inherits"); p && numbersAt(a, *p, time, false, flag, error) && !flag.empty()) {
        inherits = flag[0] != 0.0;
    }
    if (const PropertyReader* p = x->find(".vals")) numbersAt(a, *p, time, true, vals, error);
    if (const PropertyReader* p = x->find(".ops")) numbersAt(a, *p, time, false, ops, error);
    if (ops.empty() && vals.size() == 16) ops = {3 << 4};
    Matrix m = identity();
    size_t k = 0;
    for (const double code : ops) {
        const int type = static_cast<int>(code) >> 4;
        const size_t need = type == 0 || type == 1 ? 3 : type == 2 ? 4 : type == 3 ? 16 : type <= 6 ? 1 : 0;
        if (need == 0 || k + need > vals.size()) break;
        const double* v = vals.data() + k;
        Matrix op = identity();
        switch (type) {
            case 0: op[0] = v[0], op[5] = v[1], op[10] = v[2]; break;
            case 1: op[12] = v[0], op[13] = v[1], op[14] = v[2]; break;
            case 2: op = rotation(v[0], v[1], v[2], v[3]); break;
            case 3: std::copy(v, v + 16, op.begin()); break;
            case 4: op = rotation(1, 0, 0, v[0]); break;
            case 5: op = rotation(0, 1, 0, v[0]); break;
            case 6: op = rotation(0, 0, 1, v[0]); break;
            default: break;
        }
        // The first op the outermost: each one comes before those listed before it.
        m = multiply(op, m);
        k += need;
    }
    return m;
}

bool isGeometry(const std::string& schema) {
    return schema == kPolyMesh || schema == kSubD || schema == kPoints || schema == kCurves;
}

bool under(const std::string& path, const std::string& root) {
    if (root.empty() || root == "/") return true;
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

bool varies(const PropertyReader& p) {
    if (p.is(PropertyType::Compound)) {
        for (const PropertyReader& c : p.children) {
            if (varies(c)) return true;
        }
        return false;
    }
    return p.header.samples > 1 && !p.header.constant();
}

/// Whether its transform or visibility -- or an object's above it -- changes.
bool placeVaries(const ObjectReader& o) {
    for (const ObjectReader* x = &o; x; x = x->parent) {
        if (const PropertyReader* p = x->properties.find(".xform"); p && varies(*p)) return true;
        if (const PropertyReader* p = x->properties.find("visible"); p && varies(*p)) return true;
    }
    return false;
}

/// What is read into the geometry, and the attributes made as it goes:
/// each padded to its element count at the end.
struct Builder {
    Geometry geo;
    struct Attr {
        AttrClass cls;
        int width = 1;  // 0: integers
        std::vector<float> floats;
        std::vector<int32_t> ints;
    };
    std::map<std::pair<int, std::string>, Attr> attrs;
    std::vector<std::string> paths;  // each primitive's object
    std::map<std::string, std::vector<uint32_t>> groups;

    Attr& attr(AttrClass cls, const std::string& name, int width) {
        Attr& a = attrs[{static_cast<int>(cls), name}];
        if (a.floats.empty() && a.ints.empty()) {
            a.cls = cls;
            a.width = width;
        }
        return a;
    }
    /// The values of elements [start, start + n) -- `extent` numbers each in
    /// `values` by `source[i]` -- into the attribute; padded before.
    void set(AttrClass cls, const std::string& name, int width, size_t start, size_t n, const std::vector<double>& values,
             int extent, const std::vector<uint32_t>& source) {
        Attr& a = attr(cls, name, width);
        if (a.width != width) return;  // the same name, another kind: the first stays
        const size_t w = static_cast<size_t>(std::max(width, 1));
        if (width == 0) {
            a.ints.resize(start + n, 0);
        } else {
            a.floats.resize((start + n) * w, 0.0f);
        }
        for (size_t i = 0; i < n; ++i) {
            const size_t e = source.empty() ? i : source[i];
            if ((e + 1) * static_cast<size_t>(extent) > values.size()) continue;
            const double* x = values.data() + e * static_cast<size_t>(extent);
            if (width == 0) {
                a.ints[start + i] = static_cast<int32_t>(x[0]);
            } else {
                for (size_t k = 0; k < w; ++k) {
                    a.floats[(start + i) * w + k] = k < static_cast<size_t>(extent) ? static_cast<float>(x[k]) : 0.0f;
                }
            }
        }
    }
    std::shared_ptr<Geometry> finish(bool pathAttribute) {
        for (auto& [key, a] : attrs) {
            const size_t n = geo.elementCount(a.cls);
            AttributeSet& set = geo.attributes(a.cls);
            if (key.second == "P" && a.cls == AttrClass::Point) continue;
            if (a.width == 0) {
                a.ints.resize(n, 0);
                auto out = set.create(key.second, AttrType::Int).write<int32_t>();
                std::copy(a.ints.begin(), a.ints.end(), out.begin());
                continue;
            }
            const AttrType type = a.width == 1   ? AttrType::Float
                                  : a.width == 2 ? AttrType::Vec2
                                  : a.width == 3 ? AttrType::Vec3
                                                 : AttrType::Vec4;
            a.floats.resize(n * static_cast<size_t>(a.width), 0.0f);
            AttributeArray& arr = set.create(key.second, type);
            const float* f = a.floats.data();
            if (type == AttrType::Float) {
                auto out = arr.write<float>();
                std::copy(a.floats.begin(), a.floats.end(), out.begin());
            } else if (type == AttrType::Vec2) {
                auto out = arr.write<Vec2>();
                for (size_t i = 0; i < out.size(); ++i) out[i] = Vec2(f[2 * i], f[2 * i + 1]);
            } else if (type == AttrType::Vec3) {
                auto out = arr.write<Vec3>();
                for (size_t i = 0; i < out.size(); ++i) out[i] = Vec3(f[3 * i], f[3 * i + 1], f[3 * i + 2]);
            } else {
                auto out = arr.write<Vec4>();
                for (size_t i = 0; i < out.size(); ++i) out[i] = Vec4(f[4 * i], f[4 * i + 1], f[4 * i + 2], f[4 * i + 3]);
            }
        }
        if (pathAttribute && geo.primitiveCount() > 0) {
            std::map<std::string, std::vector<uint8_t>> masks;
            for (size_t p = 0; p < paths.size(); ++p) {
                std::vector<uint8_t>& m = masks[paths[p]];
                if (m.empty()) m.assign(geo.primitiveCount(), 0);
                m[p] = 1;
            }
            for (const auto& [path, mask] : masks) setPrimitiveString(geo, "path", path, mask);
        }
        for (const auto& [name, prims] : groups) {
            Group& g = geo.createGroup(name, AttrClass::Primitive);
            g.resize(geo.primitiveCount());
            for (const uint32_t p : prims) g.set(p, true);
        }
        return std::make_shared<Geometry>(std::move(geo));
    }
};

/// A group's name from a face set's: letters, digits and underscores.
std::string groupName(const std::string& s) {
    std::string out;
    for (const char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

/// A geometry parameter of the schema -- an array, or a compound of
/// .vals and .indices -- at `time`: its numbers, how many each, its scope.
struct Param {
    std::vector<double> values;
    int extent = 0;
    std::string scope;
    bool integral = false;
};

bool paramAt(const ArchiveReader& a, const PropertyReader& p, double time, bool blend, Param& out, std::string& error) {
    out = Param();
    const PropertyReader* vals = &p;
    const PropertyReader* indices = nullptr;
    if (p.is(PropertyType::Compound)) {
        vals = p.find(".vals");
        indices = p.find(".indices");
        if (!vals || vals->is(PropertyType::Compound)) return false;
    }
    const auto scope = p.header.meta.find("geoScope");
    out.scope = scope != p.header.meta.end() ? scope->second : (vals->header.meta.count("geoScope") ? vals->header.meta.at("geoScope") : "");
    out.extent = std::max<int>(vals->header.extent, 1);
    out.integral = vals->header.pod != Pod::F16 && vals->header.pod != Pod::F32 && vals->header.pod != Pod::F64;
    if (vals->header.pod == Pod::String || vals->header.pod == Pod::WString) return false;
    if (!numbersAt(a, *vals, time, blend && !indices, out.values, error)) return false;
    if (indices) {
        std::vector<double> idx;
        if (!numbersAt(a, *indices, time, false, idx, error)) return false;
        std::vector<double> expanded;
        expanded.reserve(idx.size() * static_cast<size_t>(out.extent));
        const size_t count = out.values.size() / static_cast<size_t>(out.extent);
        for (const double d : idx) {
            const size_t i = d >= 0.0 ? static_cast<size_t>(d) : count;
            for (int k = 0; k < out.extent; ++k) expanded.push_back(i < count ? out.values[i * static_cast<size_t>(out.extent) + static_cast<size_t>(k)] : 0.0);
        }
        out.values = std::move(expanded);
    }
    return true;
}

class Importer {
public:
    Importer(const ArchiveReader& a, double time, const ImportOptions& o, std::vector<std::string>* skipped)
        : a_(a), time_(time), options_(o), skipped_(skipped) {}

    void object(const ObjectReader& o) {
        const std::string schema = schemaOf(o);
        if (!isGeometry(schema)) return;
        if (!options_.hidden && !visible(a_, o, time_)) return;
        const PropertyReader* geom = o.properties.find(".geom");
        if (!geom || !geom->is(PropertyType::Compound)) {
            note(o, "no .geom properties");
            return;
        }
        world_ = worldTransform(a_, o, time_);
        if (schema == kPoints) points(o, *geom);
        else if (schema == kCurves) curves(o, *geom);
        else mesh(o, *geom);
    }

    std::shared_ptr<Geometry> finish() { return b_.finish(options_.pathAttribute); }

private:
    void note(const ObjectReader& o, const std::string& why) {
        if (skipped_) skipped_->push_back(o.path + ": " + why);
    }

    /// P at the time, in the world; false (said why) when there is none.
    bool positions(const ObjectReader& o, const PropertyReader& geom, std::vector<Vec3>& out) {
        out.clear();
        const PropertyReader* P = geom.find("P");
        std::vector<double> v;
        std::string error;
        if (!P || !numbersAt(a_, *P, time_, true, v, error) || P->header.extent != 3) {
            note(o, error.empty() ? "no points (P)" : error);
            return false;
        }
        out.resize(v.size() / 3);
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = transformPoint(world_, Vec3(static_cast<float>(v[3 * i]), static_cast<float>(v[3 * i + 1]), static_cast<float>(v[3 * i + 2])));
        }
        return true;
    }

    size_t addPoints(const std::vector<Vec3>& P) {
        const size_t start = b_.geo.addPoints(P.size());
        auto out = b_.geo.positionsForWrite();
        std::copy(P.begin(), P.end(), out.begin() + static_cast<std::ptrdiff_t>(start));
        return start;
    }

    /// A parameter onto what this object added: `points`, `corners` (the
    /// file's corner of each corner made) or `faces` (the file's face of
    /// each face made).
    void param(const std::string& name, const Param& p, size_t pointStart, size_t points, size_t vertexStart,
               const std::vector<uint32_t>& corners, size_t primStart, const std::vector<uint32_t>& faces,
               bool direction, bool normal) {
        if (p.values.empty() || p.extent < 1 || p.extent > 4) return;
        std::vector<double> values = p.values;
        if (direction && p.extent == 3) {
            for (size_t i = 0; i + 2 < values.size(); i += 3) {
                Vec3 d = transformDirection(world_, Vec3(static_cast<float>(values[i]), static_cast<float>(values[i + 1]), static_cast<float>(values[i + 2])));
                if (normal) d = normalized(d);
                values[i] = d.x, values[i + 1] = d.y, values[i + 2] = d.z;
            }
        }
        const size_t count = values.size() / static_cast<size_t>(p.extent);
        const int width = p.integral && p.extent == 1 ? 0 : p.extent;
        if (p.scope == "fvr" && count == fileCorners_) {
            b_.set(AttrClass::Vertex, name, width, vertexStart, corners.size(), values, p.extent, corners);
        } else if ((p.scope == "vtx" || p.scope == "var" || p.scope.empty() || p.scope == "fvr") && count == points) {
            std::vector<uint32_t> none;
            b_.set(AttrClass::Point, name, width, pointStart, points, values, p.extent, none);
        } else if (p.scope == "uni" && count == fileFaces_) {
            b_.set(AttrClass::Primitive, name, width, primStart, faces.size(), values, p.extent, faces);
        } else if (p.scope == "con" || count == 1) {
            std::vector<uint32_t> zeros(faces.empty() ? points : faces.size(), 0);
            if (faces.empty()) b_.set(AttrClass::Point, name, width, pointStart, points, values, p.extent, zeros);
            else b_.set(AttrClass::Primitive, name, width, primStart, faces.size(), values, p.extent, zeros);
        }
    }

    void arbitrary(const PropertyReader& geom, size_t pointStart, size_t points, size_t vertexStart,
                   const std::vector<uint32_t>& corners, size_t primStart, const std::vector<uint32_t>& faces) {
        const PropertyReader* arb = geom.find(".arbGeomParams");
        if (!arb || !arb->is(PropertyType::Compound)) return;
        for (const PropertyReader& p : arb->children) {
            Param v;
            std::string error;
            if (!paramAt(a_, p, time_, true, v, error)) continue;
            std::string name = p.header.name;
            if (name == "Cs" || name == "displayColor" || name == "color") name = "Cd";
            if (name == "P") continue;
            param(name, v, pointStart, points, vertexStart, corners, primStart, faces, false, false);
        }
    }

    void mesh(const ObjectReader& o, const PropertyReader& geom) {
        std::vector<Vec3> P;
        if (!positions(o, geom, P)) return;
        std::vector<double> counts, indices;
        std::string error;
        const PropertyReader* fc = geom.find(".faceCounts");
        const PropertyReader* fi = geom.find(".faceIndices");
        if (!fc || !fi || !numbersAt(a_, *fc, time_, false, counts, error) || !numbersAt(a_, *fi, time_, false, indices, error)) {
            note(o, error.empty() ? "a mesh without faces" : error);
            return;
        }
        const bool flip = [&] {
            // A mirroring transform turns the faces round again.
            const double* m = world_.data();
            const double det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
            return det < 0.0;
        }();
        const size_t pointStart = addPoints(P);
        const size_t vertexStart = b_.geo.vertexCount(), primStart = b_.geo.primitiveCount();
        std::vector<uint32_t> ring, all, sizes, corners, faces;
        std::vector<int64_t> made(counts.size(), -1);  // the file's face -> the face made
        size_t corner = 0, bad = 0;
        for (size_t f = 0; f < counts.size(); ++f) {
            const size_t c = counts[f] > 0.0 ? static_cast<size_t>(counts[f]) : 0;
            if (corner + c > indices.size()) {
                note(o, "fewer face indices than the faces need");
                break;
            }
            bool ok = c >= 3;
            ring.clear();
            for (size_t k = 0; k < c && ok; ++k) {
                // Wound back the other way round (unless mirrored).
                const size_t at = corner + (flip ? k : c - 1 - k);
                const double i = indices[at];
                if (!(i >= 0.0) || i >= static_cast<double>(P.size())) ok = false;
                else ring.push_back(static_cast<uint32_t>(pointStart + static_cast<size_t>(i)));
            }
            if (ok) {
                made[f] = static_cast<int64_t>(primStart + faces.size());
                faces.push_back(static_cast<uint32_t>(f));
                all.insert(all.end(), ring.begin(), ring.end());
                sizes.push_back(static_cast<uint32_t>(c));
                for (size_t k = 0; k < c; ++k) corners.push_back(static_cast<uint32_t>(corner + (flip ? k : c - 1 - k)));
            } else if (c >= 3) {
                ++bad;
            }
            corner += c;
        }
        // The file's faces and corners: what a value a face or a corner is
        // counted against.
        fileFaces_ = counts.size();
        fileCorners_ = indices.size();
        if (bad > 0) note(o, std::to_string(bad) + " faces with points not there");
        const uint8_t closed = 1;
        if (!sizes.empty()) b_.geo.addPrimitives(all, sizes, std::span<const uint8_t>(&closed, 1));
        b_.paths.resize(b_.geo.primitiveCount(), o.path);
        // Normals, uv, velocities, the rest.
        Param p;
        if (const PropertyReader* N = geom.find("N"); N && paramAt(a_, *N, time_, true, p, error)) {
            param("N", p, pointStart, P.size(), vertexStart, corners, primStart, faces, true, true);
        }
        if (const PropertyReader* uv = geom.find("uv"); uv && paramAt(a_, *uv, time_, false, p, error)) {
            // As Houdini keeps it: u, v, 0.
            Param uvw = p;
            if (p.extent == 2) {
                uvw.values.clear();
                for (size_t i = 0; i + 1 < p.values.size(); i += 2) uvw.values.insert(uvw.values.end(), {p.values[i], p.values[i + 1], 0.0});
                uvw.extent = 3;
            }
            param("uv", uvw, pointStart, P.size(), vertexStart, corners, primStart, faces, false, false);
        }
        if (const PropertyReader* v = geom.find(".velocities"); v && paramAt(a_, *v, time_, true, p, error)) {
            if (p.scope.empty()) p.scope = "vtx";
            param("v", p, pointStart, P.size(), vertexStart, corners, primStart, faces, true, false);
        }
        arbitrary(geom, pointStart, P.size(), vertexStart, corners, primStart, faces);
        // The face sets under it: groups.
        if (!options_.faceSets) return;
        for (const auto& child : o.children) {
            if (schemaOf(*child) != kFaceSet) continue;
            const PropertyReader* set = child->properties.find(".faceset");
            const PropertyReader* f = set ? set->find(".faces") : nullptr;
            std::vector<double> list;
            if (!f || !numbersAt(a_, *f, time_, false, list, error)) continue;
            std::vector<uint32_t>& g = b_.groups[groupName(child->name)];
            for (const double d : list) {
                const size_t i = d >= 0.0 ? static_cast<size_t>(d) : made.size();
                if (i < made.size() && made[i] >= 0) g.push_back(static_cast<uint32_t>(made[i]));
            }
        }
    }

    void points(const ObjectReader& o, const PropertyReader& geom) {
        std::vector<Vec3> P;
        if (!positions(o, geom, P)) return;
        const size_t start = addPoints(P);
        const std::vector<uint32_t> none;
        std::string error;
        Param p;
        if (const PropertyReader* ids = geom.find(".pointIds"); ids && paramAt(a_, *ids, time_, false, p, error)) {
            p.integral = true;
            p.scope = "var";
            param("id", p, start, P.size(), 0, none, 0, none, false, false);
        }
        if (const PropertyReader* v = geom.find(".velocities"); v && paramAt(a_, *v, time_, true, p, error)) {
            p.scope = "var";
            param("v", p, start, P.size(), 0, none, 0, none, true, false);
        }
        if (const PropertyReader* w = geom.find(".widths"); w && paramAt(a_, *w, time_, true, p, error)) {
            for (double& x : p.values) x *= 0.5;
            if (p.values.size() == 1) p.values.assign(P.size(), p.values[0]);
            p.scope = "var";
            param("pscale", p, start, P.size(), 0, none, 0, none, false, false);
        }
        fileFaces_ = 0;
        fileCorners_ = 0;
        arbitrary(geom, start, P.size(), 0, none, 0, none);
    }

    void curves(const ObjectReader& o, const PropertyReader& geom) {
        std::vector<Vec3> P;
        if (!positions(o, geom, P)) return;
        std::vector<double> counts;
        std::string error;
        const PropertyReader* n = geom.find("nVertices");
        if (!n || !numbersAt(a_, *n, time_, false, counts, error)) {
            note(o, "curves without their counts (nVertices)");
            return;
        }
        const size_t start = addPoints(P);
        const size_t primStart = b_.geo.primitiveCount(), vertexStart = b_.geo.vertexCount();
        std::vector<uint32_t> all, sizes, faces, corners;
        size_t at = 0;
        for (size_t c = 0; c < counts.size(); ++c) {
            const size_t k = counts[c] > 0.0 ? static_cast<size_t>(counts[c]) : 0;
            if (at + k > P.size()) {
                note(o, "fewer points than the curves need");
                break;
            }
            if (k >= 2) {
                for (size_t i = 0; i < k; ++i) {
                    all.push_back(static_cast<uint32_t>(start + at + i));
                    corners.push_back(static_cast<uint32_t>(at + i));
                }
                sizes.push_back(static_cast<uint32_t>(k));
                faces.push_back(static_cast<uint32_t>(c));
            }
            at += k;
        }
        const uint8_t open = 0;
        if (!sizes.empty()) b_.geo.addPrimitives(all, sizes, std::span<const uint8_t>(&open, 1));
        b_.paths.resize(b_.geo.primitiveCount(), o.path);
        fileFaces_ = counts.size();
        fileCorners_ = P.size();
        Param p;
        if (const PropertyReader* w = geom.find("width"); w && paramAt(a_, *w, time_, true, p, error)) {
            for (double& x : p.values) x *= 0.5;
            param("pscale", p, start, P.size(), vertexStart, corners, primStart, faces, false, false);
        }
        if (const PropertyReader* v = geom.find(".velocities"); v && paramAt(a_, *v, time_, true, p, error)) {
            p.scope = "vtx";
            param("v", p, start, P.size(), vertexStart, corners, primStart, faces, true, false);
        }
        arbitrary(geom, start, P.size(), vertexStart, corners, primStart, faces);
    }

    const ArchiveReader& a_;
    double time_;
    const ImportOptions& options_;
    std::vector<std::string>* skipped_;
    Builder b_;
    Matrix world_ = identity();
    size_t fileFaces_ = 0, fileCorners_ = 0;
};

}  // namespace

Matrix worldTransform(const ArchiveReader& a, const ObjectReader& object, double time) {
    Matrix m = identity();
    for (const ObjectReader* o = &object; o; o = o->parent) {
        bool inherits = true;
        m = multiply(m, localTransform(a, *o, time, inherits));
        if (!inherits) break;
    }
    return m;
}

bool visible(const ArchiveReader& a, const ObjectReader& object, double time) {
    std::string error;
    for (const ObjectReader* o = &object; o; o = o->parent) {
        const PropertyReader* p = o->properties.find("visible");
        std::vector<double> v;
        if (p && !p->is(PropertyType::Compound) && numbersAt(a, *p, time, false, v, error) && !v.empty() && v[0] == 0.0) {
            return false;
        }
    }
    return true;
}

std::vector<const ObjectReader*> geometryObjects(const ArchiveReader& a, const ImportOptions& options) {
    std::vector<const ObjectReader*> out;
    for (const ObjectReader* o : a.objects()) {
        if (!isGeometry(schemaOf(*o))) continue;
        bool inside = options.roots.empty();
        for (const std::string& r : options.roots) inside = inside || under(o->path, r);
        if (inside) out.push_back(o);
    }
    return out;
}

std::shared_ptr<Geometry> importGeometry(const ArchiveReader& a, double time, const ImportOptions& options,
                                         std::vector<std::string>* skipped) {
    Importer importer(a, time, options, skipped);
    for (const ObjectReader* o : geometryObjects(a, options)) importer.object(*o);
    if (skipped) {
        for (const ObjectReader* o : a.objects()) {
            const std::string s = schemaOf(*o);
            if (s.rfind("AbcGeom_NuPatch", 0) == 0) skipped->push_back(o->path + ": a NURBS patch, not read");
        }
    }
    return importer.finish();
}

bool geometryVaries(const ArchiveReader& a, const ImportOptions& options) {
    for (const ObjectReader* o : geometryObjects(a, options)) {
        if (const PropertyReader* g = o->properties.find(".geom"); g && varies(*g)) return true;
        if (placeVaries(*o)) return true;
    }
    return false;
}

std::vector<const ObjectReader*> cameras(const ArchiveReader& a) {
    std::vector<const ObjectReader*> out;
    for (const ObjectReader* o : a.objects()) {
        if (schemaOf(*o) == kCamera) out.push_back(o);
    }
    return out;
}

bool cameraAt(const ArchiveReader& a, const ObjectReader& camera, double time, Matrix& world, Lens& lens, std::string& error) {
    world = worldTransform(a, camera, time);
    const PropertyReader* geom = camera.properties.find(".geom");
    const PropertyReader* core = geom ? geom->find(".core") : nullptr;
    std::vector<double> c;
    if (!core || !numbersAt(a, *core, time, true, c, error) || c.size() < 16) {
        if (error.empty()) error = camera.path + ": a camera without its lens (.core)";
        return false;
    }
    lens = Lens::of(c.data());
    return true;
}

bool cameraVaries(const ArchiveReader&, const ObjectReader& camera) {
    const PropertyReader* geom = camera.properties.find(".geom");
    return (geom && varies(*geom)) || placeVaries(camera);
}

bool timeRange(const ArchiveReader& a, double& first, double& last) {
    first = 1e300;
    last = -1e300;
    bool any = false;
    std::vector<const PropertyReader*> stack;
    for (const ObjectReader* o : a.objects()) stack.push_back(&o->properties);
    while (!stack.empty()) {
        const PropertyReader* p = stack.back();
        stack.pop_back();
        for (const PropertyReader& c : p->children) stack.push_back(&c);
        if (p->is(PropertyType::Compound) || p->header.samples < 2 || p->header.constant()) continue;
        first = std::min(first, a.timeOf(*p, 0));
        last = std::max(last, a.timeOf(*p, p->header.samples - 1));
        any = true;
    }
    return any;
}

}  // namespace pg::abc
