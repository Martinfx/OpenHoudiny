#include "pg/core/Tree.h"

#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kGolden = 2.39996323f;  // radians: the golden angle, 137.5 degrees
// No more than these a tree, whatever the settings ask: 200 branches on
// each of 200 on each of 200 would be eight million.
constexpr size_t kMostStems = 200000, kMostLeaves = 1000000;

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// Numbers of their own for each part of a tree: the same whichever order
/// the parts are made in.
struct Random {
    uint64_t state;

    Random(uint64_t seed, uint64_t a, uint64_t b = 0)
        : state(seed * 0x9E3779B97F4A7C15ull ^ (a + 0x632BE59BD9B4E019ull) * 0xD6E8FEB86659FD93ull ^
                (b + 1) * 0xBF58476D1CE4E5B9ull) {
        splitmix(state);
    }
    float unit() { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1u << 24); }  // [0, 1)
    float centred() { return 2.0f * unit() - 1.0f; }                                                  // [-1, 1)
    /// A way, any way.
    Vec3 direction() {
        const float z = centred(), a = 2.0f * kPi * unit();
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return Vec3(r * std::cos(a), r * std::sin(a), z);
    }
};

const Vec3 kUp(0.0f, 1.0f, 0.0f);

/// A unit vector square to the unit vector `d`.
Vec3 perpendicular(const Vec3& d) {
    const Vec3 a = std::fabs(d.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : std::fabs(d.y) < 0.6f ? kUp : Vec3(0.0f, 0.0f, 1.0f);
    return normalize(cross(d, a));
}

/// How long the first branches are from the crown's foot (0) to its top
/// (1), a share of the longest -- Weber and Penn's shape ratios.
float outline(TreeSettings::Shape shape, float s) {
    const float r = std::clamp(1.0f - s, 0.0f, 1.0f);  // theirs: 1 at the foot, 0 at the top
    using Shape = TreeSettings::Shape;
    float ratio = 1.0f;
    switch (shape) {
        case Shape::Conical: ratio = 0.2f + 0.8f * r; break;
        case Shape::Spherical:
        case Shape::Weeping: ratio = 0.2f + 0.8f * std::sin(kPi * r); break;
        case Shape::Hemispherical: ratio = 0.2f + 0.8f * std::sin(0.5f * kPi * r); break;
        case Shape::Cylindrical: ratio = 1.0f; break;
        case Shape::Flame: ratio = r <= 0.7f ? r / 0.7f : (1.0f - r) / 0.3f; break;
        case Shape::Umbrella: ratio = 1.0f - 0.8f * r; break;
    }
    return std::max(ratio, 0.1f);
}

/// Where on a stem `t` of its length is: the point, the way it goes there
/// and its radius. Its points are as far apart all along.
void sample(const TreeStem& st, float t, Vec3& at, Vec3& dir, float& r) {
    const size_t n = st.points.size() - 1;
    const float f = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n);
    const size_t i = std::min(static_cast<size_t>(f), n - 1);
    const float u = f - static_cast<float>(i);
    at = st.points[i] * (1.0f - u) + st.points[i + 1] * u;
    dir = normalize(st.points[i + 1] - st.points[i]);
    r = st.radius[i] * (1.0f - u) + st.radius[i + 1] * u;
}

/// How a stem turns as it grows, in radians over its length: a random
/// wander, a lift up to the light, a sag under its weight -- more towards
/// its tip -- and a steady bow.
struct Turning {
    float wander = 0.0f, lift = 0.0f, sag = 0.0f;
    Vec3 bow;
};

