#include "pg/usd/Geom.h"

#include "pg/core/Instances.h"
#include "pg/core/Material.h"
#include "pg/io/Vdb.h"
#include "pg/usd/Shade.h"

#include <glm/ext/quaternion_double.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

namespace pg::usd {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool xformable(const std::string& type) {
    static const char* const kNot[] = {"",          "Scope",          "Material",      "Shader",     "NodeGraph",
                                       "GeomSubset", "SkelAnimation",  "BlendShape",    "RenderSettings",
                                       "RenderProduct", "RenderVar",   "RenderPass",    "Backdrop"};
    for (const char* t : kNot) {
        if (type == t) return false;
    }
    return true;
}

/// A number of the file as an index below `n`; `n` for one that is not
/// (negative, too large, NaN).
size_t indexBelow(double x, size_t n) { return x >= 0.0 && x < static_cast<double>(n) ? static_cast<size_t>(x) : n; }

/// A count of the file, at most `most`; `most + 1` for one above it (or NaN).
size_t countUpTo(double x, size_t most) {
    if (x <= 0.0) return 0;
    return x <= static_cast<double>(most) ? static_cast<size_t>(x) : most + 1;
}

bool under(const std::string& path, const std::string& root) {
    if (root.empty() || root == "/") return true;
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

std::string opType(const std::string& name) {
    // xformOp:rotateXYZ:suffix -> rotateXYZ
    const size_t a = name.find(':');
    if (a == std::string::npos) return {};
    const size_t b = name.find(':', a + 1);
    return name.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
}

Matrix opMatrix(const Stage& stage, const Stage::Prim& prim, const std::string& name, double time) {
    const Value v = stage.value(prim, name, time);
    const std::string type = opType(name);
    const std::vector<double>& n = v.numbers;
    if (!v.isNumbers()) return {};
    if (type == "translate" && n.size() >= 3) return Matrix::translate(n[0], n[1], n[2]);
    if (type == "scale" && n.size() >= 3) return Matrix::scale(n[0], n[1], n[2]);
    if (type == "rotateX" && !n.empty()) return Matrix::rotate(0, n[0]);
    if (type == "rotateY" && !n.empty()) return Matrix::rotate(1, n[0]);
    if (type == "rotateZ" && !n.empty()) return Matrix::rotate(2, n[0]);
    if (type.size() == 9 && type.rfind("rotate", 0) == 0 && n.size() >= 3) {
        // rotateXYZ: about x first, then y, then z -- Rx Ry Rz for rows.
        Matrix m;
        for (int k = 0; k < 3; ++k) {
            const int axis = type[6 + static_cast<size_t>(k)] - 'X';
            if (axis < 0 || axis > 2) return {};
            m = m * Matrix::rotate(axis, n[static_cast<size_t>(axis)]);
        }
        return m;
    }
    if (type == "orient" && n.size() >= 4) return Matrix::orient(n[0], n[1], n[2], n[3]);
    if (type == "transform" && n.size() >= 16) {
        Matrix m;
        std::copy(n.begin(), n.begin() + 16, glm::value_ptr(m.m));
        return m;
    }
    return {};
}

// --- Building geometry ------------------------------------------------------------------------

/// Everything read, gathered into one geometry: points and primitives as
/// they come, attributes by name -- zeros where a prim has none -- and the
/// prims' paths once each.
struct Builder {
    Geometry geo;
    struct Attr {
        AttrClass cls;
        int width;        ///< 1..4 floats; 0: an int
        std::vector<float> floats;
        std::vector<int32_t> ints;
        size_t count = 0;  ///< elements filled
        std::vector<std::pair<size_t, size_t>> given;  ///< the elements prims gave values: [from, to)
    };
    std::map<std::pair<AttrClass, std::string>, Attr> attrs;
    std::vector<int32_t> paths;  ///< per primitive
    std::vector<std::string> pathTable;
    std::vector<int32_t> materials;  ///< per primitive: its material in `materialTable`; -1 none
    std::vector<std::string> materialTable;

    /// The index of material `path` in the table; -1 for none.
    int32_t material(const std::string& path) {
        if (path.empty()) return -1;
        const auto it = std::find(materialTable.begin(), materialTable.end(), path);
        if (it != materialTable.end()) return static_cast<int32_t>(it - materialTable.begin());
        materialTable.push_back(path);
        return static_cast<int32_t>(materialTable.size() - 1);
    }
    std::map<std::string, std::vector<uint32_t>> groups;
    std::vector<std::string>* notes = nullptr;
    /// The geometry's prototype each prototype prim read is; and the copies
    /// of one stretched by a transform, by it.
    std::map<std::string, int32_t> prototypeOf;
    std::map<std::pair<int32_t, std::array<double, 9>>, int32_t> stretched;

    size_t elements(AttrClass c) const {
        switch (c) {
            case AttrClass::Point: return geo.pointCount();
            case AttrClass::Vertex: return geo.vertexCount();
            case AttrClass::Primitive: return geo.primitiveCount();
            default: return 1;
        }
    }

    /// The attribute, its elements up to `start` filled (zeros where earlier
    /// prims had none); null when its name is taken by another width.
    Attr* attr(AttrClass cls, const std::string& name, int width, size_t start) {
        auto [it, fresh] = attrs.try_emplace({cls, name}, Attr{cls, width, {}, {}, 0, {}});
        Attr& a = it->second;
        if (!fresh && a.width != width) {
            if (notes) notes->push_back("'" + name + "' is " + std::to_string(width) + " numbers here but " +
                                        std::to_string(a.width) + " elsewhere: left out here");
            return nullptr;
        }
        pad(a, start);
        return &a;
    }
    static void pad(Attr& a, size_t count) {
        if (a.count >= count) return;
        if (a.width == 0) a.ints.resize(count, 0);
        else a.floats.resize(count * static_cast<size_t>(a.width), 0.0f);
        a.count = count;
    }
    /// A prim gave elements [from, to) of `a` values: filled up to there,
    /// and remembered as given.
    static void give(Attr& a, size_t from, size_t to) {
        pad(a, to);
        if (to > from) a.given.emplace_back(from, to);
    }

    /// A name on more than one class -- one prim's uv on its points,
    /// another's on its corners, a third's on its faces -- goes onto the
    /// corners, as a merge in Houdini promotes it: what reads the name takes
    /// the corners' first, and there the prims that gave theirs elsewhere
    /// would have zeros. Each corner takes the value its prim gave it, else
    /// its point's, else its face's; points no primitive uses keep theirs.
    /// Velocities stay where each prim gave them: the renderers take them
    /// from the points alone.
    void unify() {
        std::map<std::string, int> classes;
        for (const auto& [key, a] : attrs) {
            if (key.first != AttrClass::Detail) ++classes[key.second];
        }
        const auto corners = geo.vertexPoints();
        const auto starts = geo.primitiveStarts();
        const auto sizes = geo.primitiveSizes();
        std::vector<uint8_t> used(geo.pointCount(), 0);
        for (const uint32_t p : corners) used[p] = 1;
        for (const auto& [name, count] : classes) {
            if (count < 2 || name == "v") continue;
            Attr* point = find(AttrClass::Point, name);
            Attr* vertex = find(AttrClass::Vertex, name);
            Attr* face = find(AttrClass::Primitive, name);
            const int width = (vertex ? vertex : point)->width;
            if ((point && point->width != width) || (face && face->width != width)) {
                if (notes) notes->push_back("'" + name + "' is of different widths on points, corners and faces: " +
                                            "left as it is");
                continue;
            }
            const auto onPoint = givenOf(point, geo.pointCount());
            const auto onFace = givenOf(face, geo.primitiveCount());
            if (!vertex) {
                const Attr fresh{AttrClass::Vertex, width, {}, {}, 0, {}};
                vertex = &attrs.try_emplace({AttrClass::Vertex, name}, fresh).first->second;
            }
            pad(*vertex, geo.vertexCount());
            const auto onCorner = givenOf(vertex, geo.vertexCount());
            const size_t w = static_cast<size_t>(width);
            auto copy = [&](const Attr& from, size_t element, size_t corner) {
                if (width == 0) vertex->ints[corner] = from.ints[element];
                else std::copy_n(&from.floats[element * w], w, &vertex->floats[corner * w]);
            };
            for (size_t prim = 0; prim < starts.size(); ++prim) {
                for (size_t c = starts[prim]; c < starts[prim] + sizes[prim]; ++c) {
                    if (onCorner[c]) continue;
                    if (point && onPoint[corners[c]]) copy(*point, corners[c], c);
                    else if (face && onFace[prim]) copy(*face, prim, c);
                }
            }
            if (face) attrs.erase({AttrClass::Primitive, name});
            bool loose = false;
            for (size_t p = 0; p < onPoint.size() && !loose; ++p) loose = onPoint[p] && !used[p];
            if (point && !loose) attrs.erase({AttrClass::Point, name});
        }
    }

    Attr* find(AttrClass cls, const std::string& name) {
        const auto it = attrs.find({cls, name});
        return it == attrs.end() ? nullptr : &it->second;
    }
    /// 1 for each of the first `n` elements of `a` a prim gave a value.
    static std::vector<uint8_t> givenOf(const Attr* a, size_t n) {
        std::vector<uint8_t> m(n, 0);
        if (!a) return m;
        for (const auto& [from, to] : a->given) {
            for (size_t i = from; i < std::min(to, n); ++i) m[i] = 1;
        }
        return m;
    }

    /// The points that stand for no prototype: instance -1, not the first
    /// prototype's 0; the instances given no tint drawn as they are.
    void instancesAsThemselves() {
        Attr* k = find(AttrClass::Point, "instance");
        if (!k || k->width != 0) return;
        const std::vector<uint8_t> stands = givenOf(k, k->count);
        for (size_t p = 0; p < stands.size(); ++p) {
            if (!stands[p]) k->ints[p] = -1;
        }
        Attr* tint = find(AttrClass::Point, "tint");
        if (!tint || tint->width != 3) return;
        const std::vector<uint8_t> tinted = givenOf(tint, tint->count);
        for (size_t p = 0; p < stands.size() && p < tinted.size(); ++p) {
            if (stands[p] && !tinted[p]) std::fill_n(&tint->floats[3 * p], 3, 1.0f);
        }
    }

    std::shared_ptr<Geometry> finish() {
        for (auto& [key, a] : attrs) pad(a, geo.elementCount(key.first));
        instancesAsThemselves();
        unify();
        auto out = std::make_shared<Geometry>(std::move(geo));
        for (auto& [key, a] : attrs) {
            const size_t n = out->elementCount(key.first);
            const AttrType type = a.width == 0 ? AttrType::Int
                                  : a.width == 1 ? AttrType::Float
                                  : a.width == 2 ? AttrType::Vec2
                                  : a.width == 3 ? AttrType::Vec3
                                                 : AttrType::Vec4;
            AttributeArray& dst = out->attributes(key.first).create(key.second, type);
            if (a.width == 0) {
                std::copy_n(a.ints.begin(), n, dst.write<int32_t>().begin());
            } else if (n > 0) {
                std::memcpy(dst.rawWrite(), a.floats.data(), n * static_cast<size_t>(a.width) * sizeof(float));
            }
        }
        if (!pathTable.empty()) {
            AttributeArray& p = out->primitives().create("path", AttrType::String);
            for (const std::string& s : pathTable) p.addString(s);
            paths.resize(out->primitiveCount(), 0);
            std::copy(paths.begin(), paths.end(), p.write<int32_t>().begin());
        }
        for (const auto& [name, members] : groups) {
            Group& g = out->createGroup(name, AttrClass::Primitive);
            g.resize(out->primitiveCount());
            for (const uint32_t m : members) g.set(m, true);
        }
        return out;
    }
};

/// A group's name from a prim's: letters, digits and _.
std::string groupName(const std::string& name) {
    std::string out;
    for (const char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    return out;
}

/// What one prim adds: where its points, corners and primitives start, and
/// how its corners and faces map to the file's (for face-varying and
/// uniform values).
struct Piece {
    size_t points = 0, vertices = 0, prims = 0;
    size_t sourcePoints = 0, sourceFaces = 0, sourceCorners = 0;
    std::vector<uint32_t> faceOf;    ///< per primitive added: the file's face
    std::vector<uint32_t> cornerOf;  ///< per corner added: the file's corner
    std::vector<uint32_t> pointOf;   ///< per point added: the file's point; empty: all, in their order
};

std::shared_ptr<Geometry> prototypeGeometry(const Stage& stage, const Stage::Prim& root, double time,
                                            const ImportOptions& options, std::vector<std::string>* notes, int depth);

/// The transform of `prim` within the prototype `root` it is in: its own
/// and those above it up to the root's, the root's own included -- what
/// is above the root left out, as a PointInstancer places a prototype.
Matrix withinPrototype(const Stage& stage, const Stage::Prim& prim, const Stage::Prim& root, double time) {
    Matrix m;
    for (const Stage::Prim* p = &prim; p; p = p->parent) {
        bool resets = false;
        m = m * localTransform(stage, *p, time, &resets);
        if (resets || p == &root) break;
    }
    return m;
}

/// The instances of a PointInstancer at `time`, as UsdGeomPointInstancer
/// computes them (ComputeInstanceTransformsAtTime at `time` from `time`):
/// each one's transform in the instancer's space -- its scale, then its
/// orientation, then its position -- and its prototype; those of
/// invisibleIds and inactiveIds left out. Where velocities have a sample
/// where positions have their last at or before `time` (else their first),
/// positions, orientations and scales are that sample's, the positions
/// moved on by the velocities (and accelerations), the orientations turned
/// on by the angularVelocities sampled there too; else each is as it is at
/// `time`. Orientations are taken unit length: USD turns by a half's
/// quaternion as it is, up to 6e-4 off a turn.
struct Instances {
    std::vector<Matrix> xforms;
    std::vector<int32_t> prototypes;  ///< of the instancer's, in its order
    std::vector<uint32_t> sources;    ///< which instance of the file each is
    std::vector<int32_t> ids;         ///< each one's id, where the file has ids
    size_t count = 0;                 ///< the file's instances
};
Instances instancesAt(const Stage& stage, const Stage::Prim& prim, double time, std::string& why) {
    Instances out;
    double at = time, seconds = 0.0;
    bool moving = false;
    const std::vector<double> times = stage.sampleTimes(prim, "positions");
    auto sampledAt = [&](const char* name, double t) {
        const std::vector<double> ts = stage.sampleTimes(prim, name);
        return std::find(ts.begin(), ts.end(), t) != ts.end();
    };
    if (!times.empty()) {
        double lower = times.front();
        for (const double t : times) {
            if (t <= time) lower = t;
        }
        if (sampledAt("velocities", lower)) {
            at = lower;
            moving = true;
            const double rate = stage.timeCodesPerSecond();
            seconds = rate > 0.0 ? (time - lower) / rate : 0.0;
        }
    }
    const Value indices = stage.value(prim, "protoIndices", at);
    const Value positions = stage.value(prim, "positions", at);
    const size_t n = indices.isNumbers() ? indices.numbers.size() : 0;
    out.count = n;
    if (n == 0) return out;
    if (!positions.isNumbers() || positions.width != 3 || positions.numbers.size() != 3 * n) {
        why = std::to_string(n) + " protoIndices but " + std::to_string(positions.size()) + " positions: no instances";
        out.count = 0;
        return out;
    }
    // The other arrays, where they are one an instance.
    auto each = [&](const char* name, int width, double t) {
        Value v = stage.value(prim, name, t);
        if (!v.isNumbers() || v.width != width || v.numbers.size() != static_cast<size_t>(width) * n) return Value();
        return v;
    };
    // orientationsf -- floats -- before the halfs of orientations.
    Value orientations = each("orientationsf", 4, at);
    if (!orientations.isNumbers()) orientations = each("orientations", 4, at);
    const Value scales = each("scales", 3, at);
    const Value ids = each("ids", 1, at);
    Value velocities, accelerations, spins;
    if (moving) {
        velocities = each("velocities", 3, at);
        if (sampledAt("accelerations", at)) accelerations = each("accelerations", 3, at);
        if (sampledAt("angularVelocities", at)) spins = each("angularVelocities", 3, at);
    }
    // Hidden by their ids at the time, or left out for good.
    std::vector<double> hidden = stage.value(prim, "invisibleIds", time).numbers;
    // inactiveIds: a list edit each opinion makes, the weakest first.
    std::vector<ListItem> inactive;
    for (auto it = prim.opinions.rbegin(); it != prim.opinions.rend(); ++it) {
        const Value* v = it->spec->meta("inactiveIds");
        if (v && v->list) {
            v->list->apply(inactive);
        } else if (v && v->isNumbers()) {
            inactive.clear();
            for (const double x : v->numbers) {
                ListItem item;
                item.text = std::to_string(static_cast<long long>(x));
                inactive.push_back(std::move(item));
            }
        }
    }
    for (const ListItem& item : inactive) {
        char* end = nullptr;
        const double id = std::strtod(item.text.c_str(), &end);
        if (end != item.text.c_str()) hidden.push_back(id);
    }
    std::sort(hidden.begin(), hidden.end());
    for (size_t i = 0; i < n; ++i) {
        const double id = ids.isNumbers() ? ids.numbers[i] : static_cast<double>(i);
        if (std::binary_search(hidden.begin(), hidden.end(), id)) continue;
        double p[3] = {positions.numbers[3 * i], positions.numbers[3 * i + 1], positions.numbers[3 * i + 2]};
        if (velocities.isNumbers()) {
            for (int k = 0; k < 3; ++k) {
                const double a = accelerations.isNumbers() ? accelerations.numbers[3 * i + k] : 0.0;
                p[k] += velocities.numbers[3 * i + k] * seconds + 0.5 * a * seconds * seconds;
            }
        }
        Matrix m;
        if (scales.isNumbers()) m = Matrix::scale(scales.numbers[3 * i], scales.numbers[3 * i + 1], scales.numbers[3 * i + 2]);
        if (orientations.isNumbers()) {
            const double* q = &orientations.numbers[4 * i];
            const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (len > 0.0) m = m * Matrix::orient(q[0] / len, q[1] / len, q[2] / len, q[3] / len);
        }
        if (spins.isNumbers()) {
            // Turned on about the axis of its angular velocity, degrees a
            // second, after its orientation.
            const double* w = &spins.numbers[3 * i];
            const double speed = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (speed > 0.0) {
                const double half = 0.5 * speed * seconds * kPi / 180.0, k = std::sin(half) / speed;
                m = m * Matrix::orient(w[0] * k, w[1] * k, w[2] * k, std::cos(half));
            }
        }
        out.xforms.push_back(m * Matrix::translate(p[0], p[1], p[2]));
        out.prototypes.push_back(static_cast<int32_t>(indices.numbers[i]));
        out.sources.push_back(static_cast<uint32_t>(i));
        if (ids.isNumbers()) out.ids.push_back(static_cast<int32_t>(id));
    }
    return out;
}

/// A file `prim`'s asset attribute `name` names at `time`, as USD resolves
/// it: from the layer of the strongest opinion that gives it; "" for none.
std::string assetAt(const Stage& stage, const Stage::Prim& prim, const std::string& name, double time) {
    const Value v = stage.value(prim, name, time);
    if (!v.isStrings() || v.text().empty()) return {};
    for (const Stage::Opinion& o : prim.opinions) {
        const Property* p = o.spec->property(name);
        if (p && (p->hasDefault || p->hasSamples)) return resolveAsset(v.text(), o.layer->identifier);
    }
    return resolveAsset(v.text(), stage.rootLayer().identifier);
}

/// A transform as an instance's: a turn (orient, x y z w) and a size
/// (pscale) -- false for one that stretches, shears or mirrors.
bool turnAndSize(const Matrix& m, Vec4& orient, float& size) {
    // GLM's columns are the rows: as a matrix of columns, it is the transpose.
    const glm::dmat3 a(m.m);
    const double det = glm::determinant(a);
    if (!(det > 0.0) || !std::isfinite(det)) return false;
    const double s = std::cbrt(det);
    const glm::dmat3 r = a / s;
    const glm::dmat3 check = glm::transpose(r) * r;
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) {
            if (std::abs(check[c][k] - (c == k ? 1.0 : 0.0)) > 1e-5) return false;
        }
    }
    const glm::dquat q = glm::normalize(glm::quat_cast(r));
    orient = Vec4(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w));
    size = static_cast<float>(s);
    return true;
}

