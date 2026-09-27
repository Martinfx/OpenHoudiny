#include "pg/sim/Pyro.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg::sim {

using detail::faceOffset;
using detail::forEachCell;
using detail::forEachIn;
using detail::noise3;

// --- set-up ----------------------------------------------------------------------

PyroSolver::PyroSolver(const Scene& scene) : scene_(scene.sanitized()) { reset(); }

void PyroSolver::setScene(const Scene& scene) {
    const Scene safe = scene.sanitized();
    const Domain d = safe.solver.domain();
    const bool resize = d.cells[0] != domain_.cells[0] || d.cells[1] != domain_.cells[1] ||
                        d.cells[2] != domain_.cells[2] || d.voxel != domain_.voxel;
    const bool walls = safe.colliders != scene_.colliders || safe.solver.closedFloor != scene_.solver.closedFloor;
    scene_ = safe;
    if (resize) {
        reset();
        return;
    }
    noise_.resize(scene_.forces.size());
    if (walls) updateSolids();
}

void PyroSolver::reset() {
    domain_ = scene_.solver.domain();
    nx_ = domain_.cells[0];
    ny_ = domain_.cells[1];
    nz_ = domain_.cells[2];
    for (int a = 0; a < 3; ++a) {
        const int fx = nx_ + (a == 0), fy = ny_ + (a == 1), fz = nz_ + (a == 2);
        vel_[a] = Grid(fx, fy, fz);
        velNext_[a] = Grid(fx, fy, fz);
    }
    for (Grid* g : {&density_, &temperature_, &fuel_, &flame_, &solid_, &back_[0], &back_[1], &back_[2], &forward_[0],
                    &forward_[1], &forward_[2], &predicted_, &lo_, &hi_, &corrected_, &expansion_, &pressure_,
                    &divergence_, &centre_[0], &centre_[1], &centre_[2], &curl_[0], &curl_[1], &curl_[2],
                    &curlLength_}) {
        *g = Grid(nx_, ny_, nz_);
    }
    noise_.assign(scene_.forces.size(), {});
    frame_ = 0;
    time_ = 0.0f;
    updateSolids();
}

Vec3 PyroSolver::worldAt(float x, float y, float z) const {
    const Vec3 o = domain_.origin();
    const float h = domain_.voxel;
    return {o.x + x * h, o.y + y * h, o.z + z * h};
}

