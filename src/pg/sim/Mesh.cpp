#include "pg/sim/Mesh.h"

#include "pg/io/Obj.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace pg::sim {
namespace {

/// The point of triangle abc nearest p (Ericson, Real-Time Collision
/// Detection, 5.1.5): which of the regions round the triangle p is in.
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

/// Where the ray o + t d meets triangle abc (Moeller and Trumbore).
bool rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 pv = cross(d, e2);
    const float det = dot(e1, pv);
    if (std::fabs(det) < 1e-30f) return false;
    const float inv = 1.0f / det;
    const Vec3 tv = o - a;
    const float u = dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 qv = cross(tv, e1);
    const float v = dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, qv) * inv;
    return true;
}

}  // namespace

void TriangleMesh::bounds(Vec3& lo, Vec3& hi) const {
    lo = Vec3(1e30f);
    hi = Vec3(-1e30f);
    for (const auto& tri : triangles) {
        for (const uint32_t v : tri) {
            const Vec3& p = positions[v];
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
    }
    if (triangles.empty()) lo = hi = Vec3();
}

bool parseObj(std::string_view text, TriangleMesh& out, std::string& error) {
    Geometry geo;
    if (!io::parseObj(text, geo, error)) return false;
    TriangleMesh m = triangulate(geo);
    if (m.triangles.empty()) {
        error = "no faces";
        return false;
    }
    out = std::move(m);
    return true;
}

TriangleMesh triangulate(const Geometry& geo) {
    TriangleMesh m;
    const auto P = geo.positions();
    m.positions.assign(P.begin(), P.end());
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto face = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < face.size(); ++i) m.triangles.push_back({face[0], face[i], face[i + 1]});
    }
    return m;
}