/// `proto` made as the linear part of `m` makes it -- its instances made
/// copies first --: an instance's prototype it stretches, of its own.
std::shared_ptr<Geometry> stretchedCopy(const Geometry& proto, const Matrix& m) {
    auto g = unpackInstances(proto);
    Matrix linear = m;
    linear.at(3, 0) = linear.at(3, 1) = linear.at(3, 2) = 0.0;
    const Matrix normals = linear.inverse();
    auto move = [&](std::span<Vec3> v, bool normal) {
        for (Vec3& x : v) {
            const double in[3] = {x.x, x.y, x.z};
            double o[3];
            if (normal) {
                // The inverse transpose: as a row, times the inverse's columns.
                for (int c = 0; c < 3; ++c) o[c] = normals.at(c, 0) * in[0] + normals.at(c, 1) * in[1] + normals.at(c, 2) * in[2];
                const double len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                if (len > 0.0) o[0] /= len, o[1] /= len, o[2] /= len;
            } else {
                linear.transformDirection(in, o);
            }
            x = Vec3(static_cast<float>(o[0]), static_cast<float>(o[1]), static_cast<float>(o[2]));
        }
    };
    move(g->positionsForWrite(), false);
    for (AttributeSet* set : {&g->points(), &g->vertices()}) {
        if (AttributeArray* n = set->find("N"); n && n->type() == AttrType::Vec3) move(n->write<Vec3>(), true);
        if (AttributeArray* v = set->find("v"); v && v->type() == AttrType::Vec3) move(v->write<Vec3>(), false);
    }
    return g;
}