void PyroSolver::updateSolids() {
    solid_.fill(0.0f);
    anySolid_ = false;
    if (!scene_.colliders.empty()) {
        // A cell is solid when its centre is inside a collider; the box round
        // each collider skips the cells far from it.
        struct Solid {
            ShapeInstance shape;
            Vec3 lo, hi;
        };
        std::vector<Solid> solids;
        for (const Collider& c : scene_.colliders) {
            Solid s{c.instance(), {}, {}};
            s.shape.bounds(s.lo, s.hi);
            solids.push_back(s);
        }
        // solid_ holds 1 + the collider's index: which one it is, for its velocity.
        forEachCell(solid_, [&](int i, int j, int k) {
            const Vec3 p = worldAt(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f,
                                   static_cast<float>(k) + 0.5f);
            for (size_t c = 0; c < solids.size(); ++c) {
                const Solid& s = solids[c];
                if (p.x < s.lo.x || p.y < s.lo.y || p.z < s.lo.z || p.x > s.hi.x || p.y > s.hi.y || p.z > s.hi.z) {
                    continue;
                }
                if (s.shape.contains(p)) {
                    solid_.at(i, j, k) = 1.0f + static_cast<float>(c);
                    return;
                }
            }
        });
        const std::vector<float>& s = solid_.values();
        anySolid_ = std::any_of(s.begin(), s.end(), [](float v) { return v > 0.5f; });
    }
    // The solid cells and the faces they block, listed once: the walls are
    // enforced several times a step, by walking these short lists.
    solidCells_.clear();
    for (int a = 0; a < 3; ++a) blocked_[a].clear();
    if (anySolid_) {
        for (size_t c = 0; c < solid_.size(); ++c) {
            if (solid_.data()[c] > 0.5f) solidCells_.push_back(c);
        }
        for (int a = 0; a < 3; ++a) {
            const Grid& v = vel_[a];
            for (int k = 0; k < v.nz(); ++k) {
                for (int j = 0; j < v.ny(); ++j) {
                    for (int i = 0; i < v.nx(); ++i) {
                        if (faceBlocked(a, i, j, k)) blocked_[a].push_back(v.index(i, j, k));
                    }
                }
            }
        }
    }
    // What a moving solid gives the faces it blocks: its velocity there,
    // along the face's axis -- the gas next to it is pushed and dragged.
    const bool moving = anySolid_ && std::any_of(scene_.colliders.begin(), scene_.colliders.end(),
                                                 [](const Collider& c) { return c.moves(); });
    for (int a = 0; a < 3; ++a) {
        blockedVel_[a].assign(blocked_[a].size(), 0.0f);
        if (!moving) continue;
        const Grid& v = vel_[a];
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        for (size_t b = 0; b < blocked_[a].size(); ++b) {
            const size_t f = blocked_[a][b];
            const int i = static_cast<int>(f % static_cast<size_t>(v.nx()));
            const int j = static_cast<int>((f / static_cast<size_t>(v.nx())) % static_cast<size_t>(v.ny()));
            const int k = static_cast<int>(f / (static_cast<size_t>(v.nx()) * static_cast<size_t>(v.ny())));
            const int at = a == 0 ? i : a == 1 ? j : k;
            // The solid cell beside the face: ahead of it, or behind.
            float owner = at < n ? solid_.at(std::min(i, nx_ - 1), std::min(j, ny_ - 1), std::min(k, nz_ - 1)) : 0.0f;
            if (owner < 0.5f && at > 0) owner = solid_.at(i - (a == 0), j - (a == 1), k - (a == 2));
            if (owner < 0.5f) continue;  // the floor
            const Collider& c = scene_.colliders[static_cast<size_t>(owner - 0.5f)];
            const Vec3 p = worldAt(static_cast<float>(i) + (a == 0 ? 0.0f : 0.5f), static_cast<float>(j) + (a == 1 ? 0.0f : 0.5f),
                                   static_cast<float>(k) + (a == 2 ? 0.0f : 0.5f));
            blockedVel_[a][b] = c.velocityAt(p)[a];
        }
    }
    PoissonBoundary boundary;
    boundary.closed[2] = scene_.solver.closedFloor;
    boundary.solid = anySolid_ ? &solid_ : nullptr;
    poisson_.setBoundary(boundary);
    // No gas inside a solid.
    for (const size_t c : solidCells_) {
        density_.data()[c] = temperature_.data()[c] = fuel_.data()[c] = flame_.data()[c] = 0.0f;
    }
    enforceWalls();
}

bool PyroSolver::faceBlocked(int axis, int i, int j, int k) const {
    const int f = axis == 0 ? i : axis == 1 ? j : k;  // face f: between cells f-1 and f
    const int n = axis == 0 ? nx_ : axis == 1 ? ny_ : nz_;
    if (axis == 1 && f == 0 && scene_.solver.closedFloor) return true;
    if (!anySolid_) return false;
    if (f > 0 && solid_.at(i - (axis == 0), j - (axis == 1), k - (axis == 2)) > 0.5f) return true;
    return f < n && solid_.at(i, j, k) > 0.5f;
}

void PyroSolver::enforceWalls() {
    if (scene_.solver.closedFloor) {
        forEachIn(0, nx_, 0, 1, 0, nz_, [&](int i, int, int k) { vel_[1].at(i, 0, k) = 0.0f; });
    }
    for (int a = 0; a < 3; ++a) {
        float* v = vel_[a].data();
        const std::vector<size_t>& faces = blocked_[a];
        const float* moving = blockedVel_[a].data();
        for (size_t b = 0; b < faces.size(); ++b) v[faces[b]] = moving[b];
    }
}

void PyroSolver::velocityAt(float x, float y, float z, float out[3]) const {
    out[0] = vel_[0].sample(x + 0.5f, y, z);
    out[1] = vel_[1].sample(x, y + 0.5f, z);
    out[2] = vel_[2].sample(x, y, z + 0.5f);
}

void PyroSolver::step() {
    const int n = scene_.solver.substeps;
    const float dt = scene_.solver.timeStep / static_cast<float>(n);
    for (int s = 0; s < n; ++s) {
        emit(dt);
        advect(dt);
        combust(dt);
        addForces(dt);
        project();
        dissipate(dt);
        time_ += dt;
    }
    ++frame_;
}

// --- emit ------------------------------------------------------------------------

