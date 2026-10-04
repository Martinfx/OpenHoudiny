#include "pg/sim/Grains.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace pg::sim {

namespace {

constexpr float kPi = 3.14159265358979f;
/// How much slower than static friction a sliding contact is held back.
constexpr float kKinetic = 0.75f;
/// How much more of a contact's correction the upper grain takes: e^(k up),
/// `up` how far above the other it is, as a share of the two radii.
constexpr float kShock = 0.5f;
/// How far past touching cohesion reaches, as a share of the smaller radius.
constexpr float kCohesionReach = 0.5f;
/// No faster than this, metres a second: a grain flung out of something.
constexpr float kMaxSpeed = 60.0f;
/// How fast, metres a second, a grain put out of others may come off them.
constexpr float kSpringOff = 0.5f;
/// Below the floor this far -- without a floor -- a grain is gone.
constexpr float kGone = -50.0f;

float pointNumber(const AttributeArray* a, size_t i, float fallback) {
    if (!a || i >= a->size()) return fallback;
    if (a->type() == AttrType::Float) return a->read<float>()[i];
    if (a->type() == AttrType::Int) return static_cast<float>(a->read<int32_t>()[i]);
    return fallback;
}

/// A number 0 to 1 of a grain's own, the k-th.
float unit(uint32_t id, uint32_t k) {
    uint32_t h = id * 0x9e3779b1u + k * 0x85ebca77u + 0x165667b1u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000);
}

uint8_t byteOf(float c) { return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

/// The cell of a point, in cells of `size`.
inline void cellOf(const Vec3& p, float inv, int& x, int& y, int& z) {
    x = static_cast<int>(std::floor(p.x * inv));
    y = static_cast<int>(std::floor(p.y * inv));
    z = static_cast<int>(std::floor(p.z * inv));
}

inline uint32_t cellKey(int x, int y, int z, uint32_t mask) {
    return (static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
            static_cast<uint32_t>(z) * 83492791u) &
           mask;
}

}  // namespace

GrainScene GrainScene::sanitized() const {
    GrainScene g = *this;
    GrainSettings& s = g.solver;
    const GrainSettings d;
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    s.radius = std::clamp(finite(s.radius, d.radius), 1e-4f, 1.0f);
    s.sizeVariance = std::clamp(finite(s.sizeVariance, d.sizeVariance), 0.0f, 0.9f);
    s.density = std::clamp(finite(s.density, d.density), 1.0f, 1e5f);
    s.friction = std::clamp(finite(s.friction, d.friction), 0.0f, 10.0f);
    s.cohesion = std::clamp(finite(s.cohesion, d.cohesion), 0.0f, 1.0f);
    s.restSpeed = std::clamp(finite(s.restSpeed, d.restSpeed), 0.0f, 10.0f);
    s.damping = std::clamp(finite(s.damping, d.damping), 0.0f, 1000.0f);
    s.airDrag = std::clamp(finite(s.airDrag, d.airDrag), 0.0f, 1000.0f);
    s.emitFrames = std::clamp(s.emitFrames, 1, 100000);
    s.maxGrains = std::clamp(s.maxGrains, 0, 20000000);
    s.substeps = std::clamp(s.substeps, 1, 200);
    s.iterations = std::clamp(s.iterations, 1, 50);
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.timeStep = std::clamp(finite(s.timeStep, d.timeStep), 1e-4f, 1.0f);
    for (int a = 0; a < 3; ++a) g.color[a] = std::clamp(finite(g.color[a], 0.5f), 0.0f, 1.0f);
    detail::sanitize(g.colliders);
    detail::sanitize(g.forces);
    return g;
}

// --- the frame, as points ----------------------------------------------------------------

bool GrainFrame::fits() const {
    const size_t n = positions.size();
    return velocities.size() == 3 * n && radii.size() == n && ids.size() == n &&
           (colors.empty() || colors.size() == 3 * n);
}