bool readObj(const std::string& path, TriangleMesh& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read it";
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (!parseObj(text.str(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

// --- the baked mesh ---------------------------------------------------------------------

MeshShape::MeshShape(TriangleMesh mesh, int resolution) : mesh_(std::move(mesh)), resolution_(std::clamp(resolution, 8, 256)) {
    Vec3 lo, hi;
    mesh_.bounds(lo, hi);
    center_ = (lo + hi) * 0.5f;
    const Vec3 size = hi - lo;
    const float longest = std::max({size.x, size.y, size.z, 1e-6f});
    for (int a = 0; a < 3; ++a) half_[a] = std::max(0.5f * size[a], 1e-3f * longest);
    bake();
}

void MeshShape::bake() {
    Vec3 lo, hi;
    mesh_.bounds(lo, hi);
    const Vec3 size = hi - lo;
    const float longest = std::max({size.x, size.y, size.z, 1e-6f});
    cell_ = longest / static_cast<float>(resolution_);
    const float margin = 4.0f * cell_;
    lo_ = lo - Vec3(margin);
    for (int a = 0; a < 3; ++a) n_[a] = static_cast<int>(std::ceil((size[a] + 2.0f * margin) / cell_)) + 1;
    const size_t total = static_cast<size_t>(n_[0]) * static_cast<size_t>(n_[1]) * static_cast<size_t>(n_[2]);
    field_.assign(total, 1e30f);
    std::vector<int> nearest(total, -1);
    auto point = [&](int i, int j, int k) {
        return lo_ + Vec3(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)) * cell_;
    };
    const auto& tris = mesh_.triangles;
    const auto& pos = mesh_.positions;
    auto distanceTo = [&](int tri, const Vec3& p) {
        const auto& t = tris[static_cast<size_t>(tri)];
        return length(p - closestOnTriangle(p, pos[t[0]], pos[t[1]], pos[t[2]]));
    };

    // 1. Exactly, close to each triangle.
    for (size_t t = 0; t < tris.size(); ++t) {
        const Vec3 &a = pos[tris[t][0]], &b = pos[tris[t][1]], &c = pos[tris[t][2]];
        int from[3], to[3];
        for (int ax = 0; ax < 3; ++ax) {
            const float mn = std::min({a[ax], b[ax], c[ax]}), mx = std::max({a[ax], b[ax], c[ax]});
            from[ax] = std::clamp(static_cast<int>(std::floor((mn - lo_[ax]) / cell_)) - 1, 0, n_[ax] - 1);
            to[ax] = std::clamp(static_cast<int>(std::ceil((mx - lo_[ax]) / cell_)) + 1, 0, n_[ax] - 1);
        }
        for (int k = from[2]; k <= to[2]; ++k) {
            for (int j = from[1]; j <= to[1]; ++j) {
                for (int i = from[0]; i <= to[0]; ++i) {
                    const size_t id = index(i, j, k);
                    const float d = distanceTo(static_cast<int>(t), point(i, j, k));
                    if (d < field_[id]) {
                        field_[id] = d;
                        nearest[id] = static_cast<int>(t);
                    }
                }
            }
        }
    }

    // 2. Out to the rest: sweep the grid in all eight diagonal directions,
    // each point trying the nearest triangles of the neighbours behind it.
    for (int sweep = 0; sweep < 8; ++sweep) {
        const int di = sweep & 1 ? -1 : 1, dj = sweep & 2 ? -1 : 1, dk = sweep & 4 ? -1 : 1;
        for (int k = dk > 0 ? 0 : n_[2] - 1; k >= 0 && k < n_[2]; k += dk) {
            for (int j = dj > 0 ? 0 : n_[1] - 1; j >= 0 && j < n_[1]; j += dj) {
                for (int i = di > 0 ? 0 : n_[0] - 1; i >= 0 && i < n_[0]; i += di) {
                    const size_t id = index(i, j, k);
                    for (int q = 1; q < 8; ++q) {
                        const int ni = i - (q & 1 ? di : 0), nj = j - (q & 2 ? dj : 0), nk = k - (q & 4 ? dk : 0);
                        if (ni < 0 || nj < 0 || nk < 0 || ni >= n_[0] || nj >= n_[1] || nk >= n_[2]) continue;
                        const int tri = nearest[index(ni, nj, nk)];
                        if (tri < 0 || tri == nearest[id]) continue;
                        const float d = distanceTo(tri, point(i, j, k));
                        if (d < field_[id]) {
                            field_[id] = d;
                            nearest[id] = tri;
                        }
                    }
                }
            }
        }
    }

    // 3. Inside or outside: along each axis, crossings counted from the
    // low side, for each shell -- the triangles joined through corners at
    // the same place -- on its own; inside an odd number of times is inside
    // it, and inside any shell is inside. So shells that overlap -- balls
    // round points, copies, merged shapes -- fill their overlap too. The rays
    // are moved off the grid lines by a hair, so that they do not run
    // exactly through the vertices of a mesh made on the same round numbers.
    // Two votes of three win.
    std::vector<uint32_t> shell(tris.size(), 0);
    uint32_t shells = 0;
    {
        // Corners at the same place are one corner.
        std::vector<uint32_t> order(pos.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        auto bitsOf = [&](uint32_t i) {
            std::array<uint32_t, 3> b{};
            for (int a = 0; a < 3; ++a) {
                const float x = pos[i][a] + 0.0f;  // -0 is 0
                std::memcpy(&b[static_cast<size_t>(a)], &x, sizeof x);
            }
            return b;
        };
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const auto ba = bitsOf(a), bb = bitsOf(b);
            return ba != bb ? ba < bb : a < b;
        });
        std::vector<uint32_t> same(pos.size());
        for (size_t i = 0; i < order.size(); ++i) {
            same[order[i]] = i > 0 && bitsOf(order[i]) == bitsOf(order[i - 1]) ? same[order[i - 1]] : order[i];
        }
        std::vector<uint32_t> parent(pos.size());
        for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
        auto root = [&](uint32_t i) {
            while (parent[i] != i) i = parent[i] = parent[parent[i]];
            return i;
        };
        for (const auto& t : tris) {
            const uint32_t a = root(same[t[0]]);
            for (int c = 1; c < 3; ++c) {
                const uint32_t b = root(same[t[static_cast<size_t>(c)]]);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
        std::vector<uint32_t> number(pos.size(), ~0u);
        for (size_t t = 0; t < tris.size(); ++t) {
            uint32_t& n = number[root(same[tris[t][0]])];
            if (n == ~0u) n = shells++;
            shell[t] = n;
        }
    }
    struct Crossing {
        uint32_t ray;   ///< iu + n[u] * iv
        int32_t at;     ///< the first grid point past it
        uint32_t shell;
    };
    std::vector<Crossing> crossings;
    std::vector<uint8_t> odd(shells, 0);
    std::vector<uint8_t> votes(total, 0);
    for (int ax = 0; ax < 3; ++ax) {
        const int u = (ax + 1) % 3, v = (ax + 2) % 3;
        const float offU = 1.13e-4f * cell_, offV = 2.71e-4f * cell_;
        crossings.clear();
        auto at = [&](int ia, int iu, int iv) {
            int ijk[3];
            ijk[ax] = ia;
            ijk[u] = iu;
            ijk[v] = iv;
            return index(ijk[0], ijk[1], ijk[2]);
        };
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            const auto& t = tris[ti];
            const Vec3 &a = pos[t[0]], &b = pos[t[1]], &c = pos[t[2]];
            const float au = a[u], av = a[v], bu = b[u], bv = b[v], cu = c[u], cv = c[v];
            const float area = (bu - au) * (cv - av) - (bv - av) * (cu - au);
            if (std::fabs(area) < 1e-30f) continue;  // edge on to the rays
            const int u0 = std::clamp(static_cast<int>(std::floor((std::min({au, bu, cu}) - lo_[u]) / cell_)), 0, n_[u] - 1);
            const int u1 = std::clamp(static_cast<int>(std::ceil((std::max({au, bu, cu}) - lo_[u]) / cell_)), 0, n_[u] - 1);
            const int v0 = std::clamp(static_cast<int>(std::floor((std::min({av, bv, cv}) - lo_[v]) / cell_)), 0, n_[v] - 1);
            const int v1 = std::clamp(static_cast<int>(std::ceil((std::max({av, bv, cv}) - lo_[v]) / cell_)), 0, n_[v] - 1);
            for (int iv = v0; iv <= v1; ++iv) {
                for (int iu = u0; iu <= u1; ++iu) {
                    const float pu = lo_[u] + static_cast<float>(iu) * cell_ + offU;
                    const float pv = lo_[v] + static_cast<float>(iv) * cell_ + offV;
                    const float w0 = (bu - pu) * (cv - pv) - (bv - pv) * (cu - pu);
                    const float w1 = (cu - pu) * (av - pv) - (cv - pv) * (au - pu);
                    const float w2 = (au - pu) * (bv - pv) - (av - pv) * (bu - pu);
                    const bool inside = (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) || (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
                    if (!inside) continue;
                    const float x = (w0 * a[ax] + w1 * b[ax] + w2 * c[ax]) / (w0 + w1 + w2);
                    const int ia = std::max(0, static_cast<int>(std::ceil((x - lo_[ax]) / cell_)));
                    if (ia < n_[ax]) {
                        crossings.push_back({static_cast<uint32_t>(iu + n_[u] * iv), ia, shell[ti]});
                    }
                }
            }
        }
        std::sort(crossings.begin(), crossings.end(), [](const Crossing& p, const Crossing& q) {
            return p.ray != q.ray ? p.ray < q.ray : p.at < q.at;
        });
        size_t next = 0;
        for (int iv = 0; iv < n_[v]; ++iv) {
            for (int iu = 0; iu < n_[u]; ++iu) {
                const uint32_t ray = static_cast<uint32_t>(iu + n_[u] * iv);
                const size_t first = next;
                int in = 0;  // shells we are inside of
                for (int ia = 0; ia < n_[ax]; ++ia) {
                    for (; next < crossings.size() && crossings[next].ray == ray && crossings[next].at == ia; ++next) {
                        uint8_t& o = odd[crossings[next].shell];
                        o ^= 1;
                        in += o ? 1 : -1;
                    }
                    const size_t id = at(ia, iu, iv);
                    votes[id] = static_cast<uint8_t>(votes[id] + (in > 0 ? 1 : 0));
                }
                for (size_t e = first; e < next; ++e) odd[crossings[e].shell] = 0;
            }
        }
    }
    for (size_t id = 0; id < total; ++id) {
        if (votes[id] >= 2) field_[id] = -field_[id];
    }
}

float MeshShape::distance(const Vec3& p) const {
    // A point that is nowhere -- a particle that blew up -- is far away.
    if (!std::isfinite(p.x + p.y + p.z)) return 1e30f;
    // Into the grid, and the way there added.
    float g[3];
    Vec3 inside;
    for (int a = 0; a < 3; ++a) {
        const float top = static_cast<float>(n_[a] - 1);
        g[a] = std::clamp((p[a] - lo_[a]) / cell_, 0.0f, top);
        inside[a] = lo_[a] + g[a] * cell_;
    }
    int i0[3];
    float f[3];
    for (int a = 0; a < 3; ++a) {
        i0[a] = std::min(static_cast<int>(g[a]), n_[a] - 2);
        f[a] = g[a] - static_cast<float>(i0[a]);
    }
    auto v = [&](int di, int dj, int dk) { return field_[index(i0[0] + di, i0[1] + dj, i0[2] + dk)]; };
    auto lerp = [](float x, float y, float t) { return x + (y - x) * t; };
    const float y0 = lerp(lerp(v(0, 0, 0), v(1, 0, 0), f[0]), lerp(v(0, 1, 0), v(1, 1, 0), f[0]), f[1]);
    const float y1 = lerp(lerp(v(0, 0, 1), v(1, 0, 1), f[0]), lerp(v(0, 1, 1), v(1, 1, 1), f[0]), f[1]);
    return lerp(y0, y1, f[2]) + length(p - inside);
}

bool MeshShape::intersect(const Vec3& origin, const Vec3& dir, float tMin, float& tHit, Vec3& normal) const {
    // The box round the mesh first.
    const Vec3 lo = center_ - half_, hi = center_ + half_;
    float t0 = tMin, t1 = 1e30f;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dir[a]) < 1e-20f) {
            if (origin[a] < lo[a] - 1e-6f || origin[a] > hi[a] + 1e-6f) return false;
            continue;
        }
        float ta = (lo[a] - origin[a]) / dir[a], tb = (hi[a] - origin[a]) / dir[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta - 1e-5f);
        t1 = std::min(t1, tb + 1e-5f);
    }
    if (t0 > t1) return false;
    float best = 1e30f;
    int hit = -1;
    const auto& pos = mesh_.positions;
    for (size_t i = 0; i < mesh_.triangles.size(); ++i) {
        const auto& tri = mesh_.triangles[i];
        float t = 0.0f;
        if (rayTriangle(origin, dir, pos[tri[0]], pos[tri[1]], pos[tri[2]], t) && t >= tMin && t < best) {
            best = t;
            hit = static_cast<int>(i);
        }
    }
    if (hit < 0) return false;
    const auto& tri = mesh_.triangles[static_cast<size_t>(hit)];
    Vec3 n = normalize(cross(pos[tri[1]] - pos[tri[0]], pos[tri[2]] - pos[tri[0]]));
    if (dot(n, dir) > 0.0f) n = n * -1.0f;
    tHit = best;
    normal = n;
    return true;
}

std::shared_ptr<const MeshShape> loadMesh(const std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = path + ": no such file";
        return nullptr;
    }
    const fs::file_time_type time = fs::last_write_time(path, ec);
    const std::string key = fs::weakly_canonical(path, ec).string();
    struct Entry {
        uintmax_t size = 0;
        fs::file_time_type time;
        std::weak_ptr<const MeshShape> mesh;
    };
    static std::mutex mu;
    static std::map<std::string, Entry> cache;
    std::lock_guard<std::mutex> lock(mu);
    const auto it = cache.find(key);
    if (it != cache.end() && it->second.size == size && it->second.time == time) {
        if (auto kept = it->second.mesh.lock()) return kept;
    }
    TriangleMesh mesh;
    if (!readObj(path, mesh, error)) return nullptr;
    auto shape = std::make_shared<MeshShape>(std::move(mesh), 48);
    shape->path = path;
    cache[key] = {size, time, shape};
    return shape;
}

