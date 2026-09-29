#include "pg/sim/Cloth.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <utility>

namespace pg::sim {

namespace {

/// The air: how heavy a cubic metre is, kg, and how hard it pushes a plate
/// that faces it (its drag coefficient).
constexpr float kAirDensity = 1.2f;
constexpr float kPlateDrag = 1.2f;
/// The most colours the links are sorted into; those that fit none are
/// solved one after another.
constexpr int kColours = 64;
/// How hard a quad holds its shape -- its diagonals -- as a share of how
/// hard its edges hold their length: woven cloth gives far more to a shear
/// than to a pull.
constexpr float kShear = 1e-4f;

/// A point attribute as a number: Float or Int; `fallback` without it.
float pointNumber(const AttributeArray* a, size_t i, float fallback) {
    if (!a || i >= a->size()) return fallback;
    if (a->type() == AttrType::Float) return a->read<float>()[i];
    if (a->type() == AttrType::Int) return static_cast<float>(a->read<int32_t>()[i]);
    return fallback;
}

uint64_t pairKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

/// Six times the signed volume of the tetrahedron of the origin and a triangle.
double tripleProduct(const Vec3& a, const Vec3& b, const Vec3& c) {
    return static_cast<double>(dot(a, cross(b, c)));
}

}  // namespace

ClothScene ClothScene::sanitized() const {
    ClothScene c = *this;
    ClothSettings& s = c.solver;
    const ClothSettings d;
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    s.density = std::clamp(finite(s.density, d.density), 1e-4f, 1e4f);
    s.stretch = std::clamp(finite(s.stretch, d.stretch), 1e-2f, 1e9f);
    s.bend = std::clamp(finite(s.bend, d.bend), 0.0f, 1e9f);
    s.pressure = std::clamp(finite(s.pressure, d.pressure), 0.0f, 100.0f);
    s.thickness = std::clamp(finite(s.thickness, d.thickness), 1e-4f, 1.0f);
    s.friction = std::clamp(finite(s.friction, d.friction), 0.0f, 10.0f);
    s.damping = std::clamp(finite(s.damping, d.damping), 0.0f, 1000.0f);
    s.airDrag = std::clamp(finite(s.airDrag, d.airDrag), 0.0f, 100.0f);
    s.substeps = std::clamp(s.substeps, 1, 200);
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.timeStep = std::clamp(finite(s.timeStep, d.timeStep), 1e-4f, 1.0f);
    detail::sanitize(c.colliders);
    return c;
}

// --- the frame, drawn ------------------------------------------------------------------

std::shared_ptr<Geometry> posedCloth(const ClothFrame& f) {
    if (!f.geometry || f.geometry->pointCount() != f.positions.size()) return nullptr;
    auto g = std::make_shared<Geometry>(*f.geometry);
    const size_t n = f.positions.size();
    {
        auto P = g->positionsForWrite();
        std::copy(f.positions.begin(), f.positions.end(), P.begin());
    }
    if (f.velocities.size() == 3 * n) {
        auto v = g->points().create("v", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < n; ++i) {
            v[i] = Vec3(floatFromHalf(f.velocities[3 * i]), floatFromHalf(f.velocities[3 * i + 1]),
                        floatFromHalf(f.velocities[3 * i + 2]));
        }
    }
    // Smooth: each point's normal the sum of the faces round it, each as big
    // as it is.
    std::vector<Vec3> sum(n);
    for (size_t p = 0; p < g->primitiveCount(); ++p) {
        if (!g->primitiveClosed(p)) continue;
        const auto c = g->primitivePoints(p);
        for (size_t k = 1; k + 1 < c.size(); ++k) {
            const Vec3 a = f.positions[c[0]], b = f.positions[c[k]], d = f.positions[c[k + 1]];
            const Vec3 face = cross(b - a, d - a);
            sum[c[0]] += face;
            sum[c[k]] += face;
            sum[c[k + 1]] += face;
        }
    }
    auto N = g->points().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < n; ++i) {
        const float l = length(sum[i]);
        N[i] = l > 1e-12f ? sum[i] * (1.0f / l) : Vec3(0.0f, 1.0f, 0.0f);
    }
    return g;
}