std::shared_ptr<Geometry> grainPoints(const GrainFrame& f, const Vec3& color) {
    auto g = std::make_shared<Geometry>();
    if (f.empty() || !f.fits()) return g;
    const size_t n = f.size();
    g->addPoints(n);
    auto P = g->positionsForWrite();
    auto v = g->points().create("v", AttrType::Vec3).write<Vec3>();
    auto pscale = g->points().create("pscale", AttrType::Float).write<float>();
    auto id = g->points().create("id", AttrType::Int).write<int32_t>();
    auto cd = g->points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto orient = g->points().create("orient", AttrType::Vec4).write<Vec4>();
    for (size_t i = 0; i < n; ++i) {
        P[i] = f.positions[i];
        v[i] = Vec3(floatFromHalf(f.velocities[3 * i]), floatFromHalf(f.velocities[3 * i + 1]),
                    floatFromHalf(f.velocities[3 * i + 2]));
        pscale[i] = floatFromHalf(f.radii[i]);
        id[i] = static_cast<int32_t>(f.ids[i]);
        cd[i] = f.colors.empty() ? color
                                 : Vec3(f.colors[3 * i], f.colors[3 * i + 1], f.colors[3 * i + 2]) * (1.0f / 255.0f);
        // Turned its own way, at random -- the same every frame (Shoemake's
        // uniform rotation from three numbers).
        const float u1 = unit(f.ids[i], 11), u2 = unit(f.ids[i], 12), u3 = unit(f.ids[i], 13);
        const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
        orient[i] = Vec4(a * std::sin(2.0f * kPi * u2), a * std::cos(2.0f * kPi * u2), b * std::sin(2.0f * kPi * u3),
                         b * std::cos(2.0f * kPi * u3));
    }
    return g;
}

// --- set-up ------------------------------------------------------------------------------

GrainSolver::GrainSolver(const GrainScene& scene) : scene_(scene.sanitized()) {
    for (const Collider& c : scene_.colliders) shapes_.push_back(c.instance());
}

void GrainSolver::setScene(const GrainScene& scene) {
    GrainScene next = scene.sanitized();
    // What the grains are made of and how they behave stay.
    next.geometry = scene_.geometry;
    next.solver = scene_.solver;
    next.solver.gravity = scene.sanitized().solver.gravity;
    scene_ = std::move(next);
    shapes_.clear();
    for (const Collider& c : scene_.colliders) shapes_.push_back(c.instance());
}

void GrainSolver::setAir(std::function<Vec3(const Vec3&)> air) { air_ = std::move(air); }

void GrainSolver::paint(size_t count) {
    const Vec3& c = scene_.color;
    for (size_t i = 0; i < count; ++i) color_.insert(color_.end(), {byteOf(c.x), byteOf(c.y), byteOf(c.z)});
}

void GrainSolver::add(std::span<const Vec3> at, std::span<const Vec3> velocity, std::span<const float> radius,
                      const Vec3* color) {
    const GrainSettings& s = scene_.solver;
    const size_t n = std::min({at.size(), velocity.size(), radius.size()});
    size_t room = static_cast<size_t>(s.maxGrains) > x_.size() ? static_cast<size_t>(s.maxGrains) - x_.size() : 0;
    if (n == 0 || room == 0) return;
    // Coloured, the grains there are take the look's colour, if they have
    // none of their own.
    if (color && color_.empty()) paint(x_.size());
    for (size_t i = 0; i < n && room > 0; ++i) {
        const Vec3 p = at[i], v = velocity[i];
        if (!std::isfinite(p.x + p.y + p.z + v.x + v.y + v.z) || !std::isfinite(radius[i])) continue;
        const float r = std::clamp(radius[i], 1e-4f, 1.0f);
        x_.push_back(p);
        v_.push_back(v);
        r_.push_back(r);
        w_.push_back(1.0f / (s.density * (4.0f / 3.0f) * kPi * r * r * r));
        id_.push_back(nextId_++);
        if (color) {
            color_.insert(color_.end(), {byteOf(color->x), byteOf(color->y), byteOf(color->z)});
        } else if (!color_.empty()) {
            paint(1);
        }
        --room;
        fresh_ = true;
    }
}