struct Reader {
    const Stage& stage;
    const ImportOptions& options;
    double time;
    Matrix conversion;
    Builder& out;
    std::vector<std::string>* notes;
    /// The prototype root what is read is under: transforms within it.
    const Stage::Prim* base = nullptr;
    int depth = 0;  ///< instancers within prototypes of instancers

    void note(const Stage::Prim& prim, const std::string& why) {
        if (notes) notes->push_back(prim.path + ": " + why);
    }

    Matrix worldOf(const Stage::Prim& prim) const {
        const Matrix w = base ? withinPrototype(stage, prim, *base, time) : worldTransform(stage, prim, time);
        return options.metresYUp ? w * conversion : w;
    }

    /// The normals' matrix: the inverse transpose of the linear part.
    static Matrix normalMatrix(const Matrix& w) {
        Matrix linear = w;
        linear.at(3, 0) = linear.at(3, 1) = linear.at(3, 2) = 0.0;
        const Matrix inv = linear.inverse();
        Matrix t;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) t.at(r, c) = inv.at(c, r);
        }
        return t;
    }

    void addPoints(const std::vector<double>& p, const Matrix& w) {
        const size_t n = p.size() / 3;
        const size_t first = out.geo.addPoints(n);
        std::span<Vec3> P = out.geo.positionsForWrite();
        for (size_t i = 0; i < n; ++i) {
            double o[3];
            w.transformPoint(&p[i * 3], o);
            P[first + i] = Vec3(static_cast<float>(o[0]), static_cast<float>(o[1]), static_cast<float>(o[2]));
        }
    }

    void pathOf(const Stage::Prim& prim, const Piece& piece) {
        if (!options.pathAttribute) return;
        out.paths.resize(piece.prims, 0);
        const int32_t index = static_cast<int32_t>(out.pathTable.size());
        out.pathTable.push_back(prim.path);
        out.paths.resize(out.geo.primitiveCount(), index);
    }

    /// A primvar -- its values, expanded by its indices -- onto what the prim
    /// added: constant and uniform on its primitives, vertex and varying on
    /// its points, faceVarying on its corners.
    void primvar(const Stage::Prim& prim, const Piece& piece, const std::string& attrName, const std::string& name,
                 const std::string& defaultInterpolation, int kind) {
        // kind: 0 plain, 1 normal, 2 velocity, 3 uv
        Value v = stage.value(prim, attrName, time);
        if (!v.isNumbers() || v.numbers.empty()) return;
        const Property* spec = stage.property(prim, attrName);
        std::string how = defaultInterpolation;
        if (spec) {
            if (const Value* i = spec->meta("interpolation"); i && !i->text().empty()) how = i->text();
            if (const Value* e = spec->meta("elementSize"); e && e->number(1.0) > 1.0) {
                note(prim, attrName + " has elements of several values: left out");
                return;
            }
        }
        const Value indices = stage.value(prim, attrName + ":indices", time);
        const size_t size = v.size();
        const int width = v.width;
        if (width > 4 || (kind == 3 && width < 2)) {
            note(prim, attrName + " is " + std::to_string(width) + " numbers an element: left out");
            return;
        }
        auto element = [&](size_t k) -> size_t {
            if (indices.isNumbers() && !indices.numbers.empty()) {
                if (k >= indices.numbers.size()) return size;
                return indexBelow(indices.numbers[k], size);
            }
            return k;
        };
        const size_t count = indices.isNumbers() && !indices.numbers.empty() ? indices.numbers.size() : size;
        AttrClass cls;
        size_t start, n;
        std::vector<uint32_t> source;  // per element added: the value's element
        const bool pointsOnly = out.geo.primitiveCount() == piece.prims;
        if (pointsOnly && (how == "constant" || how == "uniform")) {
            // Points have no primitives: one value for them all.
            cls = AttrClass::Point;
            start = piece.points;
            n = out.geo.pointCount() - start;
            source.assign(n, 0);
            if (count < 1 || how == "uniform") return;
        } else if (how == "constant") {
            cls = AttrClass::Primitive;
            start = piece.prims;
            n = out.geo.primitiveCount() - start;
            source.assign(n, 0);
            if (count < 1) return;
        } else if (how == "uniform") {
            cls = AttrClass::Primitive;
            start = piece.prims;
            n = out.geo.primitiveCount() - start;
            if (count != piece.sourceFaces) {
                note(prim, attrName + ": " + std::to_string(count) + " values for " + std::to_string(piece.sourceFaces) + " faces");
                return;
            }
            source = piece.faceOf;
        } else if (how == "faceVarying") {
            cls = AttrClass::Vertex;
            start = piece.vertices;
            n = out.geo.vertexCount() - start;
            if (count != piece.sourceCorners) {
                note(prim, attrName + ": " + std::to_string(count) + " values for " + std::to_string(piece.sourceCorners) + " corners");
                return;
            }
            source = piece.cornerOf;
        } else {  // vertex, varying
            cls = AttrClass::Point;
            start = piece.points;
            n = out.geo.pointCount() - start;
            if (count != piece.sourcePoints) {
                note(prim, attrName + ": " + std::to_string(count) + " values for " + std::to_string(piece.sourcePoints) + " points");
                return;
            }
            source.resize(n);
            for (size_t i = 0; i < n; ++i) source[i] = piece.pointOf.empty() ? static_cast<uint32_t>(i) : piece.pointOf[i];
        }
        const bool integral = v.type == "int" || v.type == "uint" || v.type == "int64" || v.type == "uint64" ||
                              v.type == "uchar" || v.type == "bool";
        const int outWidth = kind == 3 ? 3 : (integral && width == 1 ? 0 : width);
        Builder::Attr* a = out.attr(cls, name, outWidth, start);
        if (!a) return;
        const Matrix w = kind == 1 ? normalMatrix(worldOf(prim)) : worldOf(prim);
        Builder::give(*a, start, start + n);
        for (size_t i = 0; i < n; ++i) {
            const size_t e = element(source[i]);
            if (e >= size) continue;
            const double* x = &v.numbers[e * static_cast<size_t>(width)];
            if (outWidth == 0) {
                a->ints[start + i] = static_cast<int32_t>(x[0]);
                continue;
            }
            float* dst = &a->floats[(start + i) * static_cast<size_t>(outWidth)];
            if (kind == 3) {
                dst[0] = static_cast<float>(x[0]);
                dst[1] = static_cast<float>(x[1]);
                dst[2] = width > 2 ? static_cast<float>(x[2]) : 0.0f;
            } else if ((kind == 1 || kind == 2) && width == 3) {
                double o[3];
                w.transformDirection(x, o);
                if (kind == 1) {
                    const double len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
                    if (len > 0.0) o[0] /= len, o[1] /= len, o[2] /= len;
                }
                for (int k = 0; k < 3; ++k) dst[k] = static_cast<float>(o[k]);
            } else {
                for (int k = 0; k < width; ++k) dst[k] = static_cast<float>(x[k]);
            }
        }
    }

    /// Every primvar of the prim onto what it added.
    void primvars(const Stage::Prim& prim, const Piece& piece, bool mesh) {
        const std::vector<std::string> names = stage.propertyNames(prim);
        const bool primvarNormals = std::find(names.begin(), names.end(), "primvars:normals") != names.end();
        for (const std::string& n : names) {
            if (n.size() > 8 && n.compare(n.size() - 8, 8, ":indices") == 0) continue;
            if (n == "normals" && mesh && !primvarNormals) {
                primvar(prim, piece, n, "N", "vertex", 1);
            } else if (n == "velocities") {
                primvar(prim, piece, n, "v", "vertex", 2);
            } else if (n.rfind("primvars:", 0) == 0) {
                const std::string base = n.substr(9);
                const Property* p = stage.property(prim, n);
                if (!p || p->relationship) continue;
                if (base == "normals") primvar(prim, piece, n, "N", "vertex", 1);
                else if (base == "displayColor") primvar(prim, piece, n, "Cd", "constant", 0);
                else if (base == "displayOpacity") primvar(prim, piece, n, "Alpha", "constant", 0);
                else if (base == "st") primvar(prim, piece, n, "uv", "constant", 3);
                else {
                    std::string name = base;
                    std::replace(name.begin(), name.end(), ':', '_');
                    primvar(prim, piece, n, name, "constant", 0);
                }
            }
        }
    }

    Piece begin() const {
        Piece p;
        p.points = out.geo.pointCount();
        p.vertices = out.geo.vertexCount();
        p.prims = out.geo.primitiveCount();
        return p;
    }

    void mesh(const Stage::Prim& prim) {
        const Value P = stage.value(prim, "points", time);
        const Value counts = stage.value(prim, "faceVertexCounts", time);
        const Value indices = stage.value(prim, "faceVertexIndices", time);
        if (!P.isNumbers() || P.width != 3 || P.numbers.empty()) {
            note(prim, "a mesh without points");
            return;
        }
        const Matrix w = worldOf(prim);
        Piece piece = begin();
        const size_t points = P.numbers.size() / 3;
        piece.sourcePoints = points;
        addPoints(P.numbers, w);
        const bool left = stage.value(prim, "orientation", time).text() == "leftHanded";
        const bool flip = left != (w.determinant3() < 0.0);
        std::vector<char> hole;
        const Value holes = stage.value(prim, "holeIndices", time);
        const size_t faces = counts.numbers.size();
        hole.assign(faces, 0);
        for (const double h : holes.numbers) {
            if (const size_t f = indexBelow(h, faces); f < faces) hole[f] = 1;
        }
        size_t corner = 0, bad = 0;
        std::vector<uint32_t> ring;
        for (size_t f = 0; f < faces; ++f) {
            const size_t c = countUpTo(counts.numbers[f], indices.numbers.size() - corner);
            if (corner + c > indices.numbers.size()) {
                note(prim, "fewer face vertex indices than the faces need");
                break;
            }
            bool ok = c >= 3 && !hole[f];
            ring.clear();
            for (size_t k = 0; k < c && ok; ++k) {
                const size_t i = indexBelow(indices.numbers[corner + (flip ? c - 1 - k : k)], points);
                if (i >= points) ok = false;
                else ring.push_back(static_cast<uint32_t>(piece.points + i));
            }
            if (ok) {
                out.geo.addPrimitive(ring, true);
                piece.faceOf.push_back(static_cast<uint32_t>(f));
                for (size_t k = 0; k < c; ++k) piece.cornerOf.push_back(static_cast<uint32_t>(corner + (flip ? c - 1 - k : k)));
            } else if (!hole[f] && c >= 3) {
                ++bad;
            }
            corner += c;
        }
        if (bad) note(prim, std::to_string(bad) + " faces name points that are not there: left out");
        piece.sourceFaces = faces;
        piece.sourceCorners = corner;
        primvars(prim, piece, true);
        pathOf(prim, piece);
        if (options.subsets) subsets(prim, piece);
        bind(prim, piece);
    }

    /// The material of each primitive the prim added: the prim's, a
    /// GeomSubset's faces theirs.
    void bind(const Stage::Prim& prim, const Piece& piece) {
        if (!options.materials) return;
        const int32_t whole = out.material(boundMaterial(stage, prim));
        const std::vector<BoundFaces> bound =
            prim.type == "Mesh" ? boundSubsets(stage, prim, time) : std::vector<BoundFaces>();
        if (whole < 0 && bound.empty() && out.materialTable.empty()) return;
        out.materials.resize(piece.prims, -1);
        out.materials.resize(out.geo.primitiveCount(), whole);
        if (bound.empty()) return;
        // The file's faces to ours.
        std::vector<int64_t> ours(piece.sourceFaces, -1);
        for (size_t k = 0; k < piece.faceOf.size(); ++k) ours[piece.faceOf[k]] = static_cast<int64_t>(piece.prims + k);
        for (const BoundFaces& b : bound) {
            const int32_t m = out.material(b.material);
            for (const uint32_t f : b.faces) {
                if (f < ours.size() && ours[f] >= 0) out.materials[static_cast<size_t>(ours[f])] = m;
            }
        }
    }

    void subsets(const Stage::Prim& prim, const Piece& piece) {
        for (const Stage::Prim* child : prim.children) {
            if (child->type != "GeomSubset" || !child->defined) continue;
            const std::string element = stage.value(*child, "elementType", time).text();
            if (!element.empty() && element != "face") continue;
            const Value indices = stage.value(*child, "indices", time);
            std::vector<uint32_t>& members = out.groups[groupName(child->name)];
            // The file's faces to ours.
            std::vector<int64_t> ours(piece.sourceFaces, -1);
            for (size_t k = 0; k < piece.faceOf.size(); ++k) ours[piece.faceOf[k]] = static_cast<int64_t>(piece.prims + k);
            for (const double i : indices.numbers) {
                if (const size_t f = indexBelow(i, ours.size()); f < ours.size() && ours[f] >= 0) {
                    members.push_back(static_cast<uint32_t>(ours[f]));
                }
            }
        }
    }

    /// Widths as pscale -- half a width, as large as the transform makes it.
    void widths(const Stage::Prim& prim, const Piece& piece, const Matrix& w) {
        const Value widths = stage.value(prim, "widths", time);
        if (!widths.isNumbers() || widths.numbers.empty()) return;
        const double s = std::cbrt(std::abs(w.determinant3()));
        const size_t n = out.geo.pointCount() - piece.points;
        Builder::Attr* a = out.attr(AttrClass::Point, "pscale", 1, piece.points);
        if (!a) return;
        Builder::give(*a, piece.points, piece.points + n);
        const bool each = widths.numbers.size() == piece.sourcePoints;
        for (size_t i = 0; i < n; ++i) {
            a->floats[piece.points + i] = static_cast<float>(0.5 * s * widths.numbers[each ? i : 0]);
        }
    }

    void pointsPrim(const Stage::Prim& prim) {
        const Value P = stage.value(prim, "points", time);
        if (!P.isNumbers() || P.width != 3) {
            note(prim, "points without positions");
            return;
        }
        const Matrix w = worldOf(prim);
        Piece piece = begin();
        piece.sourcePoints = P.numbers.size() / 3;
        addPoints(P.numbers, w);
        widths(prim, piece, w);
        const Value ids = stage.value(prim, "ids", time);
        if (ids.isNumbers() && ids.numbers.size() == piece.sourcePoints) {
            if (Builder::Attr* a = out.attr(AttrClass::Point, "id", 0, piece.points)) {
                Builder::give(*a, piece.points, piece.points + piece.sourcePoints);
                for (size_t i = 0; i < piece.sourcePoints; ++i) a->ints[piece.points + i] = static_cast<int32_t>(ids.numbers[i]);
            }
        }
        primvars(prim, piece, false);
    }

    /// A PointInstancer: its prototypes the geometry's -- each prim read
    /// once, as prototypeGeometry() reads it --, its instances points that
    /// stand for them (core/Instances.h), each placing its prototype as the
    /// instancer places it (instancesAt): P, orient, pscale; id from ids, v
    /// from velocities, its primvars an instance each. An instance whose
    /// transform stretches, shears or mirrors -- what orient and pscale
    /// cannot say -- stands for a copy of its prototype made so.
    void instancer(const Stage::Prim& prim) {
        std::string why;
        const Instances found = instancesAt(stage, prim, time, why);
        if (!why.empty()) note(prim, why);
        if (found.xforms.empty()) return;
        if (depth >= 8) {
            note(prim, "instancers in the prototypes of instancers 8 deep: left out");
            return;
        }
        const std::vector<std::string> targets = stage.targets(prim, "prototypes");
        std::vector<int32_t> slot(targets.size(), -1);  // the geometry's prototype of each
        for (size_t k = 0; k < targets.size(); ++k) {
            const Stage::Prim* root = stage.find(stripVariants(targets[k]));
            if (!root) {
                note(prim, "no prototype " + targets[k]);
                continue;
            }
            if (const auto it = out.prototypeOf.find(root->path); it != out.prototypeOf.end()) {
                slot[k] = it->second;
                continue;
            }
            slot[k] = static_cast<int32_t>(
                out.geo.addPrototype(prototypeGeometry(stage, *root, time, options, notes, depth + 1)));
            out.prototypeOf[root->path] = slot[k];
        }
        // From the prototype's space -- converted as the stage is -- to the
        // world: back to the stage's units, the instance's transform, the
        // instancer's.
        const Matrix w = worldOf(prim);
        const Matrix back = options.metresYUp ? conversion.inverse() : Matrix();
        Piece piece = begin();
        piece.sourcePoints = found.count;
        std::vector<int32_t> stands, ids;
        std::vector<Vec4> turns;
        std::vector<float> sizes;
        std::vector<double> at;
        size_t stretchedOnes = 0, unknown = 0;
        for (size_t j = 0; j < found.xforms.size(); ++j) {
            const int32_t k = found.prototypes[j];
            if (k < 0 || static_cast<size_t>(k) >= slot.size() || slot[k] < 0) {
                ++unknown;
                continue;
            }
            const Matrix m = back * found.xforms[j] * w;
            Vec4 turn(0.0f, 0.0f, 0.0f, 1.0f);
            float size = 1.0f;
            int32_t proto = slot[k];
            if (!turnAndSize(m, turn, size)) {
                // A copy of its own, made as the transform makes it.
                std::array<double, 9> key{};
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) key[static_cast<size_t>(3 * r + c)] = std::round(m.at(r, c) * 1e6) / 1e6;
                }
                auto [it, fresh] = out.stretched.try_emplace({proto, key}, -1);
                if (fresh) {
                    const auto& prototypes = out.geo.prototypes();
                    it->second = static_cast<int32_t>(out.geo.addPrototype(stretchedCopy(*prototypes[static_cast<size_t>(proto)], m)));
                }
                proto = it->second;
                turn = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                size = 1.0f;
                ++stretchedOnes;
            }
            stands.push_back(proto);
            turns.push_back(turn);
            sizes.push_back(size);
            at.insert(at.end(), {m.at(3, 0), m.at(3, 1), m.at(3, 2)});
            piece.pointOf.push_back(found.sources[j]);
            if (!found.ids.empty()) ids.push_back(found.ids[j]);
        }
        if (unknown) note(prim, std::to_string(unknown) + " instances of no prototype: left out");
        if (stretchedOnes) {
            note(prim, std::to_string(stretchedOnes) + " instances stretch, shear or mirror their prototype: copies of it made so");
        }
        if (stands.empty()) return;
        addPoints(at, Matrix());
        const size_t n = stands.size();
        if (Builder::Attr* a = out.attr(AttrClass::Point, "instance", 0, piece.points)) {
            Builder::give(*a, piece.points, piece.points + n);
            std::copy(stands.begin(), stands.end(), a->ints.begin() + static_cast<std::ptrdiff_t>(piece.points));
        }
        if (Builder::Attr* a = out.attr(AttrClass::Point, "orient", 4, piece.points)) {
            Builder::give(*a, piece.points, piece.points + n);
            for (size_t i = 0; i < n; ++i) {
                float* q = &a->floats[4 * (piece.points + i)];
                q[0] = turns[i].x, q[1] = turns[i].y, q[2] = turns[i].z, q[3] = turns[i].w;
            }
        }
        if (Builder::Attr* a = out.attr(AttrClass::Point, "pscale", 1, piece.points)) {
            Builder::give(*a, piece.points, piece.points + n);
            std::copy(sizes.begin(), sizes.end(), a->floats.begin() + static_cast<std::ptrdiff_t>(piece.points));
        }
        if (ids.size() == n) {
            if (Builder::Attr* a = out.attr(AttrClass::Point, "id", 0, piece.points)) {
                Builder::give(*a, piece.points, piece.points + n);
                std::copy(ids.begin(), ids.end(), a->ints.begin() + static_cast<std::ptrdiff_t>(piece.points));
            }
        }
        primvars(prim, piece, false);
    }

    /// A Volume: each of its fields (field:density, an OpenVDBAsset) the
    /// grid of its file -- filePath at the time, fieldName -- as
    /// io::readVdb() reads it, placed by the field's transform (the
    /// volume's with it, where the field is under it), named as the field
    /// is: density; vel.x, vel.y, vel.z for a vector.
    void volume(const Stage::Prim& prim) {
        for (const std::string& name : stage.propertyNames(prim)) {
            if (name.rfind("field:", 0) != 0) continue;
            const Property* rel = stage.property(prim, name);
            if (!rel || !rel->relationship) continue;
            const std::vector<std::string> targets = stage.targets(prim, name);
            if (targets.empty()) continue;
            const std::string as = name.substr(6);
            const Stage::Prim* field = stage.find(stripVariants(targets.front()));
            if (!field) {
                note(prim, name + ": no field " + targets.front());
                continue;
            }
            if (field->type != "OpenVDBAsset") {
                note(prim, name + ": a " + field->type + " is not read");
                continue;
            }
            const std::string file = assetAt(stage, *field, "filePath", time);
            if (file.empty()) {
                note(prim, name + ": no file");
                continue;
            }
            std::string grid = stage.value(*field, "fieldName", time).text();
            if (grid.empty()) grid = field->name;
            bool under = false;
            for (const Stage::Prim* a = field->parent; a && !under; a = a->parent) under = a == &prim;
            const Matrix w = under ? worldOf(*field) : localTransform(stage, *field, time) * worldOf(prim);
            io::VdbReadOptions o;
            o.grids = {grid};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) o.place[r][c] = w.at(r, c);
            }
            io::VdbVolumes read;
            std::string error;
            bool ok = false;
            if (!file.empty() && file.back() == ']' && file.find(".usdz[") != std::string::npos) {
                // In a package: its bytes.
                std::vector<uint8_t> bytes;
                ok = readFileBytes(file, bytes, error) && io::parseVdb(bytes, read, error, o);
            } else {
                ok = io::readVdb(file, read, error, o);
            }
            if (!ok) {
                note(prim, name + ": " + error);
                continue;
            }
            for (const std::string& n : read.notes) note(prim, name + ": " + n);
            if (read.volumes.empty()) {
                note(prim, name + ": no grid \"" + grid + "\" in " + file);
                continue;
            }
            // The first grid of the name: one volume, three for a vector.
            const size_t parts = std::min<size_t>(read.volumes.size(), read.components.front() == 3 ? 3 : 1);
            for (size_t k = 0; k < parts; ++k) {
                Volume v = std::move(read.volumes[k]);
                v.name = parts == 3 ? as + (k == 0 ? ".x" : k == 1 ? ".y" : ".z") : as;
                out.geo.addVolume(std::move(v));
            }
        }
    }

    void curves(const Stage::Prim& prim) {
        const Value P = stage.value(prim, "points", time);
        const Value counts = stage.value(prim, "curveVertexCounts", time);
        if (!P.isNumbers() || P.width != 3) {
            note(prim, "curves without points");
            return;
        }
        const Matrix w = worldOf(prim);
        Piece piece = begin();
        const size_t points = P.numbers.size() / 3;
        piece.sourcePoints = points;
        addPoints(P.numbers, w);
        const bool periodic = stage.value(prim, "wrap", time).text() == "periodic";
        size_t at = 0;
        std::vector<uint32_t> ring;
        for (size_t c = 0; c < counts.numbers.size(); ++c) {
            const size_t n = countUpTo(counts.numbers[c], points - at);
            if (at + n > points) break;
            ring.clear();
            for (size_t k = 0; k < n; ++k) ring.push_back(static_cast<uint32_t>(piece.points + at + k));
            if (n >= 2) {
                out.geo.addPrimitive(ring, periodic && n >= 3);
                piece.faceOf.push_back(static_cast<uint32_t>(c));
                for (size_t k = 0; k < n; ++k) piece.cornerOf.push_back(static_cast<uint32_t>(at + k));
            }
            at += n;
        }
        piece.sourceFaces = counts.numbers.size();
        piece.sourceCorners = at;
        widths(prim, piece, w);
        primvars(prim, piece, false);
        pathOf(prim, piece);
    }

    // --- Implicit shapes ---------------------------------------------------------------

    double number(const Stage::Prim& prim, const char* name, double fallback) {
        const Value v = stage.value(prim, name, time);
        return v.isNumbers() && !v.numbers.empty() ? v.numbers[0] : fallback;
    }

    /// A shape made along +z, turned to its axis (a turn, not a mirror).
    static void toAxis(std::vector<double>& p, const std::string& axis) {
        for (size_t i = 0; i + 2 < p.size(); i += 3) {
            const double x = p[i], y = p[i + 1], z = p[i + 2];
            if (axis == "X") p[i] = z, p[i + 1] = x, p[i + 2] = y;
            else if (axis == "Y") p[i] = y, p[i + 1] = z, p[i + 2] = x;
        }
    }

    /// A surface of revolution about z: rings of (radius, z) from the
    /// bottom up, `segments` round; a ring of radius 0 is one point.
    static void revolve(const std::vector<std::pair<double, double>>& rings, int segments, std::vector<double>& p,
                        std::vector<std::vector<uint32_t>>& faces) {
        std::vector<std::vector<uint32_t>> ids;
        for (const auto& [r, z] : rings) {
            std::vector<uint32_t> ring;
            if (r <= 0.0) {
                ring.assign(static_cast<size_t>(segments), static_cast<uint32_t>(p.size() / 3));
                p.insert(p.end(), {0.0, 0.0, z});
            } else {
                for (int s = 0; s < segments; ++s) {
                    const double a = 2.0 * kPi * s / segments;
                    ring.push_back(static_cast<uint32_t>(p.size() / 3));
                    p.insert(p.end(), {r * std::cos(a), r * std::sin(a), z});
                }
            }
            ids.push_back(std::move(ring));
        }
        for (size_t k = 0; k + 1 < ids.size(); ++k) {
            const auto& lo = ids[k];
            const auto& hi = ids[k + 1];
            for (int s = 0; s < segments; ++s) {
                const size_t a = static_cast<size_t>(s), b = static_cast<size_t>((s + 1) % segments);
                std::vector<uint32_t> f{lo[a], lo[b], hi[b], hi[a]};
                f.erase(std::unique(f.begin(), f.end()), f.end());
                if (f.size() > 1 && f.front() == f.back()) f.pop_back();
                if (f.size() >= 3) faces.push_back(std::move(f));
            }
        }
        // Caps where the rings end with a radius.
        if (!rings.empty() && rings.front().first > 0.0) {
            std::vector<uint32_t> cap(ids.front().rbegin(), ids.front().rend());
            faces.push_back(cap);
        }
        if (!rings.empty() && rings.back().first > 0.0) faces.push_back(ids.back());
    }

    void implicit(const Stage::Prim& prim) {
        std::vector<double> p;
        std::vector<std::vector<uint32_t>> faces;
        const std::string& t = prim.type;
        if (t == "Cube") {
            const double h = number(prim, "size", 2.0) * 0.5;
            for (int i = 0; i < 8; ++i) p.insert(p.end(), {i & 1 ? h : -h, i & 2 ? h : -h, i & 4 ? h : -h});
            faces = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        } else if (t == "Sphere") {
            const double r = number(prim, "radius", 1.0);
            std::vector<std::pair<double, double>> rings;
            for (int k = 0; k <= 16; ++k) {
                const double a = -kPi / 2 + kPi * k / 16;
                rings.emplace_back(k == 0 || k == 16 ? 0.0 : r * std::cos(a), r * std::sin(a));
            }
            revolve(rings, 32, p, faces);
        } else if (t == "Cylinder" || t == "Cone" || t == "Capsule") {
            const bool capsule = t == "Capsule";
            const double r = number(prim, "radius", capsule ? 0.5 : 1.0);
            const double h = number(prim, "height", capsule ? 1.0 : 2.0) * 0.5;
            std::vector<std::pair<double, double>> rings;
            if (t == "Cylinder") {
                rings = {{r, -h}, {r, h}};
            } else if (t == "Cone") {
                rings = {{r, -h}, {0.0, h}};
            } else {
                for (int k = 0; k <= 8; ++k) {
                    const double a = -kPi / 2 + (kPi / 2) * k / 8;
                    rings.emplace_back(k == 0 ? 0.0 : r * std::cos(a), -h + r * std::sin(a));
                }
                for (int k = 0; k <= 8; ++k) {
                    const double a = (kPi / 2) * k / 8;
                    rings.emplace_back(k == 8 ? 0.0 : r * std::cos(a), h + r * std::sin(a));
                }
            }
            revolve(rings, 32, p, faces);
            toAxis(p, stage.value(prim, "axis", time).text().empty() ? "Z" : stage.value(prim, "axis", time).text());
        } else if (t == "Plane") {
            const double w = number(prim, "width", 2.0) * 0.5, l = number(prim, "length", 2.0) * 0.5;
            p = {-w, -l, 0.0, w, -l, 0.0, w, l, 0.0, -w, l, 0.0};
            faces = {{0, 1, 2, 3}};
            toAxis(p, stage.value(prim, "axis", time).text().empty() ? "Z" : stage.value(prim, "axis", time).text());
        }
        const Matrix w = worldOf(prim);
        Piece piece = begin();
        piece.sourcePoints = p.size() / 3;
        addPoints(p, w);
        const bool flip = w.determinant3() < 0.0;
        size_t corners = 0;
        for (size_t f = 0; f < faces.size(); ++f) {
            std::vector<uint32_t> ring;
            for (const uint32_t i : faces[f]) ring.push_back(static_cast<uint32_t>(piece.points + i));
            if (flip) std::reverse(ring.begin(), ring.end());
            out.geo.addPrimitive(ring, true);
            piece.faceOf.push_back(static_cast<uint32_t>(f));
            corners += ring.size();
        }
        piece.sourceFaces = faces.size();
        piece.sourceCorners = corners;
        // Only what does not depend on the shape's own points: constant and
        // uniform (per face) primvars read here.
        primvars(prim, piece, false);
        pathOf(prim, piece);
        bind(prim, piece);
    }
};