void PyroSolver::emit(float dt) {
    const float h = domain_.voxel;
    const Vec3 origin = domain_.origin();
    const int n[3] = {nx_, ny_, nz_};
    for (const Emitter& e : scene_.emitters) {
        if (!e.activeAt(time_)) continue;
        // How much of the source is at a world point: 1 inside, easing to 0
        // at its surface.
        const ShapeInstance shape = e.shapeAt(time_);
        auto weight = [&](const Vec3& p) { return shape.falloff(p); };
        const uint32_t seed = e.seed * 7919u + scene_.solver.seed;
        // Its output flickers with noise that rises with the gas.
        const float rise = time_ * std::max(length(e.velocity), 0.2f);
        auto flicker = [&](const Vec3& p) {
            if (e.flicker <= 0.0f) return 1.0f;
            const float s = 1.0f / e.flickerSize;
            const float noise = noise3(p.x * s, (p.y - rise) * s, p.z * s, seed);
            return std::max(0.0f, 1.0f + e.flicker * 1.5f * (2.0f * noise - 1.0f));
        };

        // The cells the source can reach, and one more face along each axis.
        Vec3 reachLo, reachHi;
        shape.bounds(reachLo, reachHi);
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::clamp(static_cast<int>(std::floor((reachLo[a] - origin[a]) / h)) - 1, 0, n[a]);
            hi[a] = std::clamp(static_cast<int>(std::ceil((reachHi[a] - origin[a]) / h)) + 1, 0, n[a]);
        }
        forEachIn(lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], [&](int i, int j, int k) {
            if (anySolid_ && solid_.at(i, j, k) > 0.5f) return;
            const Vec3 p = worldAt(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f,
                                   static_cast<float>(k) + 0.5f);
            const float w = weight(p);
            if (w <= 0.0f) return;
            const float amount = dt * w * flicker(p);
            fuel_.at(i, j, k) += e.fuel * amount;
            density_.at(i, j, k) += e.smoke * amount;
            temperature_.at(i, j, k) += e.heat * amount;
        });

        // Push the gas the source's way -- along its own axes -- and across it
        // a little, so a plume does not stay a column. A moving source drags
        // the gas along.
        const Vec3 push = shape.turn().apply(e.velocity) + e.motionVelocityAt(time_) + e.moving;
        const float speed = length(push);
        if (speed <= 0.0f) continue;
        const Vec3 along = push * (1.0f / speed);
        for (int a = 0; a < 3; ++a) {
            Grid& vel = vel_[a];
            const float across = 1.0f - std::fabs(along[a]);
            forEachIn(lo[0], std::min(hi[0] + (a == 0), vel.nx()), lo[1], std::min(hi[1] + (a == 1), vel.ny()), lo[2],
                      std::min(hi[2] + (a == 2), vel.nz()), [&](int i, int j, int k) {
                          const Vec3 p = worldAt(static_cast<float>(i) + faceOffset(a, 0),
                                                 static_cast<float>(j) + faceOffset(a, 1),
                                                 static_cast<float>(k) + faceOffset(a, 2));
                          const float w = weight(p);
                          if (w <= 0.0f) return;
                          const float m = flicker(p);
                          float& v = vel.at(i, j, k);
                          const float target = push[a] * w * (0.6f + 0.4f * m);
                          if (push[a] > 0.0f) v = std::max(v, target);
                          else if (push[a] < 0.0f) v = std::min(v, target);
                          if (e.flicker > 0.0f && across > 0.0f) {
                              const float s = 6.0f / (e.flickerSize * 14.0f);  // broader than the flicker
                              const float wobble = noise3(p.x * s + 11.0f * static_cast<float>(a),
                                                          (p.y - rise) * s, p.z * s, seed + 1u + static_cast<uint32_t>(a));
                              v += speed * 0.3f * e.flicker * across * w * (wobble - 0.5f);
                          }
                      });
        }
    }
    enforceWalls();
}

// --- advect ----------------------------------------------------------------------