void GrainSolver::emit() {
    const GrainSettings& s = scene_.solver;
    const Geometry* geo = scene_.geometry.get();
    if (!geo || frame_ >= s.emitFrames || geo->pointCount() == 0) return;
    const size_t room = static_cast<size_t>(s.maxGrains) > x_.size() ? static_cast<size_t>(s.maxGrains) - x_.size() : 0;
    if (room == 0) return;
    const auto P = geo->positions();
    const AttributeArray* pscale = geo->points().find("pscale");
    const AttributeArray* cd = geo->points().find("Cd");
    const AttributeArray* vel = geo->points().find("v");
    const bool coloured = cd && cd->type() == AttrType::Vec3;
    const bool moving = vel && vel->type() == AttrType::Vec3;
    if (coloured && color_.empty() && !x_.empty()) paint(x_.size());
    // Only where no grain is in the way -- of those there already, in the
    // grid of where they are now, and of those made now: points closer
    // than their grains are wide are thinned out.
    const size_t before = x_.size();
    float biggest = 0.0f;
    for (const float r : r_) biggest = std::max(biggest, r);
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        biggest = std::max(biggest, pointNumber(pscale, i, s.radius) * (1.0f + s.sizeVariance));
    }
    if (before > 0) sortIntoCells(2.0f * biggest);
    const float inv = 1.0f / (2.0f * biggest);
    const float oldInv = 1.0f / cell_;
    const uint32_t mask = static_cast<uint32_t>(bucket_.size() > 1 ? bucket_.size() - 2 : 0);
    std::unordered_map<uint64_t, std::vector<uint32_t>> made;  // the new ones, by cell
    auto cellId = [](int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFFu) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFFu) << 21) |
               static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFFu);
    };
    auto near = [&](const Vec3& p, float r, uint32_t j) {
        const Vec3 d = x_[j] - p;
        const float reach = 0.9f * (r + r_[j]);
        return dot(d, d) < reach * reach;
    };
    auto inTheWay = [&](const Vec3& p, float r) {
        int cx, cy, cz;
        if (before > 0) {
            cellOf(p, oldInv, cx, cy, cz);
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const uint32_t k = cellKey(cx + dx, cy + dy, cz + dz, mask);
                        for (uint32_t o = bucket_[k]; o < bucket_[k + 1]; ++o) {
                            if (near(p, r, order_[o])) return true;
                        }
                    }
                }
            }
        }
        cellOf(p, inv, cx, cy, cz);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto cell = made.find(cellId(cx + dx, cy + dy, cz + dz));
                    if (cell == made.end()) continue;
                    for (const uint32_t j : cell->second) {
                        if (near(p, r, j)) return true;
                    }
                }
            }
        }
        return false;
    };
    const float rho = s.density;
    size_t added = 0;
    for (size_t i = 0; i < geo->pointCount() && added < room; ++i) {
        const uint32_t id = nextId_;
        const float base = std::max(pointNumber(pscale, i, s.radius), 1e-4f);
        const float r = base * (1.0f + s.sizeVariance * (2.0f * unit(id, 1) - 1.0f));
        // Each frame's a little off the last's: a stream, not columns.
        Vec3 p = P[i];
        if (frame_ > 0) {
            p += Vec3(unit(id, 2) - 0.5f, unit(id, 3) - 0.5f, unit(id, 4) - 0.5f) * (0.2f * base);
        }
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        if (inTheWay(p, r)) continue;
        int cx, cy, cz;
        cellOf(p, inv, cx, cy, cz);
        made[cellId(cx, cy, cz)].push_back(static_cast<uint32_t>(x_.size()));
        x_.push_back(p);
        v_.push_back(moving ? vel->read<Vec3>()[i] : Vec3());
        r_.push_back(r);
        w_.push_back(1.0f / (rho * (4.0f / 3.0f) * kPi * r * r * r));
        id_.push_back(id);
        if (coloured) {
            const Vec3 c = cd->read<Vec3>()[i];
            color_.insert(color_.end(), {byteOf(c.x), byteOf(c.y), byteOf(c.z)});
        } else if (!color_.empty()) {
            paint(1);
        }
        ++nextId_;
        ++added;
    }
}

// --- neighbours --------------------------------------------------------------------------