TreeStem growStem(int level, const Vec3& base, const Vec3& dir, float length, float r0, float r1, int pieces,
                  const Turning& turn, Random& random) {
    TreeStem st;
    st.level = level;
    st.length = length;
    st.points.reserve(static_cast<size_t>(pieces) + 1);
    st.radius.reserve(static_cast<size_t>(pieces) + 1);
    Vec3 p = base, d = normalize(dir);
    st.points.push_back(p);
    st.radius.push_back(r0);
    const float dt = 1.0f / static_cast<float>(pieces), ds = length * dt;
    // A random walk: as far off, whatever the number of pieces.
    const float wander = turn.wander * std::sqrt(dt);
    for (int i = 1; i <= pieces; ++i) {
        const float t = static_cast<float>(i) * dt;
        Vec3 push = random.direction() * wander + kUp * (turn.lift * dt) + turn.bow * dt - kUp * (2.0f * turn.sag * t * dt);
        push = push - d * dot(push, d);  // along the way it goes is no turn
        d = normalize(d + push);
        p += d * ds;
        st.points.push_back(p);
        st.radius.push_back(r0 + (r1 - r0) * t);
    }
    return st;
}

/// A colour a little lighter or darker than `c`, and a little warmer.
Vec3 shade(const Vec3& c, float variation, Random& random) {
    const float light = 1.0f + 0.5f * variation * random.centred();
    const float warm = 0.35f * variation * random.unit();
    const Vec3 out = (c * (1.0f - warm) + Vec3(0.55f, 0.47f, 0.12f) * warm) * light;
    return Vec3(std::clamp(out.x, 0.0f, 1.0f), std::clamp(out.y, 0.0f, 1.0f), std::clamp(out.z, 0.0f, 1.0f));
}

}  // namespace