std::shared_ptr<Geometry> drawnCloth(const ClothFrame& f, const Vec3& color) {
    auto g = posedCloth(f);
    if (!g) return nullptr;
    if (!g->primitives().contains("Cd") && !g->points().contains("Cd")) {
        auto cd = g->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
        std::fill(cd.begin(), cd.end(), color);
    }
    return g;
}

// --- set-up ----------------------------------------------------------------------------

ClothSolver::ClothSolver(const ClothScene& scene) : scene_(scene.sanitized()) {
    build();
    setScene(scene);
}

void ClothSolver::build() {
    const Geometry* geo = scene_.geometry.get();
    if (!geo) return;
    const size_t n = geo->pointCount();
    const auto P = geo->positions();
    x_.assign(P.begin(), P.end());
    rest_ = x_;
    v_.assign(n, Vec3());
    if (const AttributeArray* v = geo->points().find("v"); v && v->type() == AttrType::Vec3 && v->size() == n) {
        const auto vel = v->read<Vec3>();
        v_.assign(vel.begin(), vel.end());
    }
    prev_ = x_;
    start_ = x_;
    target_ = x_;
    pinned_.assign(n, 0);
    const AttributeArray* pin = geo->points().find("pin");
    for (size_t i = 0; i < n; ++i) pinned_[i] = pointNumber(pin, i, 0.0f) > 0.5f ? 1 : 0;

    // The triangles -- polygons as fans -- and the links: the edges of the
    // polygons and the segments of the lines; the diagonals of quads.
    const ClothSettings& s = scene_.solver;
    std::vector<double> mass(n, 0.0);
    std::map<uint64_t, uint32_t> linked;  // pair -> link
    auto link = [&](uint32_t a, uint32_t b, float compliance) {
        if (a == b || a >= n || b >= n) return;
        const uint64_t key = pairKey(a, b);
        if (linked.count(key)) return;
        linked[key] = static_cast<uint32_t>(links_.size());
        links_.push_back({a, b, length(x_[a] - x_[b]), compliance});
    };
    const float stretchCompliance = 1.0f / s.stretch;
    std::vector<std::array<uint32_t, 3>> ropeBends;
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        const auto c = geo->primitivePoints(p);
        if (c.size() < 2) continue;
        if (!geo->primitiveClosed(p) || c.size() == 2) {
            // A rope: its segments, and each point to the one after the next.
            for (size_t k = 0; k + 1 < c.size(); ++k) {
                link(c[k], c[k + 1], stretchCompliance);
                const double half = 0.5 * s.density * length(x_[c[k]] - x_[c[k + 1]]);
                mass[c[k]] += half;
                mass[c[k + 1]] += half;
                if (k + 2 < c.size()) ropeBends.push_back({c[k], c[k + 1], c[k + 2]});
            }
            continue;
        }
        for (size_t k = 0; k < c.size(); ++k) link(c[k], c[(k + 1) % c.size()], stretchCompliance);
        if (c.size() == 4) {
            link(c[0], c[2], stretchCompliance / kShear);
            link(c[1], c[3], stretchCompliance / kShear);
        }
        for (size_t k = 1; k + 1 < c.size(); ++k) {
            const uint32_t t[3] = {c[0], c[k], c[k + 1]};
            tris_.insert(tris_.end(), t, t + 3);
            const double third = s.density * 0.5 * length(cross(x_[t[1]] - x_[t[0]], x_[t[2]] - x_[t[0]])) / 3.0;
            for (const uint32_t q : t) mass[q] += third;
        }
    }
    stretchCount_ = links_.size();

    // Bending: across each edge two triangles share, their far points; along
    // a rope, each point and the one after the next.
    if (s.bend > 0.0f) {
        const float bendCompliance = 1.0f / s.bend;
        std::map<uint64_t, std::vector<uint32_t>> across;  // edge -> the far points of its triangles
        for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = tris_[t + static_cast<size_t>(e)], b = tris_[t + static_cast<size_t>((e + 1) % 3)];
                across[pairKey(a, b)].push_back(tris_[t + static_cast<size_t>((e + 2) % 3)]);
            }
        }
        for (const auto& [edge, far] : across) {
            if (far.size() == 2) link(far[0], far[1], bendCompliance);
        }
        for (const auto& r : ropeBends) link(r[0], r[2], bendCompliance);
    }
    bendCount_ = links_.size() - stretchCount_;

    // Mass: the cloth round each point, or its attribute.
    const AttributeArray* given = geo->points().find("mass");
    w_.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double m = std::max(static_cast<double>(pointNumber(given, i, static_cast<float>(mass[i]))), 1e-6);
        w_[i] = pinned_[i] ? 0.0f : static_cast<float>(1.0 / m);
    }

    // Each point's triangles, for the air's push.
    triStart_.assign(n + 1, 0);
    for (const uint32_t q : tris_) ++triStart_[q + 1];
    for (size_t i = 0; i < n; ++i) triStart_[i + 1] += triStart_[i];
    triOf_.assign(tris_.size(), 0);
    {
        std::vector<uint32_t> at(triStart_.begin(), triStart_.end() - 1);
        for (size_t k = 0; k < tris_.size(); ++k) triOf_[at[tris_[k]]++] = static_cast<uint32_t>(k / 3);
    }

    // The closed meshes -- every edge of their triangles shared by two: the
    // air reaches only their outside, and with pressure they are balloons.
    outside_.assign(tris_.size() / 3, 0);
    if (!tris_.empty()) {
        const size_t count = tris_.size() / 3;
        std::vector<uint32_t> parent(n);
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](uint32_t a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        };
        for (size_t t = 0; t < count; ++t) {
            const uint32_t a = find(tris_[3 * t]);
            for (int k = 1; k < 3; ++k) {
                const uint32_t b = find(tris_[3 * t + static_cast<size_t>(k)]);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
        std::map<uint32_t, std::vector<uint32_t>> parts;  // root -> triangles
        for (size_t t = 0; t < count; ++t) parts[find(tris_[3 * t])].push_back(static_cast<uint32_t>(t));
        for (const auto& [root, triangles] : parts) {
            std::map<uint64_t, int> uses;
            for (const uint32_t t : triangles) {
                for (int e = 0; e < 3; ++e) {
                    ++uses[pairKey(tris_[3 * t + static_cast<size_t>(e)], tris_[3 * t + static_cast<size_t>((e + 1) % 3)])];
                }
            }
            const bool closed = std::all_of(uses.begin(), uses.end(), [](const auto& u) { return u.second == 2; });
            if (!closed) continue;
            Balloon b;
            b.triangles = triangles;
            double volume = 0.0;
            for (const uint32_t t : triangles) {
                volume += tripleProduct(x_[tris_[3 * t]], x_[tris_[3 * t + 1]], x_[tris_[3 * t + 2]]);
            }
            b.rest = static_cast<float>(volume / 6.0);
            if (std::fabs(b.rest) <= 1e-9f) continue;
            for (const uint32_t t : triangles) outside_[t] = b.rest > 0.0f ? 1 : -1;
            if (s.pressure > 0.0f) balloons_.push_back(std::move(b));
        }
    }

    // Colours: each link the first none of whose points has it yet.
    std::vector<uint64_t> used(n, 0);
    colours_.assign(kColours, {});
    for (uint32_t l = 0; l < links_.size(); ++l) {
        const uint64_t taken = used[links_[l].a] | used[links_[l].b];
        if (~taken == 0) {
            leftOver_.push_back(l);
            continue;
        }
        const int c = std::countr_one(taken);
        colours_[static_cast<size_t>(c)].push_back(l);
        used[links_[l].a] |= 1ull << c;
        used[links_[l].b] |= 1ull << c;
    }
    while (!colours_.empty() && colours_.back().empty()) colours_.pop_back();

    // Each point's neighbours -- what it is linked to -- which it does not
    // push off itself; and how close it lets the rest come: half an edge.
    std::vector<std::vector<uint32_t>> nb(n);
    double edges = 0.0;
    for (size_t l = 0; l < links_.size(); ++l) {
        nb[links_[l].a].push_back(links_[l].b);
        nb[links_[l].b].push_back(links_[l].a);
        if (l < stretchCount_) edges += links_[l].rest;
    }
    nearStart_.assign(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        std::sort(nb[i].begin(), nb[i].end());
        nearStart_[i + 1] = nearStart_[i] + static_cast<uint32_t>(nb[i].size());
        near_.insert(near_.end(), nb[i].begin(), nb[i].end());
    }
    const float meanEdge = stretchCount_ ? static_cast<float>(edges / static_cast<double>(stretchCount_)) : s.thickness;
    selfRadius_ = std::max(s.thickness, 0.3f * meanEdge);
    airOfTri_.assign(tris_.size() / 3, Vec3());
}