void GrainSolver::reorder() {
    // The grains in the order of the cells they are in, z, then y, then x:
    // neighbours near each other in memory too. The same order on any
    // number of threads -- a stable sort of the grains by number.
    const size_t n = x_.size();
    if (n < 2) return;
    float biggest = 0.0f;
    for (const float r : r_) biggest = std::max(biggest, r);
    const float inv = 1.0f / (2.0f * biggest);
    std::vector<int32_t> c(3 * n);
    for (size_t i = 0; i < n; ++i) cellOf(x_[i], inv, c[3 * i], c[3 * i + 1], c[3 * i + 2]);
    std::vector<uint32_t> by(n);
    for (size_t i = 0; i < n; ++i) by[i] = static_cast<uint32_t>(i);
    std::stable_sort(by.begin(), by.end(), [&](uint32_t a, uint32_t b) {
        const int32_t* p = c.data() + 3 * a;
        const int32_t* q = c.data() + 3 * b;
        if (p[2] != q[2]) return p[2] < q[2];
        if (p[1] != q[1]) return p[1] < q[1];
        return p[0] < q[0];
    });
    auto permute = [&](auto& list, size_t width) {
        auto old = list;
        for (size_t i = 0; i < n; ++i) {
            std::copy_n(old.begin() + static_cast<std::ptrdiff_t>(width * by[i]), width,
                        list.begin() + static_cast<std::ptrdiff_t>(width * i));
        }
    };
    permute(x_, 1);
    permute(v_, 1);
    permute(r_, 1);
    permute(w_, 1);
    permute(id_, 1);
    if (!color_.empty()) permute(color_, 3);
}

void GrainSolver::sortIntoCells(float cell) {
    const size_t n = x_.size();
    cell_ = std::max(cell, 1e-5f);
    const float inv = 1.0f / cell_;
    // A table some twice as big as there are grains, a power of two.
    const size_t table = std::bit_ceil(std::max<size_t>(2 * n, 64));
    const uint32_t mask = static_cast<uint32_t>(table - 1);
    key_.resize(n);
    cellOf_.resize(3 * n);
    pg::parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            int* c = cellOf_.data() + 3 * i;
            cellOf(x_[i], inv, c[0], c[1], c[2]);
            key_[i] = cellKey(c[0], c[1], c[2], mask);
        }
    });
    // Counted into the table, in order: the grains of each bucket by number.
    bucket_.assign(table + 1, 0);
    for (size_t i = 0; i < n; ++i) ++bucket_[key_[i] + 1];
    for (size_t k = 0; k < table; ++k) bucket_[k + 1] += bucket_[k];
    order_.resize(n);
    std::vector<uint32_t> at(bucket_.begin(), bucket_.end() - 1);
    for (size_t i = 0; i < n; ++i) order_[at[key_[i]]++] = static_cast<uint32_t>(i);
}

void GrainSolver::findNeighbours(float reach) {
    const size_t n = x_.size();
    const uint32_t mask = static_cast<uint32_t>(bucket_.size() - 2);
    nearStart_.assign(n + 1, 0);
    // Twice over: how many, then which -- each grain its own, in the order
    // of the buckets, the same on any number of threads.
    auto visit = [&](size_t i, auto&& take) {
        const int* c = cellOf_.data() + 3 * i;
        const float ri = r_[i];
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = c[0] + dx, y = c[1] + dy, z = c[2] + dz;
                    const uint32_t k = cellKey(x, y, z, mask);
                    for (uint32_t o = bucket_[k]; o < bucket_[k + 1]; ++o) {
                        const uint32_t j = order_[o];
                        // Of this cell -- not another whose key is the same.
                        const int* cj = cellOf_.data() + 3 * j;
                        if (j == i || cj[0] != x || cj[1] != y || cj[2] != z) continue;
                        const Vec3 d = x_[i] - x_[j];
                        const float within = ri + r_[j] + reach;
                        if (dot(d, d) < within * within) take(j);
                    }
                }
            }
        }
    };
    // Each block of grains its own lists, at once; then one after the
    // other, in the order of the blocks: the same on any number of threads.
    const std::vector<std::pair<size_t, size_t>> blocks = pg::chunkRanges(n, 1024);
    std::vector<std::vector<uint32_t>> found(blocks.size());
    pg::parallelFor(blocks.size(), 1, [&](size_t begin, size_t end) {
        for (size_t b = begin; b < end; ++b) {
            std::vector<uint32_t>& list = found[b];
            for (size_t i = blocks[b].first; i < blocks[b].second; ++i) {
                const size_t before = list.size();
                visit(i, [&](uint32_t j) { list.push_back(j); });
                nearStart_[i + 1] = static_cast<uint32_t>(list.size() - before);
            }
        }
    });
    for (size_t i = 0; i < n; ++i) nearStart_[i + 1] += nearStart_[i];
    near_.resize(nearStart_[n]);
    for (size_t b = 0; b < blocks.size(); ++b) {
        std::copy(found[b].begin(), found[b].end(), near_.begin() + nearStart_[blocks[b].first]);
    }
    pressed_.assign(near_.size(), 0.0f);
}

// --- a step ------------------------------------------------------------------------------