/// What a material is to the program: its name -- a preset's where it is
/// one, "bark_2" as "bark" --, its pictures, and its values.
struct Look {
    std::string name, texture;
    MaterialPreset preset = MaterialPreset::None;
    io::mtlx::Surface surface;
    bool glass = false;
    bool colored = false;  ///< its colour a value: the primitives' Cd
};

std::string lookName(const std::string& prim) {
    if (materialPreset(prim) != MaterialPreset::None) return prim;
    const size_t cut = prim.rfind('_');
    if (cut != std::string::npos && cut + 1 < prim.size() &&
        std::all_of(prim.begin() + static_cast<std::ptrdiff_t>(cut + 1), prim.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) &&
        materialPreset(std::string_view(prim).substr(0, cut)) != MaterialPreset::None) {
        return prim.substr(0, cut);
    }
    return prim;
}

/// The file the stage was opened from: its root layer's, out of a package.
std::string stageFile(const Stage& stage) {
    std::string file = stage.rootLayer().identifier;
    const size_t package = file.find(".usdz[");
    if (package != std::string::npos) file.resize(package + 5);
    return file;
}

/// The primitives' materials as the program's attributes (ImportOptions::materials);
/// `metres` what a unit of the stage is in the geometry's: its metres where
/// it is converted, else 1.
void applyMaterials(const Stage& stage, double time, Geometry& geo, std::vector<int32_t> lookOf,
                    const std::vector<std::string>& table, float metres) {
    if (table.empty()) return;
    const size_t prims = geo.primitiveCount();
    lookOf.resize(prims, -1);
    const std::string file = stageFile(stage);
    std::vector<Look> looks(table.size());
    bool textured = false, valued = false, glassy = false, colored = false;
    for (size_t k = 0; k < table.size(); ++k) {
        Look& l = looks[k];
        const Stage::Prim* m = stage.find(table[k]);
        if (!m) continue;
        l.name = lookName(m->name);
        l.preset = materialPreset(l.name);
        l.surface = materialSurface(stage, *m, time);
        const io::mtlx::Surface& s = l.surface;
        if (!s.found) continue;
        if (!s.color.empty()) l.texture = file + "#" + table[k];
        // Light goes through it: a transmission, or a preview surface
        // mostly see-through that no picture cuts out.
        l.glass = s.values.transmission >= 0.5f ||
                  (s.shader == "UsdPreviewSurface" && s.opacity.empty() && s.values.opacity < 0.5f);
        l.colored = s.color.empty() && !s.tinted && s.values.color.x >= 0.0f && !l.glass;
        textured = textured || !l.texture.empty();
        valued = true;
        glassy = glassy || l.glass;
        colored = colored || l.colored;
    }
    auto lookAt = [&](size_t p) -> const Look* { return lookOf[p] >= 0 ? &looks[static_cast<size_t>(lookOf[p])] : nullptr; };
    // Names and pictures: "" first, as the program's string attributes have it.
    auto strings = [&](const char* name, auto&& of) {
        AttributeArray& a = geo.primitives().create(name, AttrType::String);
        if (a.stringTableSize() == 0) a.addString("");
        std::vector<int32_t> index(looks.size());
        for (size_t k = 0; k < looks.size(); ++k) index[k] = a.internString(of(looks[k]));
        auto w = a.write<int32_t>();
        for (size_t p = 0; p < prims; ++p) {
            if (lookOf[p] >= 0) w[p] = index[static_cast<size_t>(lookOf[p])];
        }
    };
    strings("material", [](const Look& l) -> const std::string& { return l.name; });
    // The numbers: the material's where it gives them; elsewhere what a
    // primitive had, else what its preset would give it.
    auto presetOf = [&](size_t p) {
        const Look* l = lookAt(p);
        return l ? l->preset : MaterialPreset::None;
    };
    auto numbers = [&](const char* name, AttrType type, auto&& of, auto&& otherwise) {
        const bool had = geo.primitives().find(name) != nullptr;
        AttributeArray& a = geo.primitives().create(name, type);
        for (size_t p = 0; p < prims; ++p) {
            const Look* l = lookAt(p);
            const bool own = l && l->surface.found;
            if (!own && had) continue;
            const float v = own ? of(*l) : otherwise(p);
            if (type == AttrType::Int) a.write<int32_t>()[p] = static_cast<int32_t>(v);
            else a.write<float>()[p] = v;
        }
    };
    if (textured) {
        strings("texture", [](const Look& l) -> const std::string& { return l.texture; });
        auto noTexture = [](size_t) { return 0.0f; };
        numbers("texture_tint", AttrType::Int, [](const Look& l) { return l.texture.empty() ? 0.0f : l.surface.tinted ? 1.0f : 0.0f; },
                noTexture);
        numbers("texture_projection", AttrType::Int,
                [](const Look& l) { return l.texture.empty() ? 0.0f : l.surface.size > 0.0f ? 2.0f : 1.0f; }, noTexture);
        numbers("texture_size", AttrType::Float,
                [&](const Look& l) { return l.texture.empty() || l.surface.size <= 0.0f ? 0.0f : l.surface.size * metres; },
                noTexture);
    }
    if (valued) {
        numbers("roughness", AttrType::Float, [](const Look& l) { return std::clamp(l.surface.values.roughness, 0.0f, 1.0f); },
                [&](size_t p) { return presetSurface(presetOf(p)).roughness; });
        numbers("metallic", AttrType::Float, [](const Look& l) { return std::clamp(l.surface.values.metalness, 0.0f, 1.0f); },
                [&](size_t p) { return presetSurface(presetOf(p)).metallic; });
    }
    if (glassy) {
        numbers("glass", AttrType::Int, [](const Look& l) { return l.glass ? 1.0f : 0.0f; }, [](size_t) { return 0.0f; });
    }
    if (!colored) return;
    // The colours that are values: on the primitives, or on their corners
    // where Cd is; without one, the others' their preset's, as the
    // renderers would give them.
    AttributeArray* vertex = geo.vertices().find("Cd");
    AttributeArray* point = geo.points().find("Cd");
    AttributeArray* primitive = geo.primitives().find("Cd");
    AttributeArray* detail = geo.detail().find("Cd");
    for (AttributeArray* a : {vertex, point, primitive, detail}) {
        if (a && a->type() != AttrType::Vec3) return;
    }
    if (!vertex && point) {
        // A point's colour to each of its corners: one face may then be a
        // colour of its own. Points no primitive uses keep theirs.
        const std::vector<Vec3> of(point->read<Vec3>().begin(), point->read<Vec3>().end());
        AttributeArray& v = geo.vertices().create("Cd", AttrType::Vec3);
        auto w = v.write<Vec3>();
        std::vector<uint8_t> used(geo.pointCount(), 0);
        for (size_t c = 0; c < geo.vertexCount(); ++c) {
            w[c] = of[geo.vertexPoint(c)];
            used[geo.vertexPoint(c)] = 1;
        }
        if (std::find(used.begin(), used.end(), 0) == used.end()) geo.points().erase("Cd");
        vertex = geo.vertices().find("Cd");
    }
    if (vertex) {
        auto w = vertex->write<Vec3>();
        for (size_t p = 0; p < prims; ++p) {
            const Look* l = lookAt(p);
            if (!l || !l->colored) continue;
            const size_t start = geo.primitiveVertexStart(p), n = geo.primitiveVertexCount(p);
            for (size_t c = start; c < start + n; ++c) w[c] = l->surface.values.color;
        }
        return;
    }
    if (!primitive) {
        const Vec3 all = detail && detail->size() > 0 ? detail->read<Vec3>()[0] : Vec3(-1.0f);
        AttributeArray& made = geo.primitives().create("Cd", AttrType::Vec3);
        auto w = made.write<Vec3>();
        for (size_t p = 0; p < prims; ++p) w[p] = all.x >= 0.0f ? all : presetSurface(presetOf(p)).color;
        if (detail) geo.detail().erase("Cd");
        primitive = geo.primitives().find("Cd");
    }
    auto w = primitive->write<Vec3>();
    for (size_t p = 0; p < prims; ++p) {
        const Look* l = lookAt(p);
        if (l && l->colored) w[p] = l->surface.values.color;
    }
}