void ClothSolver::setScene(const ClothScene& scene) {
    ClothScene next = scene.sanitized();
    // What it is made of and how it is held together stay.
    next.geometry = scene_.geometry;
    next.solver.density = scene_.solver.density;
    next.solver.stretch = scene_.solver.stretch;
    next.solver.bend = scene_.solver.bend;
    next.solver.pressure = scene_.solver.pressure;
    scene_ = std::move(next);
    shapes_.clear();
    for (const Collider& c : scene_.colliders) shapes_.push_back(c.instance());
}

void ClothSolver::setAir(std::function<Vec3(const Vec3&)> air) { air_ = std::move(air); }

float ClothSolver::balloonVolume(size_t b) const {
    double volume = 0.0;
    for (const uint32_t t : balloons_[b].triangles) volume += tripleProduct(x_[tris_[3 * t]], x_[tris_[3 * t + 1]], x_[tris_[3 * t + 2]]);
    return static_cast<float>(volume / 6.0);
}

// --- a step ----------------------------------------------------------------------------

void ClothSolver::aero(std::vector<Vec3>& accel) const {
    const ClothSettings& s = scene_.solver;
    const size_t count = tris_.size() / 3;
    std::vector<Vec3> force(count);
    const float h = s.timeStep / static_cast<float>(s.substeps);
    pg::parallelFor(count, 256, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const uint32_t a = tris_[3 * t], b = tris_[3 * t + 1], c = tris_[3 * t + 2];
            const Vec3 middle = (x_[a] + x_[b] + x_[c]) * (1.0f / 3.0f);
            Vec3 flow = airOfTri_[t];
            for (const Force& f : scene_.forces) {
                if (f.kind == ForceKind::Wind) flow += detail::windAt(f, time_, f.seed * 7919u + 17u, middle);
            }
            const Vec3 face = cross(x_[b] - x_[a], x_[c] - x_[a]);
            const float twice = length(face);
            if (twice < 1e-12f) continue;
            const Vec3 normal = face * (1.0f / twice);
            const Vec3 rel = flow - (v_[a] + v_[b] + v_[c]) * (1.0f / 3.0f);
            const float along = dot(rel, normal);
            // A closed mesh: only where the air comes at its outside.
            if (outside_[t] != 0 && along * static_cast<float>(outside_[t]) >= 0.0f) continue;
            // Pushed along its normal as a plate is, as hard as the air comes
            // at it -- no more in a substep than takes it the air's way.
            float push = 0.5f * kAirDensity * kPlateDrag * s.airDrag * 0.5f * twice * std::fabs(along) * along;
            float lightest = 1e30f;
            for (const uint32_t q : {a, b, c}) {
                if (w_[q] > 0.0f) lightest = std::min(lightest, 1.0f / w_[q]);
            }
            if (lightest < 1e30f) {
                const float most = 3.0f * lightest * std::fabs(along) / h;
                push = std::clamp(push, -most, most);
            }
            force[t] = normal * push;
        }
    });
    const size_t n = x_.size();
    pg::parallelFor(n, 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Vec3 sum;
            for (uint32_t k = triStart_[i]; k < triStart_[i + 1]; ++k) sum += force[triOf_[k]];
            accel[i] = sum * (w_[i] / 3.0f);
        }
    });
}