void GrainSolver::solveContacts(bool last) {
    const GrainSettings& s = scene_.solver;
    const size_t n = x_.size();
    const float mus = s.friction, muk = kKinetic * s.friction;
    const float cohesion = s.cohesion;
    const float g = length(s.gravity);
    const Vec3 gravityDir = g > 0.0f ? s.gravity * (1.0f / g) : Vec3();
    // Each grain from where every grain was before this pass: its own
    // corrections, averaged.
    pg::parallelFor(n, 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Vec3 pi = x_[i];
            const Vec3 movedI = pi - prev_[i];
            const float ri = r_[i], wi = w_[i];
            // Those it touches, averaged among themselves; those it is
            // pulled to, among all: a wet grain among many a little apart is
            // still pushed out of the few it is in.
            Vec3 sum, pull;
            int count = 0, pulls = 0;
            for (uint32_t k = nearStart_[i]; k < nearStart_[i + 1]; ++k) {
                const uint32_t j = near_[k];
                const Vec3 d = pi - x_[j];
                const float R = ri + r_[j];
                const float dd = dot(d, d);
                const float cohere = cohesion > 0.0f ? kCohesionReach * std::min(ri, r_[j]) : 0.0f;
                if (dd >= (R + cohere) * (R + cohere)) continue;
                const float dist = std::sqrt(dd);
                // On top of each other: apart along a way of their own.
                const Vec3 normal = dist > 1e-9f ? d * (1.0f / dist)
                                                 : (i < j ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(0.0f, -1.0f, 0.0f));
                // Who gives how much: as heavy as each is -- and the upper
                // one more, as if the one below were heavier (the masses
                // scaled with height of Macklin et al. 2014, §5.4, after
                // Guendelman's shock propagation): a pile carries its
                // weight down in a few passes, its grains do not sink into
                // each other.
                const float up = std::clamp(-dot(d, gravityDir) / R, -1.0f, 1.0f);
                const float share = wi / (wi + w_[j] * std::exp(-kShock * up));
                // Apart along the normal -- or, a little apart and wet,
                // pulled together.
                Vec3 c;
                const bool touches = dist < R;
                if (touches) {
                    const float into = R - dist;
                    c = normal * (into * share);
                    // How hard they have been pressed together this substep,
                    // all passes so far: what friction holds against.
                    pressed_[k] += into;
                } else {
                    c = normal * (-cohesion * (dist - R) * share);
                }
                // Friction: what slid along the contact since the substep
                // began, held back -- as hard as they are pressed, and as
                // wet grains cling.
                const float pressed = pressed_[k] + cohesion * cohere;
                const Vec3 slid = movedI - (x_[j] - prev_[j]);
                const Vec3 along = slid - normal * dot(slid, normal);
                const float length2 = dot(along, along);
                if (length2 > 1e-20f && pressed > 0.0f) {
                    const float len = std::sqrt(length2);
                    const float keep = len < mus * pressed ? 1.0f : std::min(muk * pressed / len, 1.0f);
                    c -= along * (keep * share);
                }
                if (touches) {
                    sum += c;
                    ++count;
                } else {
                    pull += c;
                    ++pulls;
                }
            }
            dx_[i] = (count > 0 ? sum * (1.0f / static_cast<float>(count)) : Vec3()) +
                     (pulls > 0 ? pull * (1.0f / static_cast<float>(count + pulls)) : Vec3());
            if (last) touching_[i] = count + pulls > 0 ? 1 : 0;
        }
    });
    pg::parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) x_[i] += dx_[i];
    });
}

