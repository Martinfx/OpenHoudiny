#include "pg/sim/Pyro.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg::sim {
namespace {

/// f(i, j, k) for every cell of an nx x ny x nz grid, in parallel. The rows
/// of cells are split into chunks of some 8k cells -- enough work that handing
/// a chunk to a thread costs little. Every cell is visited by exactly one
/// chunk and computed the same whichever it is, so the split changes how the
/// work spreads over threads, never a result.
size_t rowsPerChunk(int nx) { return std::max<size_t>(1, 8192 / static_cast<size_t>(std::max(1, nx))); }

template <class F>
void forEachCell(int nx, int ny, int nz, const F& f) {
    const size_t rows = static_cast<size_t>(ny) * static_cast<size_t>(nz);
    pg::parallelFor(rows, rowsPerChunk(nx), [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            const int j = static_cast<int>(r % static_cast<size_t>(ny));
            const int k = static_cast<int>(r / static_cast<size_t>(ny));
            for (int i = 0; i < nx; ++i) f(i, j, k);
        }
    });
}

template <class F>
void forEachCell(const Grid& g, const F& f) {
    forEachCell(g.nx(), g.ny(), g.nz(), f);
}

uint32_t hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u + 0x7F4A7C15u;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE35u;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float lattice(int x, int y, int z, uint32_t seed) {
    return static_cast<float>(hash(x, y, z, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

/// Smooth value noise in [0, 1]: what makes the source flicker.
float noise3(float x, float y, float z, uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float tx = fade(x - fx), ty = fade(y - fy), tz = fade(z - fz);
    auto l = [&](int a, int b, int c) { return lattice(ix + a, iy + b, iz + c, seed); };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float y0 = lerp(lerp(l(0, 0, 0), l(1, 0, 0), tx), lerp(l(0, 1, 0), l(1, 1, 0), tx), ty);
    const float y1 = lerp(lerp(l(0, 0, 1), l(1, 0, 1), tx), lerp(l(0, 1, 1), l(1, 1, 1), tx), ty);
    return lerp(y0, y1, tz);
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Position of sample (0, 0, 0) of velocity component `axis` along axis `a`,
/// in cell units: the faces sit on the cell borders along their own axis.
float faceOffset(int axis, int a) { return axis == a ? 0.0f : 0.5f; }

}  // namespace

// --- presets -------------------------------------------------------------------

PyroSettings PyroSettings::fire() {
    PyroSettings s;
    s.fuelRate = 14.0f;
    s.heatRate = 1.0f;
    s.sourceSpeed = 0.4f;
    s.sourceNoise = 0.7f;
    s.burnRate = 10.0f;
    s.heatRelease = 2.5f;
    s.sootRelease = 0.5f;
    s.expansion = 0.8f;
    s.flameLife = 0.1f;
    s.buoyancy = 0.9f;
    s.weight = 0.05f;
    s.vorticity = 0.9f;
    s.turbulence = 3.5f;
    s.turbulenceScale = 0.05f;
    s.cooling = 1.5f;
    s.smokeDecay = 0.15f;
    return s;
}

PyroSettings PyroSettings::smoke() {
    PyroSettings s;
    s.smokeRate = 5.0f;
    s.heatRate = 3.0f;
    s.sourceSpeed = 0.55f;
    s.sourceNoise = 0.15f;
    s.buoyancy = 1.0f;
    s.weight = 0.08f;
    s.vorticity = 0.9f;
    s.turbulence = 3.5f;
    s.turbulenceScale = 0.05f;
    s.cooling = 0.6f;
    s.smokeDecay = 0.03f;
    return s;
}

const std::vector<PyroParam>& pyroParams() {
    using S = PyroSettings;
    static const std::vector<PyroParam> params = {
        {"sourceRadius", "Radius", "Source", &S::sourceRadius, 0.02f, 0.4f, "Size of the source sphere."},
        {"sourceHeight", "Height", "Source", &S::sourceHeight, 0.02f, 1.0f, "Its centre above the floor."},
        {"sourceSpeed", "Speed", "Source", &S::sourceSpeed, 0.0f, 3.0f, "How fast it pushes the gas up."},
        {"fuelRate", "Fuel", "Source", &S::fuelRate, 0.0f, 40.0f, "Fuel added per second: fire."},
        {"smokeRate", "Smoke", "Source", &S::smokeRate, 0.0f, 20.0f, "Smoke added per second."},
        {"heatRate", "Heat", "Source", &S::heatRate, 0.0f, 10.0f, "Heat added per second."},
        {"sourceNoise", "Flicker", "Source", &S::sourceNoise, 0.0f, 1.0f, "0 steady, 1 strongly flickering."},
        {"burnRate", "Burn rate", "Combustion", &S::burnRate, 0.0f, 20.0f,
         "Share of the fuel that burns per second: short or tall flames."},
        {"heatRelease", "Heat", "Combustion", &S::heatRelease, 0.0f, 10.0f, "Heat per unit of fuel burnt."},
        {"sootRelease", "Soot", "Combustion", &S::sootRelease, 0.0f, 2.0f, "Smoke per unit of fuel burnt."},
        {"expansion", "Expansion", "Combustion", &S::expansion, 0.0f, 3.0f,
         "How much the burning gas expands: it pushes outwards."},
        {"flameLife", "Flame life", "Combustion", &S::flameLife, 0.02f, 1.0f,
         "Seconds a flame lasts: short licks or long tongues."},
        {"buoyancy", "Buoyancy", "Forces", &S::buoyancy, 0.0f, 5.0f, "Lift per unit of heat."},
        {"weight", "Weight", "Forces", &S::weight, 0.0f, 2.0f, "Sink per unit of smoke."},
        {"vorticity", "Swirl", "Forces", &S::vorticity, 0.0f, 2.0f,
         "Vorticity confinement: the small swirls a coarse grid loses."},
        {"turbulence", "Turbulence", "Forces", &S::turbulence, 0.0f, 10.0f,
         "Noisy force where there is heat or fuel: breaks the flow up."},
        {"turbulenceScale", "Turbulence size", "Forces", &S::turbulenceScale, 0.03f, 0.5f,
         "Size of the whirls the turbulence makes."},
        {"cooling", "Cooling", "Dissipation", &S::cooling, 0.0f, 5.0f, "How fast the heat fades, per second."},
        {"smokeDecay", "Smoke decay", "Dissipation", &S::smokeDecay, 0.0f, 2.0f,
         "How fast the smoke thins out, per second."},
    };
    return params;
}

// --- solver ------------------------------------------------------------------------

PyroSolver::PyroSolver(const PyroSettings& settings) : settings_(settings) { reset(); }

void PyroSolver::setSettings(const PyroSettings& settings) {
    const bool resize = settings.resolution != settings_.resolution;
    settings_ = settings;
    if (resize) reset();
}

void PyroSolver::reset() {
    nx_ = std::max(8, (settings_.resolution + 7) / 8 * 8);
    ny_ = nx_ / 2 * 3;
    nz_ = nx_;
    for (int a = 0; a < 3; ++a) {
        const int fx = nx_ + (a == 0), fy = ny_ + (a == 1), fz = nz_ + (a == 2);
        vel_[a] = Grid(fx, fy, fz);
        velNext_[a] = Grid(fx, fy, fz);
    }
    for (Grid* g : {&density_, &temperature_, &fuel_, &flame_, &back_[0], &back_[1], &back_[2], &forward_[0], &forward_[1],
                    &forward_[2], &predicted_, &lo_, &hi_, &corrected_, &expansion_, &pressure_, &divergence_,
                    &centre_[0], &centre_[1], &centre_[2], &curl_[0], &curl_[1], &curl_[2], &curlLength_}) {
        *g = Grid(nx_, ny_, nz_);
    }
    frame_ = 0;
    time_ = 0.0f;
}

void PyroSolver::velocityAt(float x, float y, float z, float out[3]) const {
    out[0] = vel_[0].sample(x + 0.5f, y, z);
    out[1] = vel_[1].sample(x, y + 0.5f, z);
    out[2] = vel_[2].sample(x, y, z + 0.5f);
}

void PyroSolver::step() {
    const int n = std::max(1, settings_.substeps);
    const float dt = settings_.timeStep / static_cast<float>(n);
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

void PyroSolver::emit(float dt) {
    const PyroSettings& s = settings_;
    const float h = cellSize(), r = s.sourceRadius, t = time_;
    // How much of the source is at a point (domain units), and its flicker.
    auto source = [&](float x, float y, float z, float& weight, float& flicker) {
        const float dx = x - 0.5f, dy = y - s.sourceHeight, dz = z - 0.5f;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= r) return false;
        weight = 1.0f - smoothstep(0.6f * r, r, d);
        // Noise that moves up through the source.
        const float n = noise3(x * 14.0f, y * 14.0f - t * 7.0f, z * 14.0f, s.seed);
        flicker = std::max(0.0f, 1.0f + s.sourceNoise * 1.5f * (2.0f * n - 1.0f));
        return true;
    };

    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
        float w, m;
        if (!source((static_cast<float>(i) + 0.5f) * h, (static_cast<float>(j) + 0.5f) * h,
                    (static_cast<float>(k) + 0.5f) * h, w, m)) {
            return;
        }
        const size_t c = density_.index(i, j, k);
        fuel_.data()[c] += s.fuelRate * dt * w * m;
        density_.data()[c] += s.smokeRate * dt * w * m;
        temperature_.data()[c] += s.heatRate * dt * w * m;
    });
    // Push the gas up, and a little sideways, so the plume does not stay a column.
    for (int a = 0; a < 3; ++a) {
        Grid& vel = vel_[a];
        forEachCell(vel, [&](int i, int j, int k) {
            const float x = (static_cast<float>(i) + faceOffset(a, 0)) * h;
            const float y = (static_cast<float>(j) + faceOffset(a, 1)) * h;
            const float z = (static_cast<float>(k) + faceOffset(a, 2)) * h;
            float w, m;
            if (!source(x, y, z, w, m)) return;
            float& v = vel.at(i, j, k);
            if (a == 1) {
                v = std::max(v, s.sourceSpeed * w * (0.6f + 0.4f * m));
            } else {
                const float wobble = noise3(x * 6.0f + 11.0f * static_cast<float>(a), y * 6.0f - t * 4.0f, z * 6.0f,
                                            s.seed + 1 + static_cast<uint32_t>(a));
                v += s.sourceSpeed * 0.3f * s.sourceNoise * w * (wobble - 0.5f);
            }
        });
    }
}

void PyroSolver::advect(float dt) {
    const float cells = dt / cellSize();  // velocity x dt, in cells
    // Where the gas of each cell was a step ago, and where it will be (RK2).
    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
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
    Grid& out = velNext_[axis];
    forEachCell(out, [&](int i, int j, int k) {
        const float x = static_cast<float>(i) + ox, y = static_cast<float>(j) + oy, z = static_cast<float>(k) + oz;
        float v0[3], v1[3];
        faceVelocity(axis, i, j, k, v0);
        velocityAt(x - 0.5f * cells * v0[0], y - 0.5f * cells * v0[1], z - 0.5f * cells * v0[2], v1);
        // This component where the gas came from. Grid::sample puts sample
        // (0, 0, 0) at 0.5: shift by what the faces lack of it.
        out.at(i, j, k) = vel_[axis].sample(x - cells * v1[0] + (0.5f - ox), y - cells * v1[1] + (0.5f - oy),
                                            z - cells * v1[2] + (0.5f - oz));
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

void PyroSolver::combust(float dt) {
    const PyroSettings& s = settings_;
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

void PyroSolver::addForces(float dt) {
    const PyroSettings& s = settings_;
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
    if (s.turbulence > 0.0f) addTurbulence(dt);
    if (s.vorticity <= 0.0f) return;

    // Vorticity confinement: find the swirls, push along them. Worked out at
    // the cell centres, then spread to the faces.
    const float h = cellSize();
    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
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
    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
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
    const float strength = s.vorticity * h;
    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
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

void PyroSolver::addTurbulence(float dt) {
    const PyroSettings& s = settings_;
    // A random force on a coarse lattice, a knot every turbulenceScale, that
    // changes smoothly a few times a second and is interpolated to the faces.
    // Random, so it pushes the gas together here and apart there; the
    // projection that follows keeps only its swirling part.
    const float h = cellSize();
    const float cellsPerKnot = std::max(s.turbulenceScale / h, 2.0f);
    const int kx = static_cast<int>(std::ceil(static_cast<float>(nx_) / cellsPerKnot)) + 2;
    const int ky = static_cast<int>(std::ceil(static_cast<float>(ny_) / cellsPerKnot)) + 2;
    const int kz = static_cast<int>(std::ceil(static_cast<float>(nz_) / cellsPerKnot)) + 2;
    const float clock = time_ * 4.0f;
    const int slice = static_cast<int>(std::floor(clock));
    const float blend = smoothstep(0.0f, 1.0f, clock - static_cast<float>(slice));
    for (int a = 0; a < 3; ++a) {
        Grid& knots = noise_[a];
        if (knots.nx() != kx || knots.ny() != ky || knots.nz() != kz) knots = Grid(kx, ky, kz);
        const uint32_t seed = s.seed * 7919u + 104729u * static_cast<uint32_t>(a + 1);
        for (int k = 0; k < kz; ++k) {
            for (int j = 0; j < ky; ++j) {
                for (int i = 0; i < kx; ++i) {
                    const float now = lattice(i, j, k, seed + static_cast<uint32_t>(slice) * 31u);
                    const float next = lattice(i, j, k, seed + static_cast<uint32_t>(slice + 1) * 31u);
                    knots.at(i, j, k) = 2.0f * (now + (next - now) * blend) - 1.0f;
                }
            }
        }
    }
    for (int a = 0; a < 3; ++a) {
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        const float ox = faceOffset(a, 0), oy = faceOffset(a, 1), oz = faceOffset(a, 2);
        forEachCell(vel_[a], [&](int i, int j, int k) {
            // Only where something burns or is hot: the cells on either side.
            const int f = a == 0 ? i : a == 1 ? j : k;
            const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
            float mask = 0.0f;
            if (f > 0) mask += temperature_.at(bi, bj, bk) + fuel_.at(bi, bj, bk);
            if (f < n) mask += temperature_.at(i, j, k) + fuel_.at(i, j, k);
            if (mask <= 0.0f) return;
            const float push = noise_[a].sample((static_cast<float>(i) + ox) / cellsPerKnot + 0.5f,
                                                (static_cast<float>(j) + oy) / cellsPerKnot + 0.5f,
                                                (static_cast<float>(k) + oz) / cellsPerKnot + 0.5f);
            vel_[a].at(i, j, k) += dt * s.turbulence * std::min(mask, 1.0f) * push;
        });
    }
}

float PyroSolver::divergence(int i, int j, int k) const {
    return (vel_[0].at(i + 1, j, k) - vel_[0].at(i, j, k) + vel_[1].at(i, j + 1, k) - vel_[1].at(i, j, k) +
            vel_[2].at(i, j, k + 1) - vel_[2].at(i, j, k)) /
           cellSize();
}

void PyroSolver::project() {
    const float h = cellSize();
    forEachCell(nx_, ny_, nz_, [&](int i, int j, int k) {
        divergence_.at(i, j, k) = divergence(i, j, k) - expansion_.at(i, j, k);
    });
    // The pressure of the previous step is the first guess.
    poisson_.solve(pressure_, divergence_, h, settings_.pressureCycles);
    // Subtract its gradient. Open boundaries: the pressure is 0 on the faces of
    // the domain -- a ghost cell outside holds minus the cell inside.
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
}

void PyroSolver::dissipate(float dt) {
    const float smoke = std::exp(-settings_.smokeDecay * dt), heat = std::exp(-settings_.cooling * dt);
    const float flame = settings_.flameLife > 0.0f ? std::exp(-dt / settings_.flameLife) : 0.0f;
    forEachCell(density_, [&](int i, int j, int k) {
        const size_t c = density_.index(i, j, k);
        density_.data()[c] = std::max(0.0f, density_.data()[c]) * smoke;
        temperature_.data()[c] = std::max(0.0f, temperature_.data()[c]) * heat;
        fuel_.data()[c] = std::max(0.0f, fuel_.data()[c]);
        flame_.data()[c] = std::max(0.0f, flame_.data()[c]) * flame;
    });
}

double PyroSolver::meanDivergence() const {
    double sum = 0.0;
    for (int k = 0; k < nz_; ++k) {
        for (int j = 0; j < ny_; ++j) {
            for (int i = 0; i < nx_; ++i) sum += std::fabs(divergence(i, j, k) - expansion_.at(i, j, k));
        }
    }
    return sum / static_cast<double>(density_.size());
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