void ClothSolver::solveLinks(float h) {
    const float h2 = h * h;
    auto solve = [&](const Link& l) {
        const float wa = w_[l.a], wb = w_[l.b];
        const float wsum = wa + wb;
        if (wsum <= 0.0f) return;
        const Vec3 d = x_[l.b] - x_[l.a];
        const float len = length(d);
        if (len < 1e-9f) return;
        const Vec3 n = d * (1.0f / len);
        const float lambda = -(len - l.rest) / (wsum + l.compliance / h2);
        x_[l.a] = x_[l.a] - n * (wa * lambda);
        x_[l.b] += n * (wb * lambda);
    };
    for (const std::vector<uint32_t>& colour : colours_) {
        pg::parallelFor(colour.size(), 512, [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) solve(links_[colour[k]]);
        });
    }
    for (const uint32_t l : leftOver_) solve(links_[l]);
}

void ClothSolver::solveBalloons(float h) {
    (void)h;
    const float pressure = scene_.solver.pressure;
    for (const Balloon& b : balloons_) {
        // C = V - pressure * V0; its gradient at a point, the sum over its
        // triangles of the cross product of the other two corners, over 6.
        std::map<uint32_t, Vec3> grad;
        double volume = 0.0;
        for (const uint32_t t : b.triangles) {
            const uint32_t p0 = tris_[3 * t], p1 = tris_[3 * t + 1], p2 = tris_[3 * t + 2];
            volume += tripleProduct(x_[p0], x_[p1], x_[p2]);
            grad[p0] += cross(x_[p1], x_[p2]) * (1.0f / 6.0f);
            grad[p1] += cross(x_[p2], x_[p0]) * (1.0f / 6.0f);
            grad[p2] += cross(x_[p0], x_[p1]) * (1.0f / 6.0f);
        }
        const double c = volume / 6.0 - static_cast<double>(pressure) * b.rest;
        double sum = 0.0;
        for (const auto& [p, g] : grad) sum += w_[p] * static_cast<double>(dot(g, g));
        if (sum < 1e-18) continue;
        const float lambda = static_cast<float>(-c / sum);
        for (const auto& [p, g] : grad) x_[p] += g * (w_[p] * lambda);
    }
}