void GrainSolver::collide(float h) {
    const GrainSettings& s = scene_.solver;
    const float mus = s.friction, muk = kKinetic * s.friction;
    const size_t n = x_.size();
    // Out of what it went into, and held back along it by friction: no more
    // than it went in, against how that moves.
    auto rub = [&](size_t i, const Vec3& normal, float depth, const Vec3& surfaceVelocity) {
        const Vec3 moved = x_[i] - prev_[i] - surfaceVelocity * h;
        const Vec3 along = moved - normal * dot(moved, normal);
        const float slide = length(along);
        if (slide < 1e-12f) return;
        x_[i] = x_[i] - along * (slide < mus * depth ? 1.0f : std::min(1.0f, muk * depth / slide));
    };
    pg::parallelFor(n, 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            touched_[i] = -1;
            carried_[i] = 0.0f;
            const float r = r_[i];
            // What holds it from the other side -- the floor, an object that
            // does not give, another piece -- and which way it pushes.
            bool held = false;
            Vec3 heldBy, pieceBy;
            if (s.floor && x_[i].y < r) {
                const float depth = r - x_[i].y;
                x_[i].y = r;
                rub(i, Vec3(0.0f, 1.0f, 0.0f), depth, Vec3());
                held = true;
                heldBy = Vec3(0.0f, 1.0f, 0.0f);
                touching_[i] = 1;
            }
            for (size_t c = 0; c < shapes_.size(); ++c) {
                // Far outside the ball round it: not near it.
                const Vec3 off = x_[i] - shapes_[c].center();
                const float reach = 0.5f * length(scene_.colliders[c].size) + r;
                if (dot(off, off) > reach * reach) continue;
                const float d = shapes_[c].distance(x_[i]);
                if (d >= r) continue;
                Vec3 normal = shapes_[c].normal(x_[i]);
                float push = r - d;
                if (s.floor && normal.y < 0.0f && x_[i].y + normal.y * push < r) {
                    // Pressed into the floor by what lies on it: squeezed out
                    // from under it, sideways -- away from its middle when it
                    // presses straight down -- no faster than kSpringOff.
                    Vec3 side(normal.x, 0.0f, normal.z);
                    if (dot(side, side) < 1e-6f) side = Vec3(off.x, 0.0f, off.z);
                    if (dot(side, side) < 1e-12f) side = Vec3(1.0f, 0.0f, 0.0f);
                    normal = normalize(side);
                    push = std::min(push, kSpringOff * h);
                }
                const Vec3 was = x_[i];
                x_[i] += normal * push;
                const Vec3 moving = drift_.empty() ? Vec3() : kick_[c] + cross(twist_[c], x_[i] - shapes_[c].center());
                const Vec3 surface = scene_.colliders[c].velocityAt(x_[i]) + moving;
                carried_[i] = std::max(carried_[i], length(surface));
                rub(i, normal, push, surface);
                touching_[i] = 1;
                if (scene_.colliders[c].mass > 0.0f && (touched_[i] < 0 || touched_[i] == static_cast<int32_t>(c))) {
                    pushed_[i] = (touched_[i] == static_cast<int32_t>(c) ? pushed_[i] : Vec3()) + (x_[i] - was);
                    touched_[i] = static_cast<int32_t>(c);
                    pieceBy = normal;
                } else {
                    // Not a piece -- or a second one: they meet each other
                    // in the RBD Solver.
                    held = true;
                    heldBy = normal;
                }
            }
            // Nothing goes through the floor.
            if (s.floor && x_[i].y < r) x_[i].y = r;
            // Caught between a piece and what holds it from the other side --
            // under a piece on the ground -- what holds it holds the piece,
            // not the grain. Beside the piece, on the ground, it pushes it.
            if (held && touched_[i] >= 0 && dot(heldBy, pieceBy) < -0.5f) touched_[i] = -1;
        }
    });
    if (drift_.empty()) return;
    // The pieces that give: what the grains they pushed took of their
    // momentum -- in order, grain by grain -- slows them and turns them.
    std::vector<Vec3> took(shapes_.size()), turned(shapes_.size());
    for (size_t i = 0; i < n; ++i) {
        if (touched_[i] < 0) continue;
        const size_t c = static_cast<size_t>(touched_[i]);
        const Vec3 given = pushed_[i] * (1.0f / (w_[i] * h));
        took[c] += given;
        turned[c] += cross(x_[i] - shapes_[c].center(), given);
    }
    for (size_t c = 0; c < shapes_.size(); ++c) {
        const Collider& o = scene_.colliders[c];
        if (o.mass <= 0.0f || (took[c] == Vec3() && turned[c] == Vec3())) continue;
        const float inertia = std::max(o.mass * dot(o.size, o.size) / 18.0f, 1e-6f);
        kick_[c] = kick_[c] - took[c] * (1.0f / o.mass);
        twist_[c] = twist_[c] - turned[c] * (1.0f / inertia);
    }
}