void PyroSolver::advect(float dt) {
    const float cells = dt / domain_.voxel;  // velocity x dt, in cells
    // Where the gas of each cell was a step ago, and where it will be (RK2).
    forEachCell(density_, [&](int i, int j, int k) {
        const size_t c = density_.index(i, j, k);
        const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j) + 0.5f,
                    z = static_cast<float>(k) + 0.5f;
        const float u = 0.5f * (vel_[0].at(i, j, k) + vel_[0].at(i + 1, j, k));
        const float v = 0.5f * (vel_[1].at(i, j, k) + vel_[1].at(i, j + 1, k));
        const float w = 0.5f * (vel_[2].at(i, j, k) + vel_[2].at(i, j, k + 1));
        for (const float sign : {-1.0f, 1.0f}) {
            float mid[3];
            velocityAt(x + sign * 0.5f * cells * u, y + sign * 0.5f * cells * v, z + sign * 0.5f * cells * w, mid);
            Grid* out = sign < 0.0f ? back_ : forward_;
            out[0].data()[c] = x + sign * cells * mid[0];
            out[1].data()[c] = y + sign * cells * mid[1];
            out[2].data()[c] = z + sign * cells * mid[2];
        }
    });

    // The velocity carries itself: plain semi-Lagrangian, stable at any step.
    for (int a = 0; a < 3; ++a) advectVelocity(a, cells);
    for (int a = 0; a < 3; ++a) std::swap(vel_[a], velNext_[a]);

    advectScalar(density_);
    advectScalar(temperature_);
    advectScalar(fuel_);
    advectScalar(flame_);
    for (const size_t c : solidCells_) {
        density_.data()[c] = temperature_.data()[c] = fuel_.data()[c] = flame_.data()[c] = 0.0f;
    }
    enforceWalls();
}

void PyroSolver::faceVelocity(int axis, int i, int j, int k, float out[3]) const {
    out[axis] = vel_[axis].at(i, j, k);
    const int n = axis == 0 ? nx_ : axis == 1 ? ny_ : nz_;
    // The cells on either side of the face, kept inside the grid.
    int below[3] = {i, j, k}, above[3] = {i, j, k};
    below[axis] = std::max(below[axis] - 1, 0);
    above[axis] = std::min(above[axis], n - 1);
    for (int b = 0; b < 3; ++b) {
        if (b == axis) continue;
        // Each cell's two faces along b.
        const Grid& v = vel_[b];
        const int e[3] = {b == 0, b == 1, b == 2};
        out[b] = 0.25f * (v.at(below[0], below[1], below[2]) + v.at(below[0] + e[0], below[1] + e[1], below[2] + e[2]) +
                          v.at(above[0], above[1], above[2]) + v.at(above[0] + e[0], above[1] + e[1], above[2] + e[2]));
    }
}

void PyroSolver::advectVelocity(int axis, float cells) {
    const float ox = faceOffset(axis, 0), oy = faceOffset(axis, 1), oz = faceOffset(axis, 2);
    const float nx = static_cast<float>(nx_), ny = static_cast<float>(ny_), nz = static_cast<float>(nz_);
    const bool floor = scene_.solver.closedFloor;
    Grid& out = velNext_[axis];
    forEachCell(out, [&](int i, int j, int k) {
        const float x = static_cast<float>(i) + ox, y = static_cast<float>(j) + oy, z = static_cast<float>(k) + oz;
        float v0[3], v1[3];
        faceVelocity(axis, i, j, k, v0);
        velocityAt(x - 0.5f * cells * v0[0], y - 0.5f * cells * v0[1], z - 0.5f * cells * v0[2], v1);
        const float px = x - cells * v1[0], py = y - cells * v1[1], pz = z - cells * v1[2];
        // Gas that comes in through an open side comes from the still air
        // outside. (Carrying in the velocity at the side instead, a flow that
        // sucks gas in -- the low pressure in a vortex -- would feed on itself.)
        if (px < 0.0f || px > nx || pz < 0.0f || pz > nz || py > ny || (py < 0.0f && !floor)) {
            out.at(i, j, k) = 0.0f;
            return;
        }
        // This component where the gas came from. Grid::sample puts sample
        // (0, 0, 0) at 0.5: shift by what the faces lack of it.
        out.at(i, j, k) = vel_[axis].sample(px + (0.5f - ox), py + (0.5f - oy), pz + (0.5f - oz));
    });
}

