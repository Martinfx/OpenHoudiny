//
// Vegetation as instances (Instances.h, Grass.h): points that stand for
// prototypes -- merged with their indices moved on, hashed with them, made
// copies of where they place them; turned and sized by Transform as their
// copies would be; Copy to Points' instances the copies it would make.
// Scatter's rules -- density, a painted share, slope, distance. A clump of
// grass, blade by blade; the Grass node's meadow; Tree's instances. Files:
// OBJ and PLY unpacked, USD a PointInstancer -- in a stage, and a shot's
// frames. The viewport's placements; the meadow example in the wind.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Grass.h"
#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/io/Export.h"
#include "pg/io/Obj.h"
#include "pg/io/Ply.h"
#include "pg/io/Usda.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Display.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/UsdExport.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace pg;
using namespace pg::sim;
namespace fs = std::filesystem;

namespace {

bool near(const Vec3& a, const Vec3& b, float eps) { return length(a - b) <= eps; }

/// A triangle one unit tall, up +y, coloured.
std::shared_ptr<Geometry> blade(const Vec3& colour = Vec3(0.2f, 0.5f, 0.1f)) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(3);
    auto P = geo->positionsForWrite();
    P[0] = Vec3(-0.1f, 0.0f, 0.0f);
    P[1] = Vec3(0.1f, 0.0f, 0.0f);
    P[2] = Vec3(0.0f, 1.0f, 0.0f);
    const uint32_t tri[3] = {0, 1, 2};
    geo->addPrimitive(tri, true);
    auto Cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
    std::fill(Cd.begin(), Cd.end(), colour);
    return geo;
}

/// Points at `at` that stand for prototype `which` (-1: none).
std::shared_ptr<Geometry> standing(const std::vector<Vec3>& at, const std::vector<int32_t>& which,
                                   std::vector<std::shared_ptr<Geometry>> prototypes) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(at.size());
    std::copy(at.begin(), at.end(), geo->positionsForWrite().begin());
    for (auto& p : prototypes) geo->addPrototype(std::move(p));
    auto k = geo->points().create("instance", AttrType::Int).write<int32_t>();
    std::copy(which.begin(), which.end(), k.begin());
    return geo;
}

Vec4 yaw(float radians) { return Vec4(0.0f, std::sin(0.5f * radians), 0.0f, std::cos(0.5f * radians)); }

std::unique_ptr<pg::Node> node(const char* type) {
    registerBuiltinNodes();
    return NodeRegistry::instance().create(type, type);
}

/// A flat square `size` wide, `cells` a side, at y = 0.
GeometryPtr ground(float size, int cells) {
    auto grid = node("grid");
    grid->setFloat("sizex", size);
    grid->setFloat("sizez", size);
    grid->setInt("rows", cells + 1);
    grid->setInt("cols", cells + 1);
    return grid->cookNode(CookContext{}, {});
}

/// The pool's thread count put back as it was.
struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