std::shared_ptr<const MeshShape> meshFromGeometry(const Geometry& geo, float radius) {
    uint64_t key = geo.hash();
    uint32_t bits = 0;
    std::memcpy(&bits, &radius, sizeof bits);
    key ^= static_cast<uint64_t>(bits) * 0x9e3779b97f4a7c15ull;
    static std::mutex mu;
    static std::map<uint64_t, std::weak_ptr<const MeshShape>> cache;
    {
        std::lock_guard<std::mutex> lock(mu);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            if (auto kept = it->second.lock()) return kept;
        }
    }
    TriangleMesh mesh = triangulate(geo);
    if (mesh.triangles.empty()) {
        // Points: an icosahedron round each.
        const size_t n = std::min(geo.pointCount(), kMaxShapePoints);
        if (n == 0) return nullptr;
        const float g = 0.5f * (1.0f + std::sqrt(5.0f));
        const Vec3 corner[12] = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                                 {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
        const uint32_t face[20][3] = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                      {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                      {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
        const auto P = geo.positions();
        const AttributeArray* pscale = geo.points().find("pscale");
        const bool sized = pscale && pscale->type() == AttrType::Float;
        const float unit = 1.0f / length(corner[0]);
        mesh = TriangleMesh();
        for (size_t i = 0; i < n; ++i) {
            const float r = std::max(sized ? pscale->read<float>()[i] : radius, 1e-4f);
            const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
            for (const Vec3& c : corner) mesh.positions.push_back(P[i] + c * (r * unit));
            for (const auto& f : face) mesh.triangles.push_back({base + f[0], base + f[1], base + f[2]});
        }
    }
    auto shape = std::make_shared<MeshShape>(std::move(mesh), 64);
    std::lock_guard<std::mutex> lock(mu);
    cache[key] = shape;
    return shape;
}

}  // namespace pg::sim