const char* const kGeometryTypes[] = {"Mesh", "Points", "BasisCurves", "Cube",  "Sphere",
                                     "Cylinder", "Cone", "Capsule", "Plane", "Volume"};

bool isGeometry(const std::string& type) {
    for (const char* t : kGeometryTypes) {
        if (type == t) return true;
    }
    return false;
}

/// Whether `prim`'s purpose is one `options` reads.
bool purposeRead(const Stage& stage, const Stage::Prim& prim, const ImportOptions& options) {
    const std::string use = purpose(stage, prim);
    return !((use == "render" && !options.render) || (use == "proxy" && !options.proxy) ||
             (use == "guide" && !options.guide));
}

/// What `reader` reads of `prims` -- meshes, points, curves and shapes in
/// their order, then the PointInstancers.
void readPrims(Reader& reader, const std::vector<const Stage::Prim*>& prims) {
    for (const Stage::Prim* p : prims) {
        if (p->type == "PointInstancer") continue;
        if (p->type == "Mesh") reader.mesh(*p);
        else if (p->type == "Points") reader.pointsPrim(*p);
        else if (p->type == "BasisCurves") reader.curves(*p);
        else if (p->type == "Volume") reader.volume(*p);
        else reader.implicit(*p);
    }
    for (const Stage::Prim* p : prims) {
        if (p->type == "PointInstancer") reader.instancer(*p);
    }
}