Tree growTree(const TreeSettings& s, const Vec3& base, float scale, uint64_t seed) {
    Tree tree;
    scale = std::max(scale, 1e-3f);
    Random random(seed, 0);
    tree.barkColor = s.barkColor * (1.0f + 0.25f * s.variation * random.centred());

    // The trunk: up, leaning a little one way and bowing back.
    const float height = std::max(s.height, 0.01f) * scale;
    tree.height = height;
    const float radius = std::max(s.radius, 1e-4f) * scale;
    const float tipRadius = radius * std::clamp(s.tip, 0.0f, 1.0f);
    const float segment = std::max(s.segment, 0.01f) * scale;
    const float heading = 2.0f * kPi * random.unit();
    const Vec3 bowWay(std::cos(heading), 0.0f, std::sin(heading));
    const float lean = std::clamp(s.lean, 0.0f, 1.0f), wobble = std::max(s.wobble, 0.0f);
    Turning trunkTurn;
    trunkTurn.wander = 0.25f * wobble;
    trunkTurn.bow = bowWay * (-0.5f * lean);
    Random trunkRandom(seed, 1);
    const int trunkPieces = std::clamp(static_cast<int>(std::ceil(height / segment)), 6, 400);
    TreeStem trunk = growStem(0, base, normalize(kUp + bowWay * (0.35f * lean)), height, radius, tipRadius, trunkPieces,
                              trunkTurn, trunkRandom);
    // Its foot flares out.
    for (size_t i = 0; i < trunk.points.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(trunkPieces);
        trunk.radius[i] *= 1.0f + std::max(s.flare, 0.0f) * std::exp(-18.0f * t);
    }

    // Where it forks, it goes on as leaders -- stems of the trunk's level --
    // each as thick as their areas add up to the trunk's, diverging.
    // `from` is how far up the trunk each begins, `until` where branches
    // may grow on it, a share of its length.
    std::vector<float> from{0.0f}, until{0.97f};
    const int forks = std::clamp(s.forks, 1, 5);
    if (forks >= 2) {
        const int f = std::clamp(static_cast<int>(std::lround(std::clamp(s.forkHeight, 0.0f, 1.0f) *
                                                              static_cast<float>(trunkPieces))),
                                 1, trunkPieces - 2);
        const Vec3 at = trunk.points[static_cast<size_t>(f)];
        const Vec3 dir = normalize(at - trunk.points[static_cast<size_t>(f - 1)]);
        const float r = trunk.radius[static_cast<size_t>(f)];
        // The trunk ends a piece past the fork, its tip inside the leaders.
        trunk.points.resize(static_cast<size_t>(f) + 2);
        trunk.radius.resize(static_cast<size_t>(f) + 2);
        trunk.length = height * static_cast<float>(f + 1) / static_cast<float>(trunkPieces);
        until[0] = static_cast<float>(f) / static_cast<float>(f + 1);
        tree.stems.push_back(std::move(trunk));
        Random fork(seed, 5);
        const float offset = 2.0f * kPi * fork.unit();
        const float rest = height * (1.0f - static_cast<float>(f) / static_cast<float>(trunkPieces));
        const Vec3 side0 = perpendicular(dir);
        for (int j = 0; j < forks; ++j) {
            const Vec3 side = glm::rotate(side0, offset + 2.0f * kPi * static_cast<float>(j) / static_cast<float>(forks) +
                                                     0.3f * fork.centred(), dir);
            const float off = std::clamp(s.forkAngle, 0.0f, 90.0f) * (1.0f + 0.2f * fork.centred()) * kPi / 180.0f;
            // As much as the branches turn up to the light, they turn back up.
            Turning turn = trunkTurn;
            turn.bow = Vec3();
            turn.lift = off * std::clamp(3.0f * s.up, 0.0f, 1.0f);
            const float length = rest * (0.85f + 0.25f * fork.unit());
            const int pieces = std::clamp(static_cast<int>(std::ceil(length / segment)), 4, 400);
            Random grow(seed, 6, static_cast<uint64_t>(j));
            TreeStem leader = growStem(0, at, normalize(dir * std::cos(off) + side * std::sin(off)), length,
                                       r * 1.1f / std::sqrt(static_cast<float>(forks)), tipRadius, pieces, turn, grow);
            leader.parent = 0;
            leader.along = until[0];
            leader.path = height * static_cast<float>(f) / static_cast<float>(trunkPieces);
            tree.stems.push_back(std::move(leader));
            from.push_back(height * static_cast<float>(f) / static_cast<float>(trunkPieces));
            until.push_back(0.97f);
        }
    } else {
        tree.stems.push_back(std::move(trunk));
    }

    // Level by level, the branches of each stem of the level before. The
    // first ones are shared among the trunk's stems as much of each is in
    // the crown, and are as long as the crown's outline says where they are.
    const int levels = std::clamp(s.levels, 0, 3);
    const float minLength = std::max(0.02f, 0.5f * s.leafSize) * scale;
    const float crownFoot = std::clamp(s.crown, 0.0f, 0.95f) * height;
    const bool weeping = s.shape == TreeSettings::Shape::Weeping;
    std::vector<int> firstCounts(tree.stems.size(), 0);
    {
        std::vector<float> inCrown(tree.stems.size(), 0.0f);
        float total = 0.0f;
        for (size_t i = 0; i < tree.stems.size(); ++i) {
            inCrown[i] = std::max(0.0f, from[i] + until[i] * tree.stems[i].length - std::max(crownFoot, from[i]));
            total += inCrown[i];
        }
        const float count = static_cast<float>(std::max(s.branches[0], 0));
        float sum = 0.0f;
        for (size_t i = 0; i < tree.stems.size() && total > 0.0f; ++i) {
            const int before = static_cast<int>(std::lround(sum / total * count));
            sum += inCrown[i];
            firstCounts[i] = static_cast<int>(std::lround(sum / total * count)) - before;
        }
    }
    size_t parentsBegin = 0, parentsEnd = tree.stems.size();
    for (int level = 1; level <= levels; ++level) {
        const size_t l = static_cast<size_t>(level - 1);
        Turning turn;
        turn.wander = wobble;
        turn.lift = 1.2f * std::max(s.up, 0.0f);
        // Thinner, they sag more; a willow's twigs hang.
        turn.sag = 2.0f * std::max(s.gravity, 0.0f) * (1.0f + 0.5f * static_cast<float>(level - 1));
        if (weeping && level >= 2) turn.sag *= 4.0f;
        const float pieceLength = segment * std::pow(0.75f, static_cast<float>(level));
        bool full = false;
        for (size_t pi = parentsBegin; pi < parentsEnd && !full; ++pi) {
            // What of the parent is needed: the stems grow as its children are added.
            const TreeStem parent = tree.stems[pi];
            const int count = level == 1 ? firstCounts[pi] : std::max(s.branches[l], 0);
            if (count == 0) continue;
            Random place(seed, 2, pi);
            float t0 = 0.12f, t1 = 0.97f;
            if (level == 1) {
                t0 = std::clamp((crownFoot - from[pi]) / parent.length, 0.0f, 1.0f);
                t1 = until[pi];
            }
            const float offset = 2.0f * kPi * place.unit();
            for (int k = 0; k < count; ++k) {
                if (tree.stems.size() >= kMostStems) {
                    full = true;
                    break;
                }
                const float u = std::clamp((static_cast<float>(k) + 0.5f + 0.4f * place.centred()) / static_cast<float>(count),
                                           0.0f, 1.0f);
                const float t = t0 + (t1 - t0) * u;
                Vec3 at, dir;
                float pr = 0.0f;
                sample(parent, t, at, dir, pr);
                // Round the parent a golden angle on from the one before, off
                // it by the level's angle.
                const Vec3 side = glm::rotate(perpendicular(dir), offset + static_cast<float>(k) * kGolden + 0.2f * place.centred(), dir);
                const float off = s.angle[l] * (1.0f + 0.15f * place.centred()) * kPi / 180.0f;
                const Vec3 way = normalize(dir * std::cos(off) + side * std::sin(off));
                // The first branches as the crown's outline says; the others
                // shorter towards their parent's tip -- a weeping tree's
                // hardly: its twigs hang long all along.
                float reach = 0.0f;
                if (level == 1) {
                    const float up = (from[pi] + t * parent.length - crownFoot) / std::max(height - crownFoot, 1e-3f);
                    reach = std::min(height * std::max(s.length[0], 0.0f) * outline(s.shape, up), height);
                } else {
                    reach = parent.length * std::max(s.length[l], 0.0f) * (1.0f - (weeping ? 0.2f : 0.6f) * u);
                }
                reach *= 0.85f + 0.3f * place.unit();
                if (reach < minLength) continue;
                const float r0 = pr * std::clamp(s.thickness, 0.05f, 0.95f);
                const int pieces = std::clamp(static_cast<int>(std::ceil(reach / pieceLength)), 3, 60);
                Random grow(seed, 3, (static_cast<uint64_t>(pi) << 16) | static_cast<uint64_t>(k));
                TreeStem branch = growStem(level, at, way, reach, r0, r0 * 0.15f, pieces, turn, grow);
                branch.parent = static_cast<int>(pi);
                branch.along = t;
                branch.path = parent.path + t * parent.length;
                tree.stems.push_back(std::move(branch));
            }
        }
        parentsBegin = parentsEnd;
        parentsEnd = tree.stems.size();
        if (parentsBegin == parentsEnd) break;
    }

    // Leaves on the twigs -- the stems nothing grows from -- and a tuft at
    // the tip of the others, the young wood: none on the trunk below the
    // crown, none where it forks -- it goes on as its leaders.
    if (s.leaves > 0) {
        std::vector<uint8_t> bare(tree.stems.size(), 1), forked(tree.stems.size(), 0);
        for (const TreeStem& st : tree.stems) {
            if (st.parent < 0) continue;
            const size_t parent = static_cast<size_t>(st.parent);
            bare[parent] = 0;
            if (st.level == tree.stems[parent].level) forked[parent] = 1;
        }
        const int tuft = std::max(1, s.leaves / 3);
        for (size_t si = 0; si < tree.stems.size(); ++si) {
            const TreeStem& st = tree.stems[si];
            if (forked[si]) continue;
            const int count = bare[si] ? s.leaves : tuft;
            const float t0 = !bare[si] ? 0.85f : st.level == 0 ? std::clamp(s.crown, 0.0f, 0.95f) : 0.25f;
            Random leafRandom(seed, 4, si);
            const float offset = 2.0f * kPi * leafRandom.unit();
            for (int j = 0; j < count && tree.leaves.size() < kMostLeaves; ++j) {
                const float t = t0 + (1.0f - t0) *
                                         std::clamp((static_cast<float>(j) + 0.5f + 0.4f * leafRandom.centred()) /
                                                        static_cast<float>(count),
                                                    0.0f, 1.0f);
                Vec3 at, dir;
                float r = 0.0f;
                sample(st, t, at, dir, r);
                const Vec3 side = glm::rotate(perpendicular(dir), offset + static_cast<float>(j) * kGolden, dir);
                TreeLeaf leaf;
                // Out from the twig, a little forward and up.
                leaf.along = normalize(side * 0.8f + dir * 0.45f + kUp * 0.25f + leafRandom.direction() * 0.2f);
                // Its blade to the sky as far as it can, turned a little.
                Vec3 facing = kUp - leaf.along * leaf.along.y;
                facing = length(facing) > 1e-3f ? normalize(facing) : perpendicular(leaf.along);
                leaf.facing = glm::rotate(facing, 0.6f * leafRandom.centred(), leaf.along);
                leaf.at = at + side * r;
                leaf.size = std::max(s.leafSize, 1e-4f) * scale * (0.8f + 0.4f * leafRandom.unit());
                leaf.color = shade(s.leafColor, s.variation, leafRandom);
                leaf.stem = static_cast<int>(si);
                leaf.path = st.path + t * st.length;
                tree.leaves.push_back(leaf);
            }
        }
    }
    return tree;
}