void GrainSolver::step() {
    const GrainSettings& s = scene_.solver;
    emit();
    const size_t colliders = scene_.colliders.size();
    const bool gives =
        std::any_of(scene_.colliders.begin(), scene_.colliders.end(), [](const Collider& c) { return c.mass > 0.0f; });
    drift_.assign(gives ? colliders : 0, Vec3());
    kick_.assign(drift_.size(), Vec3());
    twist_.assign(drift_.size(), Vec3());
    reactions_.clear();
    size_t n = x_.size();
    if (n == 0) {
        ++frame_;
        time_ = static_cast<float>(frame_) * s.timeStep;
        contacts_ = 0;
        return;
    }
    reorder();
    const int steps = s.substeps;
    const float h = s.timeStep / static_cast<float>(steps);
    // The air the grains are in, once a step: the wind of the forces, the
    // gas's flow.
    const bool blown = s.airDrag > 0.0f &&
                       (air_ || std::any_of(scene_.forces.begin(), scene_.forces.end(),
                                            [](const Force& f) { return f.kind == ForceKind::Wind; }));
    wind_.assign(blown ? n : 0, Vec3());
    if (blown) {
        pg::parallelFor(n, 1024, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                Vec3 flow = air_ ? air_(x_[i]) : Vec3();
                for (const Force& f : scene_.forces) {
                    if (f.kind == ForceKind::Wind) flow += detail::windAt(f, time_, f.seed * 7919u + 17u, x_[i]);
                }
                wind_[i] = flow;
            }
        });
    }
    float biggest = 0.0f;
    for (const float r : r_) biggest = std::max(biggest, r);
    const float cohere = s.cohesion > 0.0f ? kCohesionReach * biggest : 0.0f;
    const float skin = 0.5f * biggest;
    const float reach = cohere + skin;
    bool rebuild = true;
    const float fade = 1.0f / (1.0f + s.damping * h);
    const float drag = blown ? 1.0f - std::exp(-s.airDrag * h) : 0.0f;
    prev_.resize(n);
    dx_.resize(n);
    touching_.assign(n, 0);
    carried_.assign(n, 0.0f);
    touched_.assign(n, -1);
    pushed_.assign(n, Vec3());
    // Grains just come in -- the grit of an RBD Solver, thrown out in a
    // bunch -- may lie in each other: they are put apart first, where they
    // are, the way they go kept (pre-stabilisation, Macklin et al. 2014,
    // §4.4) -- pushed out of each other, not flung.
    if (fresh_) {
        prev_ = x_;
        sortIntoCells(2.0f * biggest + reach);
        findNeighbours(reach);
        for (int it = 0; it < s.iterations; ++it) solveContacts(false);
        fresh_ = false;
    }
    for (int k = 0; k < steps; ++k) {
        const float t = static_cast<float>(k + 1) / static_cast<float>(steps);
        pg::parallelFor(n, 4096, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                prev_[i] = x_[i];
                Vec3 v = v_[i] + s.gravity * h;
                if (blown) v += (wind_[i] - v) * drag;
                const float speed2 = dot(v, v);
                if (speed2 > kMaxSpeed * kMaxSpeed) v = v * (kMaxSpeed / std::sqrt(speed2));
                v_[i] = v;
                x_[i] += v * h;
            }
        });
        // Who is near whom, where they are about to be: found again only
        // when a grain has moved half the skin since -- two coming at each
        // other have then closed it -- not every substep (Verlet's lists).
        float moved = 0.0f;
        if (!rebuild) {
            for (size_t i = 0; i < n; ++i) {
                const Vec3 d = x_[i] - builtAt_[i];
                moved = std::max(moved, dot(d, d));
            }
        }
        if (rebuild || moved > 0.25f * skin * skin) {
            sortIntoCells(2.0f * biggest + reach);
            findNeighbours(reach);
            builtAt_ = x_;
            rebuild = false;
        }
        std::fill(pressed_.begin(), pressed_.end(), 0.0f);
        for (int it = 0; it < s.iterations; ++it) solveContacts(it + 1 == s.iterations);
        contacts_ = near_.size() / 2;
        // What moves -- the pieces, animated objects -- where it is at this
        // substep, as fast as it goes.
        for (size_t c = 0; c < shapes_.size(); ++c) {
            const Collider& o = scene_.colliders[c];
            if (!drift_.empty()) drift_[c] += kick_[c] * h;
            if (!o.moves() && (drift_.empty() || drift_[c] == Vec3())) continue;
            const Vec3 back = o.velocity * (s.timeStep * (1.0f - t));
            shapes_[c] = ShapeInstance(o.shape, o.center - back + (drift_.empty() ? Vec3() : drift_[c]), o.rotation,
                                       o.size, o.mesh);
        }
        collide(h);
        const float still = s.restSpeed * h;
        pg::parallelFor(n, 4096, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                Vec3 moved = x_[i] - prev_[i];
                // In contact, and barely moving: it stays.
                if (touching_[i] && dot(moved, moved) < still * still) {
                    x_[i] = prev_[i];
                    moved = Vec3();
                }
                // Put out of where it went in, a grain is not flung: the
                // contacts stop it and at most send it off kSpringOff
                // faster than it came in -- or than the fastest surface it
                // touches goes, which carries it along. However deep it
                // went in, and however many substeps it is put out in a
                // row: its speed does not build up.
                Vec3 v = moved * (1.0f / h);
                const float most = std::max(length(v_[i]), carried_[i]) + kSpringOff;
                const float speed2 = dot(v, v);
                if (speed2 > most * most) v = v * (most / std::sqrt(speed2));
                v_[i] = v * fade;
            }
        });
    }
    // Gone below everything, without a floor.
    if (!s.floor) {
        size_t kept = 0;
        for (size_t i = 0; i < n; ++i) {
            if (x_[i].y < kGone) continue;
            x_[kept] = x_[i];
            v_[kept] = v_[i];
            r_[kept] = r_[i];
            w_[kept] = w_[i];
            id_[kept] = id_[i];
            if (!color_.empty()) std::copy_n(color_.begin() + static_cast<std::ptrdiff_t>(3 * i), 3,
                                             color_.begin() + static_cast<std::ptrdiff_t>(3 * kept));
            ++kept;
        }
        x_.resize(kept);
        v_.resize(kept);
        r_.resize(kept);
        w_.resize(kept);
        id_.resize(kept);
        if (!color_.empty()) color_.resize(3 * kept);
        n = kept;
    }
    ++frame_;
    time_ = static_cast<float>(frame_) * s.timeStep;
    for (size_t c = 0; c < drift_.size(); ++c) {
        const int32_t piece = scene_.colliders[c].piece;
        if (piece < 0 || (drift_[c] == Vec3() && kick_[c] == Vec3() && twist_[c] == Vec3())) continue;
        reactions_.push_back({static_cast<uint32_t>(piece), drift_[c], kick_[c], twist_[c]});
    }
}