std::shared_ptr<Geometry> prototypeGeometry(const Stage& stage, const Stage::Prim& root, double time,
                                            const ImportOptions& options, std::vector<std::string>* notes, int depth) {
    // What is under the root, but what is under an instancer of its own;
    // visible as far up as the root, the ancestors of which are not the
    // prototype's (as UsdImaging has them).
    std::vector<const Stage::Prim*> prims;
    std::vector<const Stage::Prim*> todo = {&root};
    while (!todo.empty()) {
        const Stage::Prim* p = todo.back();
        todo.pop_back();
        if (!p->active) continue;
        if (p->defined && (isGeometry(p->type) || p->type == "PointInstancer") && purposeRead(stage, *p, options)) {
            bool shown = true;
            for (const Stage::Prim* a = p; a && shown; a = a->parent) {
                shown = stage.value(*a, "visibility", time).text() != "invisible";
                if (a == &root) break;
            }
            if (shown) prims.push_back(p);
        }
        if (p->type == "PointInstancer") continue;
        for (auto it = p->children.rbegin(); it != p->children.rend(); ++it) todo.push_back(*it);
    }
    Builder builder;
    builder.notes = notes;
    Reader reader{stage, options, time, toMetresYUp(stage), builder, notes, &root, depth};
    readPrims(reader, prims);
    std::vector<int32_t> materials = std::move(builder.materials);
    const std::vector<std::string> materialTable = std::move(builder.materialTable);
    auto geo = builder.finish();
    applyMaterials(stage, time, *geo, std::move(materials), materialTable,
                   options.metresYUp ? static_cast<float>(stage.metersPerUnit()) : 1.0f);
    return geo;
}

}  // namespace