void PyroSolver::advectScalar(Grid& field) {
    // MacCormack: advect, advect the result back, correct by half the error
    // that round trip shows, clamp to what the first step interpolated from.
    forEachCell(field, [&](int i, int j, int k) {
        const size_t c = field.index(i, j, k);
        float lo, hi;
        predicted_.data()[c] = field.sample(back_[0].data()[c], back_[1].data()[c], back_[2].data()[c], true, lo, hi);
        lo_.data()[c] = lo;
        hi_.data()[c] = hi;
    });
    const float nx = static_cast<float>(nx_), ny = static_cast<float>(ny_), nz = static_cast<float>(nz_);
    forEachCell(field, [&](int i, int j, int k) {
        const size_t c = field.index(i, j, k);
        const float fx = forward_[0].data()[c], fy = forward_[1].data()[c], fz = forward_[2].data()[c];
        // Where the gas leaves the domain the round trip would come back
        // empty and the correction add what is not there: smoke would pile up
        // at the open boundaries. There, plain semi-Lagrangian.
        if (fx < 0.0f || fy < 0.0f || fz < 0.0f || fx > nx || fy > ny || fz > nz) {
            corrected_.data()[c] = predicted_.data()[c];
            return;
        }
        const float roundTrip = predicted_.sample(fx, fy, fz);
        const float v = predicted_.data()[c] + 0.5f * (field.data()[c] - roundTrip);
        corrected_.data()[c] = std::clamp(v, lo_.data()[c], hi_.data()[c]);
    });
    std::swap(field, corrected_);
}

// --- combust -----------------------------------------------------------------------

void PyroSolver::combust(float dt) {
    const SolverSettings& s = scene_.solver;
    const float share = 1.0f - std::exp(-s.burnRate * dt);
    forEachCell(fuel_, [&](int i, int j, int k) {
        const size_t c = fuel_.index(i, j, k);
        const float burnt = fuel_.data()[c] * share;
        fuel_.data()[c] -= burnt;
        temperature_.data()[c] += burnt * s.heatRelease;
        density_.data()[c] += burnt * s.sootRelease;
        flame_.data()[c] += burnt;
        expansion_.data()[c] = burnt * s.expansion / dt;
    });
}

// --- forces --------------------------------------------------------------------------

void PyroSolver::addForces(float dt) {
    const SolverSettings& s = scene_.solver;
    // Buoyancy on the vertical faces, from the cells below and above them.
    forEachCell(vel_[1], [&](int i, int j, int k) {
        float heat = 0.0f, smoke = 0.0f, n = 0.0f;
        if (j > 0) {
            heat += temperature_.at(i, j - 1, k);
            smoke += density_.at(i, j - 1, k);
            n += 1.0f;
        }
        if (j < ny_) {
            heat += temperature_.at(i, j, k);
            smoke += density_.at(i, j, k);
            n += 1.0f;
        }
        vel_[1].at(i, j, k) += dt * (s.buoyancy * heat - s.weight * smoke) / n;
    });
    if (s.vorticity > 0.0f) addVorticity(dt);
    for (size_t f = 0; f < scene_.forces.size(); ++f) addForce(scene_.forces[f], f, dt);
    enforceWalls();
}