namespace {

/// The outline of a leaf: along its axis (0 its base, 1 its tip) and
/// across it, both a share of its length.
std::span<const std::array<float, 2>> leafOutline(TreeSettings::Leaf shape) {
    static const std::array<float, 2> broad[] = {{0.0f, 0.0f},  {0.18f, 0.2f},   {0.5f, 0.3f},   {0.82f, 0.18f},
                                                 {1.0f, 0.0f},  {0.82f, -0.18f}, {0.5f, -0.3f},  {0.18f, -0.2f}};
    static const std::array<float, 2> narrow[] = {{0.0f, 0.0f}, {0.2f, 0.07f}, {0.6f, 0.08f}, {1.0f, 0.0f},
                                                  {0.6f, -0.08f}, {0.2f, -0.07f}};
    // A spray of needles, toothed: each corner seen from the base a little
    // further round than the one before, so that a fan from it covers it.
    static const std::array<float, 2> needle[] = {{0.0f, 0.0f},   {0.15f, 0.16f},  {0.3f, 0.08f},   {0.62f, 0.15f},
                                                  {0.75f, 0.06f}, {1.0f, 0.0f},    {0.75f, -0.06f}, {0.62f, -0.15f},
                                                  {0.3f, -0.08f}, {0.15f, -0.16f}};
    switch (shape) {
        case TreeSettings::Leaf::Narrow: return narrow;
        case TreeSettings::Leaf::Needles: return needle;
        case TreeSettings::Leaf::Broad: break;
    }
    return broad;
}

/// Metres of bark one picture of it covers (examples/textures/bark).
constexpr float kBarkPicture = 1.0f;

/// The corner of the quarter of the leaf picture a leaf's uv is in -- the
/// picture of examples/textures/leaf: a broad leaf top left and another
/// top right, a narrow one bottom left, a spray of needles bottom right,
/// each its base at the middle of the bottom of its quarter, its tip at
/// the top. Broad leaves take either, by `index`.
Vec2 leafCell(TreeSettings::Leaf shape, size_t index) {
    switch (shape) {
        case TreeSettings::Leaf::Narrow: return Vec2(0.0f, 0.0f);
        case TreeSettings::Leaf::Needles: return Vec2(0.5f, 0.0f);
        case TreeSettings::Leaf::Broad: break;
    }
    return Vec2(((index * 0x9E3779B97F4A7C15ull) >> 63) != 0 ? 0.5f : 0.0f, 0.5f);
}

/// The unit quaternion x, y, z, w that turns x, y and z to the unit
/// vectors `X`, `Y`, `Z`, square to each other.
Vec4 quaternionOf(const Vec3& X, const Vec3& Y, const Vec3& Z) {
    const float trace = X.x + Y.y + Z.z;
    if (trace > 0.0f) {
        const float r = 2.0f * std::sqrt(trace + 1.0f);
        return Vec4((Y.z - Z.y) / r, (Z.x - X.z) / r, (X.y - Y.x) / r, 0.25f * r);
    }
    if (X.x > Y.y && X.x > Z.z) {
        const float r = 2.0f * std::sqrt(1.0f + X.x - Y.y - Z.z);
        return Vec4(0.25f * r, (Y.x + X.y) / r, (Z.x + X.z) / r, (Y.z - Z.y) / r);
    }
    if (Y.y > Z.z) {
        const float r = 2.0f * std::sqrt(1.0f + Y.y - X.x - Z.z);
        return Vec4((Y.x + X.y) / r, 0.25f * r, (Z.y + Y.z) / r, (Z.x - X.z) / r);
    }
    const float r = 2.0f * std::sqrt(1.0f + Z.z - X.x - Y.y);
    return Vec4((Z.x + X.z) / r, (Z.y + Y.z) / r, 0.25f * r, (X.y - Y.x) / r);
}

/// Primitive attributes of the ones added from `first` on.
void primitiveInts(Geometry& geo, const char* name, size_t first, const std::vector<int32_t>& values) {
    auto out = geo.primitives().create(name, AttrType::Int).write<int32_t>();
    std::copy(values.begin(), values.end(), out.begin() + static_cast<std::ptrdiff_t>(first));
}

}  // namespace