GrainFrame GrainSolver::capture() const {
    GrainFrame f;
    const size_t n = x_.size();
    f.positions = x_;
    f.velocities.reserve(3 * n);
    for (const Vec3& v : v_) {
        for (int a = 0; a < 3; ++a) f.velocities.push_back(halfFromFloat(v[a]));
    }
    f.radii.reserve(n);
    for (const float r : r_) f.radii.push_back(halfFromFloat(r));
    f.ids = id_;
    f.colors = color_;
    return f;
}

// --- the state ---------------------------------------------------------------------------

void GrainSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.pod(nextId_);
    out.list(x_);
    out.list(v_);
    out.list(r_);
    out.list(w_);
    out.list(id_);
    out.list(color_);
    out.list(reactions_);
}

bool GrainSolver::loadState(StateReader& in) {
    int32_t frame = 0;
    float time = 0.0f;
    uint32_t nextId = 0;
    std::vector<Vec3> x, v;
    std::vector<float> r, w;
    std::vector<uint32_t> ids;
    std::vector<uint8_t> colors;
    std::vector<Reaction> reactions;
    if (!in.pod(frame) || !in.pod(time) || !in.pod(nextId) || !in.list(x) || !in.list(v) || !in.list(r) ||
        !in.list(w) || !in.list(ids) || !in.list(colors) || !in.list(reactions)) {
        return false;
    }
    const size_t n = x.size();
    if (frame < 0 || v.size() != n || r.size() != n || w.size() != n || ids.size() != n ||
        (!colors.empty() && colors.size() != 3 * n)) {
        return false;
    }
    for (const uint32_t id : ids) {
        if (id >= nextId) return false;
    }
    frame_ = frame;
    time_ = time;
    nextId_ = nextId;
    fresh_ = false;
    x_ = std::move(x);
    v_ = std::move(v);
    r_ = std::move(r);
    w_ = std::move(w);
    id_ = std::move(ids);
    color_ = std::move(colors);
    reactions_ = std::move(reactions);
    return true;
}

}  // namespace pg::sim