void ClothSolver::selfCollide() {
    const size_t n = x_.size();
    const float r = selfRadius_, reach = 2.0f * r;
    const float cell = reach;
    auto keyOf = [&](const Vec3& p, int di, int dj, int dk) {
        const int64_t i = static_cast<int64_t>(std::floor(p.x / cell)) + di;
        const int64_t j = static_cast<int64_t>(std::floor(p.y / cell)) + dj;
        const int64_t k = static_cast<int64_t>(std::floor(p.z / cell)) + dk;
        return static_cast<uint64_t>((i * 73856093) ^ (j * 19349663) ^ (k * 83492791));
    };
    std::vector<std::pair<uint64_t, uint32_t>> sorted(n);
    for (size_t i = 0; i < n; ++i) sorted[i] = {keyOf(x_[i], 0, 0, 0), static_cast<uint32_t>(i)};
    std::sort(sorted.begin(), sorted.end());
    std::vector<Vec3> push(n);
    pg::parallelFor(n, 256, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (w_[i] <= 0.0f) continue;
            Vec3 sum;
            const uint32_t* nb = near_.data() + nearStart_[i];
            const uint32_t* nbEnd = near_.data() + nearStart_[i + 1];
            for (int dk = -1; dk <= 1; ++dk) {
                for (int dj = -1; dj <= 1; ++dj) {
                    for (int di = -1; di <= 1; ++di) {
                        const uint64_t key = keyOf(x_[i], di, dj, dk);
                        auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(key, 0u));
                        for (; it != sorted.end() && it->first == key; ++it) {
                            const uint32_t j = it->second;
                            if (j == i || std::binary_search(nb, nbEnd, j)) continue;
                            // Points nearer than that at rest -- round the
                            // pole of a sphere -- kept no nearer than they were.
                            const float apart = std::min(reach, length(rest_[i] - rest_[j]));
                            const Vec3 d = x_[i] - x_[j];
                            const float dist = length(d);
                            if (dist >= apart || dist < 1e-9f) continue;
                            const float share = w_[i] / (w_[i] + w_[j]);
                            sum += d * ((apart - dist) * share / dist);
                        }
                    }
                }
            }
            push[i] = sum;
        }
    });
    for (size_t i = 0; i < n; ++i) x_[i] += push[i];
}