void PyroSolver::addVorticity(float dt) {
    // Vorticity confinement: find the swirls, push along them. Worked out at
    // the cell centres, then spread to the faces.
    const float h = domain_.voxel;
    forEachCell(density_, [&](int i, int j, int k) {
        centre_[0].at(i, j, k) = 0.5f * (vel_[0].at(i, j, k) + vel_[0].at(i + 1, j, k));
        centre_[1].at(i, j, k) = 0.5f * (vel_[1].at(i, j, k) + vel_[1].at(i, j + 1, k));
        centre_[2].at(i, j, k) = 0.5f * (vel_[2].at(i, j, k) + vel_[2].at(i, j, k + 1));
    });
    // Neighbours along each axis, kept inside the grid; the differences are
    // over the distance between them.
    struct Around {
        size_t minus[3], plus[3];
        float scale[3];
    };
    auto around = [&](int i, int j, int k) {
        const Grid& g = density_;
        const int im = std::max(i - 1, 0), ip = std::min(i + 1, nx_ - 1);
        const int jm = std::max(j - 1, 0), jp = std::min(j + 1, ny_ - 1);
        const int km = std::max(k - 1, 0), kp = std::min(k + 1, nz_ - 1);
        return Around{{g.index(im, j, k), g.index(i, jm, k), g.index(i, j, km)},
                      {g.index(ip, j, k), g.index(i, jp, k), g.index(i, j, kp)},
                      {1.0f / (static_cast<float>(ip - im) * h), 1.0f / (static_cast<float>(jp - jm) * h),
                       1.0f / (static_cast<float>(kp - km) * h)}};
    };
    forEachCell(density_, [&](int i, int j, int k) {
        const Around n = around(i, j, k);
        // d(component a)/d(axis b)
        auto d = [&](int a, int b) { return (centre_[a].data()[n.plus[b]] - centre_[a].data()[n.minus[b]]) * n.scale[b]; };
        const float wx = d(2, 1) - d(1, 2);
        const float wy = d(0, 2) - d(2, 0);
        const float wz = d(1, 0) - d(0, 1);
        const size_t c = density_.index(i, j, k);
        curl_[0].data()[c] = wx;
        curl_[1].data()[c] = wy;
        curl_[2].data()[c] = wz;
        curlLength_.data()[c] = std::sqrt(wx * wx + wy * wy + wz * wz);
    });
    // The force, at the centres: centre_ is free again.
    const float strength = scene_.solver.vorticity * h;
    forEachCell(density_, [&](int i, int j, int k) {
        const Around n = around(i, j, k);
        const float* l = curlLength_.data();
        float g[3];
        for (int b = 0; b < 3; ++b) g[b] = (l[n.plus[b]] - l[n.minus[b]]) * n.scale[b];
        const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) + 1e-6f;
        const float gx = g[0] / len, gy = g[1] / len, gz = g[2] / len;
        const size_t c = density_.index(i, j, k);
        const float wx = curl_[0].data()[c], wy = curl_[1].data()[c], wz = curl_[2].data()[c];
        centre_[0].data()[c] = strength * (gy * wz - gz * wy);
        centre_[1].data()[c] = strength * (gz * wx - gx * wz);
        centre_[2].data()[c] = strength * (gx * wy - gy * wx);
    });
    for (int a = 0; a < 3; ++a) {
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        forEachCell(vel_[a], [&](int i, int j, int k) {
            const int f = a == 0 ? i : a == 1 ? j : k;  // face f is between cells f-1 and f
            float force = 0.0f, count = 0.0f;
            if (f > 0) {
                force += centre_[a].at(i - (a == 0), j - (a == 1), k - (a == 2));
                count += 1.0f;
            }
            if (f < n) {
                force += centre_[a].at(i, j, k);
                count += 1.0f;
            }
            vel_[a].at(i, j, k) += dt * force / count;
        });
    }
}

float PyroSolver::maskAt(Mask mask, int axis, int i, int j, int k) const {
    if (mask == Mask::Everywhere) return 1.0f;
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    const int n = axis == 0 ? nx_ : axis == 1 ? ny_ : nz_;
    auto amount = [&](int x, int y, int z) {
        return mask == Mask::Heat ? temperature_.at(x, y, z) + fuel_.at(x, y, z) : density_.at(x, y, z);
    };
    float m = 0.0f;
    if (f > 0) m += amount(i - (axis == 0), j - (axis == 1), k - (axis == 2));
    if (f < n) m += amount(i, j, k);
    return std::min(m, 1.0f);
}

void PyroSolver::addForce(const Force& force, size_t index, float dt) {
    const uint32_t seed = force.seed * 7919u + scene_.solver.seed * 31u;
    detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[index],
                     [&](int a, int i, int j, int k) { return maskAt(force.mask, a, i, j, k); });
}

// --- project -----------------------------------------------------------------------

float PyroSolver::divergence(int i, int j, int k) const {
    return (vel_[0].at(i + 1, j, k) - vel_[0].at(i, j, k) + vel_[1].at(i, j + 1, k) - vel_[1].at(i, j, k) +
            vel_[2].at(i, j, k + 1) - vel_[2].at(i, j, k)) /
           domain_.voxel;
}

void PyroSolver::project() {
    const float h = domain_.voxel;
    enforceWalls();  // what flows through walls is 0 before anything is measured
    forEachCell(divergence_, [&](int i, int j, int k) {
        divergence_.at(i, j, k) =
            anySolid_ && solid_.at(i, j, k) > 0.5f ? 0.0f : divergence(i, j, k) - expansion_.at(i, j, k);
    });
    // The pressure of the previous step is the first guess.
    poisson_.solve(pressure_, divergence_, h, scene_.solver.pressureCycles);
    // Subtract its gradient. The open sides hold p = 0 on the face -- a ghost
    // cell outside holds minus the cell inside. What this does to the faces of
    // walls and solids does not count: they go back to 0 right after.
    for (int a = 0; a < 3; ++a) {
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        forEachCell(vel_[a], [&](int i, int j, int k) {
            const int f = a == 0 ? i : a == 1 ? j : k;  // face f is between cells f-1 and f
            const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
            const float ahead = f < n ? pressure_.at(i, j, k) : -pressure_.at(bi, bj, bk);
            const float behind = f > 0 ? pressure_.at(bi, bj, bk) : -pressure_.at(i, j, k);
            vel_[a].at(i, j, k) -= (ahead - behind) / h;
        });
    }
    enforceWalls();
}