void meshTree(const Tree& tree, const TreeSettings& s, int treeIndex, Geometry& geo) {
    std::vector<Vec3> P, Cd;
    std::vector<float> flex;
    std::vector<uint32_t> corners, sizes;
    std::vector<Vec3> uvs;  // each corner's, as Houdini keeps it: (u, v, 0)
    const float perHeight = 1.0f / std::max(tree.height, 1e-6f);
    std::vector<int32_t> level, stem;
    const uint32_t first = static_cast<uint32_t>(geo.pointCount());
    auto polygon = [&](std::initializer_list<uint32_t> points, std::initializer_list<Vec3> uv, int lv, int st) {
        corners.insert(corners.end(), points.begin(), points.end());
        uvs.insert(uvs.end(), uv.begin(), uv.end());
        sizes.push_back(static_cast<uint32_t>(points.size()));
        level.push_back(lv);
        stem.push_back(st);
    };

    // Each stem a tube: a ring round each of its points but the last, a
    // point at its tip; the rings turned along with it -- no twist.
    for (size_t si = 0; si < tree.stems.size(); ++si) {
        const TreeStem& st = tree.stems[si];
        const size_t m = st.points.size();
        if (m < 2) continue;
        // Never six: their edges would be as sharp as the viewport's crease,
        // 60 degrees -- smooth one frame, sharp the next as the tree sways.
        int around = std::max(3, st.level == 0 ? s.sides : s.sides - 2 * st.level);
        if (around == 6) around = 7;
        const uint32_t sides = static_cast<uint32_t>(around);
        // Its uv: round it as many whole pictures of bark (kBarkPicture
        // metres across) as it is round at its foot, one at least; up it
        // as far in the same measure -- the pictures square at its foot,
        // narrower as it thins, as SpeedTree lays them.
        const float girth = 2.0f * kPi * std::max(st.radius[0], 1e-5f);
        const float pictures = std::max(1.0f, std::round(girth / kBarkPicture));
        const float perMetre = pictures / girth;
        auto barkUv = [&](float k, size_t i) {
            return Vec3(pictures * k / static_cast<float>(sides),
                        perMetre * st.length * static_cast<float>(i) / static_cast<float>(m - 1), 0.0f);
        };
        const Vec3 colour = tree.barkColor * (1.0f + 0.12f * static_cast<float>(st.level));  // the young wood lighter
        const uint32_t base = first + static_cast<uint32_t>(P.size());
        Vec3 normal = perpendicular(normalize(st.points[1] - st.points[0]));
        for (size_t i = 0; i + 1 < m; ++i) {
            const Vec3 tangent = normalize(st.points[i + 1] - st.points[i == 0 ? 0 : i - 1]);
            normal = normal - tangent * dot(normal, tangent);
            normal = length(normal) > 1e-6f ? normalize(normal) : perpendicular(tangent);
            const Vec3 binormal = cross(tangent, normal);
            const float bend = (st.path + st.length * static_cast<float>(i) / static_cast<float>(m - 1)) * perHeight;
            for (uint32_t k = 0; k < sides; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(sides);
                P.push_back(st.points[i] + (normal * std::cos(a) + binormal * std::sin(a)) * st.radius[i]);
                Cd.push_back(colour);
                flex.push_back(bend);
            }
        }
        const uint32_t tip = first + static_cast<uint32_t>(P.size());
        P.push_back(st.points[m - 1]);
        Cd.push_back(colour);
        flex.push_back((st.path + st.length) * perHeight);
        const uint32_t rings = static_cast<uint32_t>(m - 1);
        const int id = static_cast<int>(si);
        for (uint32_t i = 0; i + 1 < rings; ++i) {
            for (uint32_t k = 0; k < sides; ++k) {
                const uint32_t k1 = (k + 1) % sides;
                const float fk = static_cast<float>(k);
                polygon({base + i * sides + k, base + i * sides + k1, base + (i + 1) * sides + k1, base + (i + 1) * sides + k},
                        {barkUv(fk, i), barkUv(fk + 1.0f, i), barkUv(fk + 1.0f, i + 1), barkUv(fk, i + 1)}, st.level, id);
            }
        }
        for (uint32_t k = 0; k < sides; ++k) {
            const float fk = static_cast<float>(k);
            polygon({base + (rings - 1) * sides + k, base + (rings - 1) * sides + (k + 1) % sides, tip},
                    {barkUv(fk, rings - 1), barkUv(fk + 1.0f, rings - 1), barkUv(fk + 0.5f, m - 1)}, st.level, id);
        }
        if (st.level == 0) {
            // The trunk's foot, closed: its face turned down, its uv the
            // bark seen from below.
            for (uint32_t k = 0; k < sides; ++k) {
                const float a = 2.0f * kPi * static_cast<float>(sides - 1 - k) / static_cast<float>(sides);
                corners.push_back(base + sides - 1 - k);
                uvs.push_back(Vec3(0.5f - 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a), 0.0f) * (girth / kPi / kBarkPicture));
            }
            sizes.push_back(sides);
            level.push_back(0);
            stem.push_back(id);
        }
    }

    // The leaves, their blades folded a little along the midrib; each its
    // uv in its quarter of the leaf picture (leafCell).
    const auto blade = leafOutline(s.leaf);
    for (size_t li = 0; li < tree.leaves.size(); ++li) {
        const TreeLeaf& leaf = tree.leaves[li];
        // Round its outline anticlockwise seen from the way it faces.
        const Vec3 across = normalize(cross(leaf.along, leaf.facing));
        const uint32_t base = first + static_cast<uint32_t>(P.size());
        const Vec2 cell = leafCell(s.leaf, li);
        for (const auto& [u, v] : blade) {
            P.push_back(leaf.at + leaf.along * (u * leaf.size) + across * (v * leaf.size) +
                        leaf.facing * (0.15f * std::fabs(v) * leaf.size));
            Cd.push_back(leaf.color);
            flex.push_back((leaf.path + u * leaf.size) * perHeight);
            // Seen from the way it faces: up it along, to the right across.
            uvs.push_back(Vec3(cell.x + 0.5f * (0.5f + v), cell.y + 0.5f * u, 0.0f));
        }
        for (uint32_t k = 0; k < blade.size(); ++k) corners.push_back(base + k);
        sizes.push_back(static_cast<uint32_t>(blade.size()));
        level.push_back(-1);
        stem.push_back(leaf.stem);
    }

    // Into the geometry.
    geo.addPoints(P.size());
    auto outP = geo.positionsForWrite();
    std::copy(P.begin(), P.end(), outP.begin() + first);
    auto outCd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    std::copy(Cd.begin(), Cd.end(), outCd.begin() + first);
    auto outFlex = geo.points().create("flex", AttrType::Float).write<float>();
    std::copy(flex.begin(), flex.end(), outFlex.begin() + first);
    const size_t prim0 = geo.primitiveCount();
    const size_t vertex0 = geo.vertexCount();
    size_t at = 0;
    for (const uint32_t n : sizes) {
        geo.addPrimitive(std::span<const uint32_t>(corners.data() + at, n), true);
        at += n;
    }
    auto outUv = geo.vertices().create("uv", AttrType::Vec3).write<Vec3>();
    std::copy(uvs.begin(), uvs.end(), outUv.begin() + static_cast<std::ptrdiff_t>(vertex0));
    primitiveInts(geo, "level", prim0, level);
    primitiveInts(geo, "stem", prim0, stem);
    primitiveInts(geo, "tree", prim0, std::vector<int32_t>(sizes.size(), treeIndex));
    // For a renderer that follows light (render/Scene.h): the leaves thin,
    // letting a share of the light through; the bark not.
    auto translucency = geo.primitives().create("translucency", AttrType::Float).write<float>();
    for (size_t i = 0; i < level.size(); ++i) translucency[prim0 + i] = level[i] < 0 ? 0.4f : 0.0f;
    // And what each is (s@material): bark, a leaf.
    std::vector<uint8_t> bark(geo.primitiveCount(), 0), leaves(geo.primitiveCount(), 0);
    for (size_t i = 0; i < level.size(); ++i) (level[i] < 0 ? leaves : bark)[prim0 + i] = 1;
    if (std::find(bark.begin(), bark.end(), 1) != bark.end()) setPrimitiveString(geo, "material", "bark", bark);
    if (std::find(leaves.begin(), leaves.end(), 1) != leaves.end()) setPrimitiveString(geo, "material", "leaf", leaves);
}