// --- Matrices ----------------------------------------------------------------------------------

Matrix Matrix::inverse() const {
    // Singular: the identity.
    if (std::abs(glm::determinant(m)) < 1e-300) return {};
    return {glm::inverse(m)};
}

double Matrix::determinant3() const { return glm::determinant(glm::dmat3(m)); }

void Matrix::transformPoint(const double in[3], double out[3]) const {
    const glm::dvec4 p = m * glm::dvec4(in[0], in[1], in[2], 1.0);
    const double w = p.w == 0.0 ? 1.0 : p.w;
    for (int c = 0; c < 3; ++c) out[c] = p[c] / w;
}

void Matrix::transformDirection(const double in[3], double out[3]) const {
    const glm::dvec3 d = glm::dmat3(m) * glm::dvec3(in[0], in[1], in[2]);
    for (int c = 0; c < 3; ++c) out[c] = d[c];
}

Matrix Matrix::translate(double x, double y, double z) {
    Matrix r;
    r.m[3] = glm::dvec4(x, y, z, 1.0);
    return r;
}

Matrix Matrix::scale(double x, double y, double z) {
    Matrix r;
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = z;
    return r;
}

Matrix Matrix::rotate(int axis, double degrees) {
    const double a = glm::radians(degrees), c = std::cos(a), s = std::sin(a);
    Matrix r;
    const int i = (axis + 1) % 3, j = (axis + 2) % 3;
    r.at(i, i) = c;
    r.at(i, j) = s;
    r.at(j, i) = -s;
    r.at(j, j) = c;
    return r;
}

Matrix Matrix::orient(double x, double y, double z, double w) {
    // As GfMatrix4d::SetRotate, the quaternion as it is: GLM's mat3_cast.
    return {glm::dmat4(glm::mat3_cast(glm::dquat::wxyz(w, x, y, z)))};
}

// --- Transforms --------------------------------------------------------------------------------

double timeCodeAt(const Stage& stage, double frame, double fps, double offset) {
    const double start = stage.rootLayer().meta("startTimeCode") ? stage.startTimeCode() : 1.0;
    const double rate = fps > 0.0 ? stage.timeCodesPerSecond() / fps : 1.0;
    return start + (frame - 1.0 + offset) * rate;
}

Matrix localTransform(const Stage& stage, const Stage::Prim& prim, double time, bool* resets) {
    if (resets) *resets = false;
    Matrix m;
    if (!xformable(prim.type)) return m;
    const Value order = stage.value(prim, "xformOpOrder", time);
    if (!order.isStrings()) return m;
    // The last op first: p x op[n-1] x ... x op[0].
    for (size_t k = order.strings.size(); k-- > 0;) {
        std::string name = order.strings[k];
        if (name == "!resetXformStack!") {
            if (resets) *resets = true;
            continue;
        }
        const bool invert = name.rfind("!invert!", 0) == 0;
        if (invert) name = name.substr(8);
        const Matrix op = opMatrix(stage, prim, name, time);
        m = m * (invert ? op.inverse() : op);
    }
    return m;
}