// --- dissipate ---------------------------------------------------------------------

void PyroSolver::dissipate(float dt) {
    const SolverSettings& s = scene_.solver;
    const float smoke = std::exp(-s.smokeDecay * dt), heat = std::exp(-s.cooling * dt);
    const float flame = s.flameLife > 0.0f ? std::exp(-dt / s.flameLife) : 0.0f;
    forEachCell(density_, [&](int i, int j, int k) {
        const size_t c = density_.index(i, j, k);
        // Where the burning gas swells -- by expansion x dt of its volume this
        // step -- what it carries spreads over the more room. Advection alone
        // keeps a value as it moves: right where the gas neither swells nor
        // shrinks, wrong here. Swollen fuel that stayed as rich would burn
        // and swell again, and an explosion would feed on itself until it
        // filled the domain.
        const float thinner = 1.0f / (1.0f + expansion_.data()[c] * dt);
        density_.data()[c] = std::max(0.0f, density_.data()[c]) * smoke * thinner;
        temperature_.data()[c] = std::max(0.0f, temperature_.data()[c]) * heat;
        fuel_.data()[c] = std::max(0.0f, fuel_.data()[c]) * thinner;
        flame_.data()[c] = std::max(0.0f, flame_.data()[c]) * flame * thinner;
    });
}

double PyroSolver::meanDivergence() const {
    double sum = 0.0;
    size_t cells = 0;
    for (int k = 0; k < nz_; ++k) {
        for (int j = 0; j < ny_; ++j) {
            for (int i = 0; i < nx_; ++i) {
                if (anySolid_ && solid_.at(i, j, k) > 0.5f) continue;
                sum += std::fabs(divergence(i, j, k) - expansion_.at(i, j, k));
                ++cells;
            }
        }
    }
    return cells ? sum / static_cast<double>(cells) : 0.0;
}

Grid lightTransmittance(const Grid& density, const float towardsLight[3], float extinctionPerCell, int divisor) {
    divisor = std::max(1, divisor);
    const int lx = std::max(1, density.nx() / divisor), ly = std::max(1, density.ny() / divisor),
              lz = std::max(1, density.nz() / divisor);
    Grid out(lx, ly, lz, 1.0f);
    const float len = std::sqrt(towardsLight[0] * towardsLight[0] + towardsLight[1] * towardsLight[1] +
                                towardsLight[2] * towardsLight[2]);
    if (len <= 0.0f) return out;
    const float d[3] = {towardsLight[0] / len, towardsLight[1] / len, towardsLight[2] / len};
    const float sx = static_cast<float>(density.nx()) / static_cast<float>(lx);
    const float sy = static_cast<float>(density.ny()) / static_cast<float>(ly);
    const float sz = static_cast<float>(density.nz()) / static_cast<float>(lz);
    const float step = static_cast<float>(divisor);  // in density cells
    const float nx = static_cast<float>(density.nx()), ny = static_cast<float>(density.ny()),
                nz = static_cast<float>(density.nz());
    forEachCell(out, [&](int i, int j, int k) {
        // Start half a step towards the light: a cell does not shade itself.
        float x = (static_cast<float>(i) + 0.5f) * sx + d[0] * 0.5f * step;
        float y = (static_cast<float>(j) + 0.5f) * sy + d[1] * 0.5f * step;
        float z = (static_cast<float>(k) + 0.5f) * sz + d[2] * 0.5f * step;
        float depth = 0.0f;
        while (x >= 0.0f && y >= 0.0f && z >= 0.0f && x <= nx && y <= ny && z <= nz && depth < 16.0f) {
            depth += density.sample(x, y, z) * extinctionPerCell * step;
            x += d[0] * step;
            y += d[1] * step;
            z += d[2] * step;
        }
        out.at(i, j, k) = std::exp(-depth);
    });
    return out;
}

}  // namespace pg::sim