void ClothSolver::collide(float h) {
    const ClothSettings& s = scene_.solver;
    const float r = s.thickness;
    const float mu = s.friction;
    const size_t n = x_.size();
    // Out of what it went into, and held back along it by friction: no more
    // than it went in, against how that moves.
    auto rub = [&](size_t i, const Vec3& normal, float depth, const Vec3& surfaceVelocity) {
        const Vec3 moved = x_[i] - prev_[i] - surfaceVelocity * h;
        const Vec3 along = moved - normal * dot(moved, normal);
        const float slide = length(along);
        if (slide < 1e-12f) return;
        x_[i] = x_[i] - along * std::min(1.0f, mu * depth / slide);
    };
    pg::parallelFor(n, 512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (w_[i] <= 0.0f) continue;
            if (s.floor && x_[i].y < r) {
                const float depth = r - x_[i].y;
                x_[i].y = r;
                rub(i, Vec3(0.0f, 1.0f, 0.0f), depth, Vec3());
            }
            for (size_t c = 0; c < shapes_.size(); ++c) {
                const float d = shapes_[c].distance(x_[i]);
                if (d >= r) continue;
                const Vec3 normal = shapes_[c].normal(x_[i]);
                x_[i] += normal * (r - d);
                rub(i, normal, r - d, scene_.colliders[c].velocityAt(x_[i]));
            }
        }
    });
}

void ClothSolver::step() {
    const ClothSettings& s = scene_.solver;
    const size_t n = x_.size();
    if (n == 0) {
        ++frame_;
        time_ += s.timeStep;
        return;
    }
    const int steps = s.substeps;
    const float h = s.timeStep / static_cast<float>(steps);
    // Where the pins go: from where they are to where the geometry has them
    // at the end of the step.
    start_ = x_;
    if (scene_.target && scene_.target->pointCount() == n) {
        const auto P = scene_.target->positions();
        target_.assign(P.begin(), P.end());
    } else {
        target_ = start_;
        if (scene_.geometry) {
            const auto P = scene_.geometry->positions();
            for (size_t i = 0; i < n; ++i) {
                if (pinned_[i]) target_[i] = P[i];
            }
        }
    }
    // The gas's flow at each triangle, once a step.
    const size_t count = tris_.size() / 3;
    if (air_ && s.airDrag > 0.0f) {
        for (size_t t = 0; t < count; ++t) {
            const Vec3 middle = (x_[tris_[3 * t]] + x_[tris_[3 * t + 1]] + x_[tris_[3 * t + 2]]) * (1.0f / 3.0f);
            airOfTri_[t] = air_(middle);
        }
    } else {
        std::fill(airOfTri_.begin(), airOfTri_.end(), Vec3());
    }
    // Still air too holds it back: a sheet falls no faster than a couple of
    // metres a second, flat.
    const bool blown = s.airDrag > 0.0f && count > 0;
    std::vector<Vec3> accel(n);
    const float fade = 1.0f / (1.0f + s.damping * h);
    for (int k = 0; k < steps; ++k) {
        const float t = static_cast<float>(k + 1) / static_cast<float>(steps);
        if (blown) aero(accel);
        pg::parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                prev_[i] = x_[i];
                if (pinned_[i]) {
                    x_[i] = start_[i] + (target_[i] - start_[i]) * t;
                    continue;
                }
                v_[i] += (s.gravity + accel[i]) * h;
                x_[i] += v_[i] * h;
            }
        });
        solveLinks(h);
        if (!balloons_.empty()) solveBalloons(h);
        if (s.selfCollision && count > 0) selfCollide();
        collide(h);
        pg::parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) v_[i] = (x_[i] - prev_[i]) * (fade / h);
        });
        time_ += h;
    }
    time_ = static_cast<float>(frame_ + 1) * s.timeStep;
    ++frame_;
}

ClothFrame ClothSolver::capture() const {
    ClothFrame f;
    f.geometry = scene_.geometry;
    f.positions = x_;
    f.points = x_.size();
    f.velocities.reserve(3 * v_.size());
    for (const Vec3& v : v_) {
        for (int a = 0; a < 3; ++a) f.velocities.push_back(halfFromFloat(v[a]));
    }
    return f;
}

// --- the state -------------------------------------------------------------------------

void ClothSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.list(x_);
    out.list(v_);
}

bool ClothSolver::loadState(StateReader& in) {
    int32_t frame = 0;
    float time = 0.0f;
    std::vector<Vec3> x, v;
    if (!in.pod(frame) || !in.pod(time) || !in.list(x) || !in.list(v)) return false;
    if (x.size() != x_.size() || v.size() != v_.size()) return in.fail();
    x_ = std::move(x);
    v_ = std::move(v);
    prev_ = x_;
    frame_ = frame;
    time_ = time;
    return true;
}

}  // namespace pg::sim