/// A folder of its own under the system's temporary one, gone afterwards.
struct TempFolder {
    fs::path path;
    explicit TempFolder(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("pg_test_" + name + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string operator/(const std::string& name) const { return (path / name).string(); }
};

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(instances_merged_keep_their_prototypes_apart) {
    // Two fields of instances: the second's prototypes follow the first's,
    // its points' numbers moved on by as many.
    auto a = standing({Vec3(0, 0, 0), Vec3(1, 0, 0)}, {0, 1}, {blade(), blade()});
    auto b = standing({Vec3(5, 0, 0)}, {0}, {blade(Vec3(1, 0, 0))});
    b->points().create("pscale", AttrType::Float).write<float>()[0] = 2.0f;
    // And plain geometry, standing for nothing.
    auto plain = blade();
    Geometry all = *a;
    all.append(*b);
    all.append(*plain);
    CHECK_EQ(all.prototypeCount(), size_t(3));
    CHECK(all.prototypes()[2] == b->prototypes()[0]);  // shared, not copied
    CHECK_EQ(instanceCount(all), size_t(3));
    CHECK_EQ(instanceOf(all, 0), 0);
    CHECK_EQ(instanceOf(all, 1), 1);
    CHECK_EQ(instanceOf(all, 2), 2);
    for (size_t p = 3; p < all.pointCount(); ++p) CHECK_EQ(instanceOf(all, p), -1);  // the blade's points
    // What the first had no pscale for is 1 on its instances: not sized to nothing.
    const auto pscale = all.points().find("pscale")->read<float>();
    CHECK_EQ(pscale[0], 1.0f);
    CHECK_EQ(pscale[1], 1.0f);
    CHECK_EQ(pscale[2], 2.0f);
    // New points stand for nothing.
    all.addPoints(2);
    CHECK_EQ(instanceOf(all, all.pointCount() - 1), -1);

    // The hash knows the prototypes.
    auto c = standing({Vec3(0, 0, 0), Vec3(1, 0, 0)}, {0, 1}, {blade(), blade(Vec3(0, 0, 1))});
    CHECK(a->hash() != c->hash());
    auto d = standing({Vec3(0, 0, 0), Vec3(1, 0, 0)}, {0, 1}, {blade(), blade()});
    CHECK_EQ(a->hash(), d->hash());
    // Changing a copy's prototypes leaves the first alone.
    Geometry e = *a;
    e.addPrototype(blade());
    CHECK_EQ(a->prototypeCount(), size_t(2));
    CHECK_EQ(e.prototypeCount(), size_t(3));
}

TEST(instances_unpack_where_their_points_place_them) {
    auto geo = standing({Vec3(3, 0, 0), Vec3(0, 0, 4), Vec3(9, 9, 9)}, {0, 0, -1}, {blade()});
    geo->points().create("orient", AttrType::Vec4).write<Vec4>()[1] = yaw(1.5707963f);
    geo->points().find("orient")->write<Vec4>()[0] = Vec4(0, 0, 0, 1);
    auto pscale = geo->points().create("pscale", AttrType::Float).write<float>();
    pscale[0] = 1.0f;
    pscale[1] = 2.0f;
    auto tint = geo->points().create("tint", AttrType::Vec3).write<Vec3>();
    tint[0] = Vec3(1, 1, 1);
    tint[1] = Vec3(2, 1, 0.5f);
    geo->points().create("Cd", AttrType::Vec3);  // a colour of its own does not colour what stands on it
    const auto flat = unpackInstances(*geo);
    CHECK_EQ(flat->prototypeCount(), size_t(0));
    CHECK(flat->points().find("instance") == nullptr);
    CHECK_EQ(flat->pointCount(), size_t(7));  // the loose point, then two blades
    CHECK_EQ(flat->primitiveCount(), size_t(2));
    const auto P = flat->positions();
    CHECK(near(P[0], Vec3(9, 9, 9), 1e-6f));
    CHECK(near(P[3], Vec3(3, 1, 0), 1e-5f));  // the first blade's tip, as it stands
    // The second: twice as big, turned a quarter about +y -- its +x to -z.
    CHECK(near(P[4], Vec3(0, 0, 4.2f), 1e-5f));
    CHECK(near(P[6], Vec3(0, 2, 4), 1e-5f));
    const auto Cd = flat->points().find("Cd")->read<Vec3>();
    CHECK(near(Cd[1], Vec3(0.2f, 0.5f, 0.1f), 1e-6f));
    CHECK(near(Cd[4], Vec3(0.4f, 0.5f, 0.05f), 1e-6f));  // tinted
    // The placements say the same.
    const auto places = placementsOf(*geo);
    CHECK(near(places[1].point(Vec3(0, 1, 0)), Vec3(0, 2, 4), 1e-5f));
    // The box round what they place -- each the blade's box, placed -- and
    // round all that is drawn: the loose point too.
    Vec3 lo, hi;
    instancesBox(*geo, lo, hi);
    CHECK(near(lo, Vec3(0.0f, 0.0f, 0.0f), 1e-5f));
    CHECK(near(hi, Vec3(3.1f, 2.0f, 4.2f), 1e-5f));
    drawnBox(*geo, lo, hi);
    CHECK(near(lo, Vec3(0.0f, 0.0f, 0.0f), 1e-5f));
    CHECK(near(hi, Vec3(9.0f, 9.0f, 9.0f), 1e-5f));
    // Unpacked again: nothing to do.
    CHECK_EQ(unpackInstances(*flat)->hash(), flat->hash());
    // Without them: the loose point only, and what only placed them gone.
    const auto rest = withoutInstances(*geo);
    CHECK_EQ(rest->pointCount(), size_t(1));
    CHECK(rest->points().find("orient") == nullptr && rest->points().find("tint") == nullptr);
}

TEST(transform_turns_and_sizes_instances_as_their_copies) {
    auto geo = standing({Vec3(1, 0, 0), Vec3(0, 0, 2)}, {0, 0}, {blade()});
    auto N = geo->points().create("N", AttrType::Vec3).write<Vec3>();
    N[0] = Vec3(0, 1, 0);
    N[1] = normalize(Vec3(1, 1, 0));
    auto t = node("transform");
    t->setVec3("r", Vec3(30.0f, 45.0f, 10.0f));
    t->setVec3("s", Vec3(2.0f, 2.0f, 2.0f));
    t->setVec3("t", Vec3(0.5f, 1.0f, -2.0f));
    // Transformed and then made copies of, or made copies of and then
    // transformed: the same -- turned to N, or by orient.
    auto same = [&](const GeometryPtr& points) {
        const GeometryPtr in[] = {points};
        const GeometryPtr moved = t->cookNode(CookContext{}, in);
        CHECK_EQ(moved->prototypeCount(), size_t(1));
        const GeometryPtr flatIn[] = {unpackInstances(*points)};
        const GeometryPtr a = unpackInstances(*moved), b = t->cookNode(CookContext{}, flatIn);
        CHECK_EQ(a->pointCount(), b->pointCount());
        for (size_t p = 0; p < a->pointCount(); ++p) CHECK(near(a->positions()[p], b->positions()[p], 1e-4f));
    };
    same(geo);
    auto turned = std::make_shared<Geometry>(*geo);
    turned->points().erase("N");
    auto orient = turned->points().create("orient", AttrType::Vec4).write<Vec4>();
    orient[0] = Vec4(0, 0, 0, 1);
    const float n = std::sqrt(0.3f * 0.3f + 0.5f * 0.5f + 0.2f * 0.2f + 0.8f * 0.8f);
    orient[1] = Vec4(0.3f / n, 0.5f / n, -0.2f / n, 0.8f / n);
    same(turned);
}

TEST(copy_to_points_instances_are_the_copies_it_would_make) {
    // Points turned by N, sized, tinted: instances unpacked are the copies.
    auto dots = std::make_shared<Geometry>();
    dots->addPoints(4);
    auto P = dots->positionsForWrite();
    auto N = dots->points().create("N", AttrType::Vec3).write<Vec3>();
    auto pscale = dots->points().create("pscale", AttrType::Float).write<float>();
    auto tint = dots->points().create("tint", AttrType::Vec3).write<Vec3>();
    auto kind = dots->points().create("kind", AttrType::Int).write<int32_t>();
    for (size_t i = 0; i < 4; ++i) {
        P[i] = Vec3(static_cast<float>(i), 0.0f, 0.5f * static_cast<float>(i));
        N[i] = normalize(Vec3(0.3f * static_cast<float>(i), 1.0f, 0.1f));
        pscale[i] = 0.5f + 0.25f * static_cast<float>(i);
        tint[i] = Vec3(1.0f, 0.8f, 0.6f);
        kind[i] = i == 2 ? 1 : 0;
    }
    // Two pieces, by their primitive attribute kind.
    auto pieces = blade();
    pieces->append(*blade(Vec3(0.6f, 0.4f, 0.1f)));
    auto k = pieces->primitives().create("kind", AttrType::Int).write<int32_t>();
    k[0] = 0;
    k[1] = 1;
    auto copy = node("copytopoints");
    copy->setString("pieceattribute", "kind");
    copy->setFloat("scale", 1.5f);
    const GeometryPtr in[] = {pieces, dots};
    const GeometryPtr copies = copy->cookNode(CookContext{}, in);
    copy->setBool("instance", true);
    const GeometryPtr instances = copy->cookNode(CookContext{}, in);
    CHECK_EQ(instances->pointCount(), size_t(4));
    CHECK_EQ(instances->prototypeCount(), size_t(2));
    CHECK_EQ(instanceOf(*instances, 2), 1);  // its kind's piece
    CHECK_EQ(instanceOf(*instances, 3), 0);
    const auto flat = unpackInstances(*instances);
    CHECK_EQ(flat->pointCount(), copies->pointCount());
    CHECK_EQ(flat->primitiveCount(), copies->primitiveCount());
    const auto a = flat->positions(), b = copies->positions();
    for (size_t p = 0; p < a.size(); ++p) CHECK(near(a[p], b[p], 1e-5f));
    const auto ca = flat->points().find("Cd")->read<Vec3>(), cb = copies->points().find("Cd")->read<Vec3>();
    for (size_t p = 0; p < ca.size(); ++p) CHECK(near(ca[p], cb[p], 1e-6f));
    // Unpack, the node: the same.
    auto unpack = node("unpack");
    const GeometryPtr packed[] = {instances};
    CHECK_EQ(unpack->cookNode(CookContext{}, packed)->hash(), flat->hash());
}

TEST(scatter_rules_density_paint_slope_and_distance) {
    ThreadCountGuard guard;
    // By density: four times the area, four times the points.
    auto scatter = node("scatter");
    scatter->setInt("mode", 1);
    scatter->setFloat("density", 25.0f);
    const GeometryPtr small[] = {ground(2.0f, 4)}, big[] = {ground(4.0f, 4)};
    CHECK_EQ(scatter->cookNode(CookContext{}, small)->pointCount(), size_t(100));
    CHECK_EQ(scatter->cookNode(CookContext{}, big)->pointCount(), size_t(400));

    // A share painted on: none where it is 0, all where it is 1, half at 0.5.
    auto painted = std::make_shared<Geometry>(*ground(4.0f, 40));
    auto share = painted->points().create("grass", AttrType::Float).write<float>();
    for (size_t p = 0; p < share.size(); ++p) {
        const float x = painted->positions()[p].x;
        share[p] = x < -1.05f ? 0.0f : x > 1.05f ? 1.0f : x > -0.95f && x < 0.95f ? 0.5f : share[p];
        if (x >= -1.05f && x <= -0.95f) share[p] = 0.25f;
        if (x >= 0.95f && x <= 1.05f) share[p] = 0.75f;
    }
    scatter->setFloat("density", 500.0f);
    scatter->setString("densityattribute", "grass");
    const GeometryPtr paintedIn[] = {painted};
    const GeometryPtr kept = scatter->cookNode(CookContext{}, paintedIn);
    size_t left = 0, middle = 0, right = 0;
    for (const Vec3& p : kept->positions()) {
        if (p.x < -1.1f) ++left;
        else if (p.x > -0.9f && p.x < 0.9f) ++middle;
        else if (p.x > 1.1f) ++right;
    }
    CHECK_EQ(left, size_t(0));
    CHECK(std::fabs(static_cast<double>(right) / (500.0 * 0.9 * 4.0) - 1.0) < 0.1);
    CHECK(std::fabs(static_cast<double>(middle) / (500.0 * 1.8 * 4.0 * 0.5) - 1.0) < 0.1);
    // With threads or without, the same points.
    TaskPool::instance().setThreadCount(1);
    const GeometryPtr one = scatter->cookNode(CookContext{}, paintedIn);
    TaskPool::instance().setThreadCount(4);
    CHECK_EQ(one->hash(), scatter->cookNode(CookContext{}, paintedIn)->hash());

    // Slope: on a box, the top only below 45 degrees; the sides too at 90.
    auto box = node("box");
    const GeometryPtr cube[] = {box->cookNode(CookContext{}, {})};
    auto onBox = node("scatter");
    onBox->setInt("count", 3000);
    onBox->setFloat("maxslope", 45.0f);
    const GeometryPtr tops = onBox->cookNode(CookContext{}, cube);
    CHECK(tops->pointCount() > 300 && tops->pointCount() < 700);  // a sixth of 3000
    for (const Vec3& n : tops->points().find("N")->read<Vec3>()) CHECK(n.y > 0.99f);
    onBox->setFloat("maxslope", 90.0f);
    const size_t sides = onBox->cookNode(CookContext{}, cube)->pointCount();
    CHECK(sides > 2200 && sides < 2800);  // five sixths

    // Distance: none nearer to another than asked.
    auto apart = node("scatter");
    apart->setInt("count", 2000);
    apart->setFloat("mindistance", 0.2f);
    const GeometryPtr spread = apart->cookNode(CookContext{}, small);
    const auto S = spread->positions();
    CHECK(S.size() > 40 && S.size() < 200);
    float nearest = 1e9f;
    for (size_t i = 0; i < S.size(); ++i) {
        for (size_t j = i + 1; j < S.size(); ++j) nearest = std::min(nearest, length(S[i] - S[j]));
    }
    CHECK(nearest >= 0.2f);
}

TEST(grass_clump_is_blades_from_root_to_tip) {
    GrassSettings s;
    s.blades = 12;
    s.segments = 5;
    const Geometry a = growGrassClump(s, 7), b = growGrassClump(s, 7), c = growGrassClump(s, 8);
    CHECK_EQ(a.hash(), b.hash());
    CHECK(a.hash() != c.hash());
    // Each blade: two points a row, a point at its tip; four-sided pieces and
    // a triangle at the top.
    CHECK_EQ(a.pointCount(), size_t(12 * 11));
    CHECK_EQ(a.primitiveCount(), size_t(12 * 5));
    const auto flex = a.points().find("flex")->read<float>();
    const auto Cd = a.points().find("Cd")->read<Vec3>();
    const auto bladeOf = a.primitives().find("blade")->read<int32_t>();
    CHECK_EQ(bladeOf[0], 0);
    CHECK_EQ(bladeOf[a.primitiveCount() - 1], 11);
    const auto P = a.positions();
    float top = 0.0f;
    for (size_t k = 0; k < 12; ++k) {
        const size_t root = k * 11, tip = root + 10;
        CHECK_EQ(flex[root], 0.0f);
        CHECK_EQ(flex[tip], 1.0f);
        CHECK(P[root].y < 0.0f);  // a little in the ground
        CHECK(P[tip].y > P[root].y + 0.05f);
        // Its roots within the clump's spread; darker at the root than at the tip, unless dry.
        CHECK(std::hypot(P[root].x, P[root].z) <= s.spread + s.width);
        CHECK(Cd[root].y <= Cd[tip].y + 1e-6f);
        top = std::max(top, P[tip].y);
        for (uint32_t i = 1; i < 11; ++i) CHECK(flex[root + i] >= flex[root + i - 1] - 1e-6f);
    }
    CHECK(top < s.height * (1.0f + s.heightVariation) + 1e-3f);
    CHECK(top > 0.5f * s.height);
    // Upright: no bow, no lean -- each blade straight up.
    s.bend = 0.0f;
    s.lean = 0.0f;
    const Geometry straight = growGrassClump(s, 7);
    for (size_t k = 0; k < 12; ++k) {
        const Vec3 root = straight.positions()[k * 11], tip = straight.positions()[k * 11 + 10];
        CHECK(std::fabs(tip.x - 0.5f * (root.x + straight.positions()[k * 11 + 1].x)) < 1e-4f);
    }
}

TEST(grass_node_scatters_clumps_as_instances) {
    auto grass = node("grass");
    grass->setFloat("density", 30.0f);
    grass->setInt("variants", 5);
    grass->setInt("blades", 6);
    const GeometryPtr field[] = {ground(4.0f, 8)};
    const GeometryPtr meadow = grass->cookNode(CookContext{}, field);
    CHECK_EQ(meadow->pointCount(), size_t(480));
    CHECK_EQ(meadow->primitiveCount(), size_t(0));
    CHECK_EQ(meadow->prototypeCount(), size_t(5));
    CHECK_EQ(instanceCount(*meadow), size_t(480));
    std::set<int32_t> kinds;
    for (size_t p = 0; p < meadow->pointCount(); ++p) kinds.insert(instanceOf(*meadow, p));
    CHECK_EQ(kinds.size(), size_t(5));
    // Each turned about +y only, a size and a shade of its own.
    const auto orient = meadow->points().find("orient")->read<Vec4>();
    const auto pscale = meadow->points().find("pscale")->read<float>();
    const auto tint = meadow->points().find("tint")->read<Vec3>();
    for (size_t p = 0; p < meadow->pointCount(); ++p) {
        CHECK(std::fabs(orient[p].x) < 1e-6f && std::fabs(orient[p].z) < 1e-6f);
        CHECK(pscale[p] >= 0.7f - 1e-5f && pscale[p] <= 1.3f + 1e-5f);
        CHECK(tint[p].x > 0.8f && tint[p].x < 1.3f);
    }
    // Instances off: the clumps themselves.
    grass->setBool("instances", false);
    const GeometryPtr blades = grass->cookNode(CookContext{}, field);
    CHECK_EQ(blades->prototypeCount(), size_t(0));
    CHECK_EQ(blades->hash(), unpackInstances(*meadow)->hash());
    grass->setBool("instances", true);
    // Along the normal: on a wall, the clumps grow out of it.
    auto wall = std::make_shared<Geometry>(*ground(2.0f, 2));
    for (Vec3& p : wall->positionsForWrite()) p = Vec3(p.x, -p.z, 0.0f);  // stood up, facing +z
    grass->setFloat("maxslope", 180.0f);
    grass->setBool("alongnormal", true);
    const GeometryPtr onWall[] = {wall};
    const GeometryPtr moss = grass->cookNode(CookContext{}, onWall);
    CHECK(moss->pointCount() > 0);
    const auto places = placementsOf(*moss);
    for (const Placement& pl : places) CHECK(quatRotate(pl.orient, Vec3(0, 1, 0)).z > 0.99f);
    // ... not at all, when the grass keeps off faces that steep.
    grass->setFloat("maxslope", 45.0f);
    CHECK_EQ(grass->cookNode(CookContext{}, onWall)->pointCount(), size_t(0));
    // Only points in: a clump on each. Nothing in: one clump, at Center.
    auto dots = std::make_shared<Geometry>();
    dots->addPoints(3);
    const GeometryPtr onDots[] = {dots};
    CHECK_EQ(instanceCount(*grass->cookNode(CookContext{}, onDots)), size_t(3));
    grass->setVec3("center", Vec3(2, 0, 0));
    const GeometryPtr none[] = {nullptr};
    const GeometryPtr one = grass->cookNode(CookContext{}, none);
    CHECK_EQ(one->prototypeCount(), size_t(0));
    CHECK_EQ(one->primitiveCount(), size_t(6 * 4));
    for (const Vec3& p : one->positions()) CHECK(std::fabs(p.x - 2.0f) < 1.0f);
}

TEST(tree_instances_stand_for_the_trees_the_points_would_grow) {
    auto tree = node("tree");
    tree->setInt("levels", 1);
    tree->setInt("leaves", 4);
    tree->setInt("output", 2);
    tree->setInt("variants", 3);
    auto dots = std::make_shared<Geometry>();
    dots->addPoints(20);
    auto P = dots->positionsForWrite();
    for (size_t i = 0; i < 20; ++i) P[i] = Vec3(3.0f * static_cast<float>(i), 0.0f, 0.0f);
    const GeometryPtr in[] = {dots};
    const GeometryPtr wood = tree->cookNode(CookContext{}, in);
    CHECK_EQ(wood->pointCount(), size_t(20));
    CHECK_EQ(wood->prototypeCount(), size_t(3));
    CHECK_EQ(instanceCount(*wood), size_t(20));
    CHECK(wood->prototypes()[0]->findGroup("bark") && wood->prototypes()[0]->findGroup("leaves"));
    for (size_t i = 0; i < 20; ++i) CHECK(near(wood->positions()[i], P[i], 1e-6f));
    // A variant is the tree the point of its number grows, at the origin.
    tree->setInt("output", 0);
    const GeometryPtr none[] = {nullptr};
    const GeometryPtr first = tree->cookNode(CookContext{}, none);
    CHECK_EQ(first->pointCount(), wood->prototypes()[0]->pointCount());
    CHECK_EQ(first->hash(), wood->prototypes()[0]->hash());
    // Alone, the instance is that tree as it stands at Center.
    tree->setInt("output", 2);
    tree->setVec3("center", Vec3(1, 2, 3));
    const GeometryPtr alone = tree->cookNode(CookContext{}, none);
    CHECK_EQ(alone->pointCount(), size_t(1));
    tree->setInt("output", 0);
    const GeometryPtr mesh = tree->cookNode(CookContext{}, none);
    const auto flat = unpackInstances(*alone);
    CHECK_EQ(flat->pointCount(), mesh->pointCount());
    for (size_t p = 0; p < mesh->pointCount(); ++p) CHECK(near(flat->positions()[p], mesh->positions()[p], 1e-5f));
}

TEST(instances_to_files_copies_or_a_point_instancer) {
    TempFolder dir("instances");
    auto geo = standing({Vec3(1, 0, 0), Vec3(0, 0, 2), Vec3(4, 0, 0)}, {0, 1, 0}, {blade(), blade(Vec3(1, 0, 0))});
    geo->points().create("orient", AttrType::Vec4).write<Vec4>()[1] = yaw(1.0f);
    geo->points().find("orient")->write<Vec4>()[0] = Vec4(0, 0, 0, 1);
    geo->points().find("orient")->write<Vec4>()[2] = yaw(-0.5f);
    auto id = geo->points().create("id", AttrType::Int).write<int32_t>();
    id[0] = 7, id[1] = 8, id[2] = 9;
    // OBJ and PLY: the copies.
    const auto flat = unpackInstances(*geo);
    CHECK_EQ(io::formatObj(*geo), io::formatObj(*flat));
    CHECK_EQ(io::formatPly(*geo), io::formatPly(*flat));
    // USD: a PointInstancer of the two, each once.
    const std::string text = io::usda::geometryStage(*geo, "field").text();
    CHECK(text.find("def PointInstancer \"instances\"") != std::string::npos);
    CHECK(text.find("rel prototypes = [</field/instances/Prototypes/proto_0>, </field/instances/Prototypes/proto_1>]") !=
          std::string::npos);
    CHECK(text.find("int[] protoIndices = [0, 1, 0]") != std::string::npos);
    CHECK(text.find("int64[] ids = [7, 8, 9]") != std::string::npos);
    CHECK(text.find("point3f[] positions = [(1, 0, 0), (0, 0, 2), (4, 0, 0)]") != std::string::npos);
    CHECK(text.find("float3[] scales = [(1, 1, 1), (1, 1, 1), (1, 1, 1)]") != std::string::npos);
    CHECK(text.find("quath[] orientations = [(1, 0, 0, 0), ") != std::string::npos);
    CHECK(text.find("def Points") == std::string::npos);  // the instances' points are not points too
    size_t meshes = 0;
    for (size_t at = text.find("def Mesh"); at != std::string::npos; at = text.find("def Mesh", at + 1)) ++meshes;
    CHECK_EQ(meshes, size_t(2));  // a blade each, not three

    // Merged with ground that had no normals: none zero written for it.
    Geometry field = *ground(2.0f, 2);
    auto dots = std::make_shared<Geometry>(*geo);
    dots->points().create("N", AttrType::Vec3).write<Vec3>()[0] = Vec3(0, 1, 0);
    field.append(*dots);
    const std::string merged = io::usda::geometryStage(field, "field").text();
    CHECK(merged.find("normals") == std::string::npos);
    CHECK(merged.find("(0, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0)]") == std::string::npos);

    // A shot: the prototypes once, in the stage; where the instances turn, a
    // layer a frame.
    sim::UsdExport usd(dir / "shot.usda", "meadow");
    std::string error;
    for (int f = 1; f <= 3; ++f) {
        auto moving = std::make_shared<Geometry>(*geo);
        moving->points().find("orient")->write<Vec4>()[0] = yaw(0.1f * static_cast<float>(f));
        sim::Frame frame;
        frame.number = f;
        CHECK(usd.add(frame, moving, nullptr, sim::Look(), error));
    }
    CHECK(usd.finish(error));
    const std::string stage = fileText(dir / "shot.usda");
    CHECK(stage.find("def PointInstancer \"instances\"") != std::string::npos);
    CHECK(stage.find("rel prototypes = [</World/meadow/instances/Prototypes/proto_0>") != std::string::npos);
    CHECK(stage.find("quath[] orientations\n") != std::string::npos);  // declared; the layers have the values
    const std::string two = fileText(dir / "shot_frames/shot.0002.usda");
    CHECK(two.find("quath[] orientations.timeSamples") != std::string::npos);
    CHECK(two.find("def Mesh") == std::string::npos && two.find("Prototypes") == std::string::npos);
    CHECK(fileText(dir / "shot_frames/shot.manifest.usda").find("int[] protoIndices") != std::string::npos);
}

TEST(display_draws_what_stands_on_the_points_instanced) {
    auto geo = standing({Vec3(1, 0, 0), Vec3(0, 0, 2), Vec3(5, 5, 5)}, {0, 0, -1}, {blade()});
    auto pscale = geo->points().create("pscale", AttrType::Float).write<float>();
    pscale[0] = 1.0f;
    pscale[1] = 3.0f;
    const DisplayInstances drawn = instancesOf(*geo);
    CHECK_EQ(drawn.prototypes.size(), size_t(1));
    CHECK_EQ(drawn.count(), size_t(2));
    const auto& f = drawn.placements[0];
    CHECK_EQ(f[4 * 0 + 0], 1.0f);
    CHECK_EQ(f[DisplayInstances::kFloats + 3], 3.0f);  // its size
    CHECK_EQ(f[DisplayInstances::kFloats + 11], 1.0f);
    CHECK(near(drawn.lo, Vec3(-0.3f, 0.0f, 0.0f), 1e-5f));
    CHECK(near(drawn.hi, Vec3(1.1f, 3.0f, 2.0f), 1e-5f));
    // The instances are not dots; the loose point is.
    const DisplayGeometry shown = displayOf(*geo);
    CHECK_EQ(shown.dotCount(), size_t(1));
    CHECK_EQ(shown.triangleCount(), size_t(0));
    // Ground merged with points that carry N: its own normals are made, not
    // the zeros the merge gave it.
    Geometry field = *ground(2.0f, 2);
    auto up = std::make_shared<Geometry>(*geo);
    auto N = up->points().create("N", AttrType::Vec3).write<Vec3>();
    std::fill(N.begin(), N.end(), Vec3(1, 0, 0));
    field.append(*up);
    const DisplayGeometry g = displayOf(field);
    CHECK(g.triangleCount() == 8);
    for (size_t t = 0; t < g.triangleCount() * 3; ++t) CHECK(g.triangles[t * 9 + 4] > 0.99f);  // facing up
}

TEST(meadow_example_the_wind_turns_only_the_points) {
    Network meadow;
    CHECK(Network::example("meadow", meadow));
    int grass = -1, wind = -1, spots = -1, shrubSpots = -1, land = -1;
    for (const auto& n : meadow.nodes()) {
        if (n.name == "grass") grass = n.id;
        if (n.name == "wind") wind = n.id;
        if (n.name == "tree_spots") spots = n.id;
        if (n.name == "shrub_spots") shrubSpots = n.id;
        if (n.name == "land") land = n.id;
    }
    CHECK(grass >= 0 && wind >= 0 && spots >= 0 && shrubSpots >= 0 && land >= 0);
    // A small piece of it, a few plants.
    CHECK(meadow.setParam(land, "sizex", "12"));
    CHECK(meadow.setParam(land, "sizez", "12"));
    CHECK(meadow.setParam(land, "center", "0 0 -6"));
    CHECK(meadow.setParam(grass, "density", "5"));
    GeometryGraph g;
    g.sync(meadow);
    const GeometryPtr a = g.cook(wind, 1), b = g.cook(wind, 30);
    CHECK(a && b && a->pointCount() == b->pointCount() && a->pointCount() > 100);
    // The eight clumps, and those of them bent ahead (Plant Wind).
    CHECK(a->prototypeCount() >= size_t(8) && b->prototypeCount() >= size_t(8));
    for (size_t k = 0; k < 8; ++k) CHECK(a->prototypes()[k] == b->prototypes()[k]);  // grown once
    CHECK(a->positions().data() == b->positions().data());  // the places shared, not copied
    const auto oa = a->points().find("orient")->read<Vec4>(), ob = b->points().find("orient")->read<Vec4>();
    float turned = 0.0f;
    for (size_t p = 0; p < oa.size(); ++p) {
        turned = std::max(turned, std::fabs(oa[p].x - ob[p].x) + std::fabs(oa[p].z - ob[p].z));
        CHECK_NEAR(ob[p].x * ob[p].x + ob[p].y * ob[p].y + ob[p].z * ob[p].z + ob[p].w * ob[p].w, 1.0, 1e-4);
    }
    CHECK(turned > 0.02f);
    // The whole of it: the ground, the grass, the trees and the shrubs.
    const GeometryPtr all = g.cook(meadow.displayed(), 1);
    CHECK(all != nullptr);
    CHECK(all->prototypeCount() >= size_t(8 + 5 + 4 + 4));
    CHECK(instanceCount(*all) > 100);
}