Matrix worldTransform(const Stage& stage, const Stage::Prim& prim, double time) {
    bool resets = false;
    Matrix m = localTransform(stage, prim, time, &resets);
    if (!resets && prim.parent && prim.parent->parent) m = m * worldTransform(stage, *prim.parent, time);
    return m;
}

bool transformVaries(const Stage& stage, const Stage::Prim& prim) {
    for (const Stage::Prim* p = &prim; p && p->parent; p = p->parent) {
        if (!xformable(p->type)) continue;
        if (stage.varies(*p, "xformOpOrder")) return true;
        const Value order = stage.value(*p, "xformOpOrder", 0.0);
        bool resets = false;
        for (std::string name : order.strings) {
            if (name == "!resetXformStack!") {
                resets = true;
                continue;
            }
            if (name.rfind("!invert!", 0) == 0) name = name.substr(8);
            if (stage.varies(*p, name)) return true;
        }
        if (resets) break;
    }
    return false;
}

Matrix toMetresYUp(const Stage& stage) {
    const double s = stage.metersPerUnit();
    Matrix m = Matrix::scale(s, s, s);
    if (stage.zUp()) {
        Matrix turn;  // x stays; y to -z; z to y
        turn.at(1, 1) = 0.0;
        turn.at(1, 2) = -1.0;
        turn.at(2, 1) = 1.0;
        turn.at(2, 2) = 0.0;
        m = turn * m;
    }
    return m;
}

bool visible(const Stage& stage, const Stage::Prim& prim, double time) {
    for (const Stage::Prim* p = &prim; p && p->parent; p = p->parent) {
        if (stage.value(*p, "visibility", time).text() == "invisible") return false;
    }
    return true;
}

std::string purpose(const Stage& stage, const Stage::Prim& prim) {
    for (const Stage::Prim* p = &prim; p && p->parent; p = p->parent) {
        const Value v = stage.value(*p, "purpose", 0.0);
        if (!v.text().empty()) return v.text();
    }
    return "default";
}

// --- Geometry ----------------------------------------------------------------------------------

std::vector<const Stage::Prim*> geometryPrims(const Stage& stage, const ImportOptions& options) {
    std::vector<const Stage::Prim*> out;
    // What is under a PointInstancer is its prototypes: drawn where it puts
    // them, not where they are (as UsdImaging has it).
    auto prototype = [](const Stage::Prim& p) {
        for (const Stage::Prim* a = p.parent; a; a = a->parent) {
            if (a->type == "PointInstancer") return true;
        }
        return false;
    };
    for (const auto& owned : stage.prims()) {
        const Stage::Prim& p = *owned;
        if (!p.defined || !isGeometry(p.type) || prototype(p)) continue;
        bool inside = options.roots.empty();
        for (const std::string& r : options.roots) inside = inside || under(p.path, r);
        if (!inside) continue;
        if (purposeRead(stage, p, options)) out.push_back(&p);
    }
    return out;
}

std::vector<const Stage::Prim*> instancerPrims(const Stage& stage, const ImportOptions& options) {
    std::vector<const Stage::Prim*> out;
    for (const auto& owned : stage.prims()) {
        const Stage::Prim& p = *owned;
        if (!p.defined || p.type != "PointInstancer") continue;
        // One in the prototypes of another is read with them.
        bool within = false;
        for (const Stage::Prim* a = p.parent; a && !within; a = a->parent) within = a->type == "PointInstancer";
        if (within) continue;
        bool inside = options.roots.empty();
        for (const std::string& r : options.roots) inside = inside || under(p.path, r);
        if (inside && purposeRead(stage, p, options)) out.push_back(&p);
    }
    return out;
}

std::shared_ptr<Geometry> importGeometry(const Stage& stage, double time, const ImportOptions& options,
                                         std::vector<std::string>* skipped) {
    Builder builder;
    builder.notes = skipped;
    Reader reader{stage, options, time, toMetresYUp(stage), builder, skipped, nullptr, 0};
    std::vector<const Stage::Prim*> prims;
    for (const Stage::Prim* p : geometryPrims(stage, options)) {
        if (visible(stage, *p, time)) prims.push_back(p);
    }
    for (const Stage::Prim* p : instancerPrims(stage, options)) {
        if (visible(stage, *p, time)) prims.push_back(p);
    }
    readPrims(reader, prims);
    if (skipped) {
        for (const auto& owned : stage.prims()) {
            const Stage::Prim& p = *owned;
            if (!p.defined) continue;
            if (p.type == "NurbsPatch" || p.type == "NurbsCurves" || p.type == "HermiteCurves" || p.type == "TetMesh") {
                bool inside = options.roots.empty();
                for (const std::string& r : options.roots) inside = inside || under(p.path, r);
                if (inside) skipped->push_back(p.path + ": a " + p.type + " is not read");
            }
        }
    }
    std::vector<int32_t> materials = std::move(builder.materials);
    const std::vector<std::string> materialTable = std::move(builder.materialTable);
    auto geo = builder.finish();
    applyMaterials(stage, time, *geo, std::move(materials), materialTable,
                   options.metresYUp ? static_cast<float>(stage.metersPerUnit()) : 1.0f);
    return geo;
}

bool geometryVaries(const Stage& stage, const ImportOptions& options) {
    for (const Stage::Prim* p : instancerPrims(stage, options)) {
        // It and what is under it, its prototypes wherever they are.
        std::vector<const Stage::Prim*> todo = {p};
        for (const std::string& t : stage.targets(*p, "prototypes")) {
            if (const Stage::Prim* root = stage.find(stripVariants(t))) todo.push_back(root);
        }
        for (const Stage::Prim* a = p; a && a->parent; a = a->parent) {
            if (stage.varies(*a, "visibility") || transformVaries(stage, *a)) return true;
        }
        size_t seen = 0;
        while (!todo.empty() && ++seen < 100000) {
            const Stage::Prim* q = todo.back();
            todo.pop_back();
            if (!q->clips.empty()) return true;
            for (const std::string& name : stage.propertyNames(*q)) {
                if (stage.varies(*q, name)) return true;
            }
            for (const Stage::Prim* c : q->children) todo.push_back(c);
        }
    }
    for (const Stage::Prim* p : geometryPrims(stage, options)) {
        if (!p->clips.empty() || transformVaries(stage, *p)) return true;
        for (const std::string& name : stage.propertyNames(*p)) {
            if (stage.varies(*p, name)) return true;
        }
        for (const Stage::Prim* a = p; a && a->parent; a = a->parent) {
            if (stage.varies(*a, "visibility")) return true;
        }
        for (const Stage::Prim* c : p->children) {
            if (c->type == "GeomSubset" && stage.varies(*c, "indices")) return true;
        }
        if (p->type == "Volume") {
            // Its fields: a file a frame, wherever they are.
            for (const std::string& name : stage.propertyNames(*p)) {
                if (name.rfind("field:", 0) != 0) continue;
                for (const std::string& t : stage.targets(*p, name)) {
                    const Stage::Prim* f = stage.find(stripVariants(t));
                    if (!f) continue;
                    for (const std::string& n : stage.propertyNames(*f)) {
                        if (stage.varies(*f, n)) return true;
                    }
                    if (transformVaries(stage, *f)) return true;
                }
            }
        }
    }
    return false;
}

// --- Cameras -----------------------------------------------------------------------------------

bool cameraAt(const Stage& stage, const Stage::Prim& prim, double time, bool metresYUp, CameraSample& out) {
    if (prim.type != "Camera") return false;
    out = CameraSample{};
    out.world = worldTransform(stage, prim, time);
    if (metresYUp) out.world = out.world * toMetresYUp(stage);
    auto number = [&](const char* name, double fallback) {
        const Value v = stage.value(prim, name, time);
        return v.isNumbers() && !v.numbers.empty() ? v.numbers[0] : fallback;
    };
    out.focalLength = number("focalLength", out.focalLength);
    out.horizontalAperture = number("horizontalAperture", out.horizontalAperture);
    out.verticalAperture = number("verticalAperture", out.verticalAperture);
    out.horizontalApertureOffset = number("horizontalApertureOffset", 0.0);
    out.verticalApertureOffset = number("verticalApertureOffset", 0.0);
    out.focusDistance = number("focusDistance", 0.0);
    out.fStop = number("fStop", 0.0);
    const Value clip = stage.value(prim, "clippingRange", time);
    if (clip.isNumbers() && clip.numbers.size() >= 2) {
        out.nearClip = clip.numbers[0];
        out.farClip = clip.numbers[1];
    }
    if (metresYUp) {
        out.nearClip *= stage.metersPerUnit();
        out.farClip *= stage.metersPerUnit();
        out.focusDistance *= stage.metersPerUnit();
    }
    out.orthographic = stage.value(prim, "projection", time).text() == "orthographic";
    return true;
}

bool cameraVaries(const Stage& stage, const Stage::Prim& prim) {
    if (transformVaries(stage, prim)) return true;
    for (const char* name : {"focalLength", "horizontalAperture", "verticalAperture", "horizontalApertureOffset",
                             "verticalApertureOffset", "clippingRange", "focusDistance", "fStop"}) {
        if (stage.varies(prim, name)) return true;
    }
    return false;
}

std::vector<const Stage::Prim*> cameras(const Stage& stage) {
    std::vector<const Stage::Prim*> out;
    for (const auto& p : stage.prims()) {
        if (p->defined && p->type == "Camera") out.push_back(p.get());
    }
    return out;
}

}  // namespace pg::usd