void skeletonTree(const Tree& tree, int treeIndex, Geometry& geo) {
    const size_t first = geo.pointCount();
    size_t count = tree.leaves.size();
    for (const TreeStem& st : tree.stems) count += st.points.size();
    geo.addPoints(count);
    auto P = geo.positionsForWrite();
    auto pscale = geo.points().create("pscale", AttrType::Float).write<float>();
    auto Cd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto N = geo.points().create("N", AttrType::Vec3).write<Vec3>();
    auto flex = geo.points().create("flex", AttrType::Float).write<float>();
    auto orient = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
    const float perHeight = 1.0f / std::max(tree.height, 1e-6f);
    size_t p = first;
    const size_t prim0 = geo.primitiveCount();
    std::vector<int32_t> level, stem, parent;
    std::vector<uint32_t> line;
    for (size_t si = 0; si < tree.stems.size(); ++si) {
        const TreeStem& st = tree.stems[si];
        line.clear();
        const Vec3 colour = tree.barkColor * (1.0f + 0.12f * static_cast<float>(st.level));
        for (size_t i = 0; i < st.points.size(); ++i, ++p) {
            P[p] = st.points[i];
            pscale[p] = st.radius[i];
            Cd[p] = colour;
            N[p] = normalize(st.points[std::min(i + 1, st.points.size() - 1)] - st.points[i == 0 ? 0 : i - 1]);
            flex[p] = (st.path + st.length * static_cast<float>(i) / static_cast<float>(st.points.size() - 1)) * perHeight;
            orient[p] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            line.push_back(static_cast<uint32_t>(p));
        }
        geo.addPrimitive(line, false);
        level.push_back(st.level);
        stem.push_back(static_cast<int32_t>(si));
        parent.push_back(st.parent);
    }
    for (const TreeLeaf& leaf : tree.leaves) {
        P[p] = leaf.at;
        pscale[p] = leaf.size;
        Cd[p] = leaf.color;
        N[p] = leaf.facing;
        flex[p] = leaf.path * perHeight;
        // A leaf modelled lying flat -- facing +y, its stalk at the origin,
        // pointing along +z -- turned onto this one.
        orient[p] = quaternionOf(cross(leaf.facing, leaf.along), leaf.facing, leaf.along);
        ++p;
    }
    primitiveInts(geo, "level", prim0, level);
    primitiveInts(geo, "stem", prim0, stem);
    primitiveInts(geo, "parent", prim0, parent);
    primitiveInts(geo, "tree", prim0, std::vector<int32_t>(level.size(), treeIndex));
}

}  // namespace pg
