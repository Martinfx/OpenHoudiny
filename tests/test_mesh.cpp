//
// Meshes from files (src/pg/sim/Mesh.h): OBJ read in all its face forms,
// the baked distance field against a box whose distance is known, inside
// and outside -- also for a mesh with a hole --, rays, a mesh placed in the
// world as a Shape::Mesh, and the cache that reads a file once.
//
#include "pg/sim/Mesh.h"
#include "pg/sim/Shape.h"

#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace pg;
using namespace pg::sim;

namespace {

/// A cube from -1 to 1: eight corners, six faces as quads.
const char* kCube =
    "# a cube\r\n"
    "o cube\n"
    "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
    "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
    "vn 0 0 -1\n"
    "vt 0 0\n"
    "usemtl stone\n"
    "s off\n"
    "f 1 4 3 2\n"        // -z
    "f 5/1 6/1 7/1 8/1\n" // +z
    "f 1//1 2//1 6//1 5//1\n"  // -y
    "f 4/1/1 8/1/1 7/1/1 3/1/1\n"  // +y
    "f -8 -4 -1 -5\n"    // -x: 1 5 8 4, counted from the end
    "f 2 3 7 6\n";       // +x

float boxDistance(const Vec3& p, float half) {
    const Vec3 d(std::fabs(p.x) - half, std::fabs(p.y) - half, std::fabs(p.z) - half);
    const float outside = length(Vec3(std::max(d.x, 0.0f), std::max(d.y, 0.0f), std::max(d.z, 0.0f)));
    return outside + std::min(std::max({d.x, d.y, d.z}), 0.0f);
}

}  // namespace

TEST(mesh_obj_reads_every_form_of_face) {
    TriangleMesh m;
    std::string error;
    CHECK(parseObj(kCube, m, error));
    CHECK_EQ(m.positions.size(), size_t(8));
    CHECK_EQ(m.triangles.size(), size_t(12));  // six quads, two triangles each
    // "f -8 -4 -1 -5" is 1 5 8 4.
    CHECK_EQ(m.triangles[8][0], uint32_t(0));
    CHECK_EQ(m.triangles[8][1], uint32_t(4));
    CHECK_EQ(m.triangles[8][2], uint32_t(7));
    Vec3 lo, hi;
    m.bounds(lo, hi);
    CHECK(lo == Vec3(-1.0f) && hi == Vec3(1.0f));

    // Numbers as C writes them, whatever the locale.
    CHECK(parseObj("v 1.5e-1 -2.25 +3\nv 0 0 0\nv 1 0 0\nf 1 2 3\n", m, error));
    CHECK(std::fabs(m.positions[0].x - 0.15f) < 1e-7f && m.positions[0].y == -2.25f && m.positions[0].z == 3.0f);
    // A pentagon becomes a fan of three.
    CHECK(parseObj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0.5 1.5 0\nv 0 1 0\nf 1 2 3 4 5\n", m, error));
    CHECK_EQ(m.triangles.size(), size_t(3));

    TriangleMesh untouched = m;
    CHECK(!parseObj("v 0 0 0\nv 1 0 0\nf 1 2 3\n", m, error));
    CHECK(error.find("line 3") != std::string::npos && error.find("no vertex 3") != std::string::npos);
    CHECK(!parseObj("v 0 0 0\nv 1 0 x\n", m, error));
    CHECK(!parseObj("v 0 0 0\n# nothing else\n", m, error));
    CHECK(error == "no faces");
    CHECK_EQ(m.triangles.size(), untouched.triangles.size());  // a failed read leaves it
}

TEST(mesh_distance_field_matches_the_box) {
    TriangleMesh m;
    std::string error;
    CHECK(parseObj(kCube, m, error));
    const MeshShape cube(m, 32);
    CHECK(cube.center() == Vec3());
    CHECK(cube.half() == Vec3(1.0f));
    const float cell = cube.cell();
    CHECK(std::fabs(cell - 2.0f / 32.0f) < 1e-6f);
    // Inside and out, near the faces and far: as the box's own distance,
    // within a cell (trilinear between grid points).
    const Vec3 points[] = {Vec3(), Vec3(0.5f, 0.2f, -0.3f), Vec3(0.95f, 0.0f, 0.0f), Vec3(1.1f, 0.0f, 0.0f),
                           Vec3(1.3f, 1.3f, 0.0f), Vec3(-0.2f, -1.2f, 0.4f), Vec3(0.0f, 0.0f, -1.2f)};
    for (const Vec3& p : points) {
        const float want = boxDistance(p, 1.0f), got = cube.distance(p);
        if (std::fabs(got - want) > cell) {
            ::testing::fail(__FILE__, __LINE__, "at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " +
                                                    std::to_string(p.z) + ": " + std::to_string(got) + ", not " +
                                                    std::to_string(want));
        }
        CHECK((got < 0.0f) == (want < 0.0f));
    }
    // Far beyond the grid: still the distance, roughly.
    CHECK(std::fabs(cube.distance(Vec3(10.0f, 0.0f, 0.0f)) - 9.0f) < 0.2f);
}

TEST(mesh_with_a_hole_is_still_inside_out_right) {
    // The cube without its top: rays along y see it wrongly above it, rays
    // along x and z do not -- two votes of three keep the inside inside and
    // the outside out.
    const std::string open = std::string(kCube).substr(0, std::string(kCube).find("f 4/1/1")) + "f -8 -4 -1 -5\nf 2 3 7 6\n";
    TriangleMesh m;
    std::string error;
    CHECK(parseObj(open, m, error));
    CHECK_EQ(m.triangles.size(), size_t(10));
    const MeshShape box(m, 24);
    CHECK(box.distance(Vec3()) < 0.0f);
    CHECK(box.distance(Vec3(0.2f, -0.5f, 0.3f)) < 0.0f);
    CHECK(box.distance(Vec3(0.0f, 1.3f, 0.0f)) > 0.0f);   // above the hole
    CHECK(box.distance(Vec3(1.4f, 0.0f, 0.0f)) > 0.0f);
    CHECK(box.distance(Vec3(0.0f, -1.3f, 0.2f)) > 0.0f);
}

TEST(mesh_rays_meet_the_triangles) {
    TriangleMesh m;
    std::string error;
    CHECK(parseObj(kCube, m, error));
    const MeshShape cube(m, 16);
    float t = 0.0f;
    Vec3 n;
    CHECK(cube.intersect(Vec3(-5.0f, 0.3f, 0.2f), Vec3(1.0f, 0.0f, 0.0f), 0.0f, t, n));
    CHECK(std::fabs(t - 4.0f) < 1e-5f);
    CHECK(n == Vec3(-1.0f, 0.0f, 0.0f));  // facing the ray
    CHECK(cube.intersect(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 2.0f, 0.0f), 0.0f, t, n));  // from inside, out
    CHECK(std::fabs(t - 0.5f) < 1e-5f);
    CHECK(!cube.intersect(Vec3(-5.0f, 1.5f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 0.0f, t, n));
    CHECK(!cube.intersect(Vec3(5.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 0.0f, t, n));  // going away
}

TEST(mesh_placed_in_the_world_as_a_shape) {
    TriangleMesh m;
    std::string error;
    CHECK(parseObj(kCube, m, error));
    auto cube = std::make_shared<MeshShape>(m, 32);
    // The 2 x 2 x 2 cube as a 0.5 x 0.2 x 0.5 slab at (1, 0.1, 0), turned 45
    // degrees about y.
    const ShapeInstance slab(Shape::Mesh, Vec3(1.0f, 0.1f, 0.0f), Vec3(0.0f, 45.0f, 0.0f), Vec3(0.5f, 0.2f, 0.5f), cube);
    CHECK(slab.shape() == Shape::Mesh && slab.mesh() == cube.get());
    CHECK(slab.contains(Vec3(1.0f, 0.1f, 0.0f)));
    CHECK(slab.contains(Vec3(1.3f, 0.1f, 0.0f)));    // along the diagonal, a corner reaches 0.35
    CHECK(!slab.contains(Vec3(1.3f, 0.1f, 0.3f)));
    CHECK(!slab.contains(Vec3(1.0f, 0.25f, 0.0f)));  // above it
    // A ray down onto it, in world units.
    float t = 0.0f;
    Vec3 n;
    CHECK(slab.intersect(Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 0.0f, t, n));
    CHECK(std::fabs(t - 1.8f) < 1e-4f);
    CHECK(std::fabs(n.y - 1.0f) < 1e-4f);
    // Distances in world units, near the surface.
    CHECK(std::fabs(slab.distance(Vec3(1.0f, 0.3f, 0.0f)) - 0.1f) < 0.02f);
    // Without its mesh it is its box.
    const ShapeInstance lost(Shape::Mesh, Vec3(), Vec3(), Vec3(1.0f));
    CHECK(lost.shape() == Shape::Box && lost.contains(Vec3(0.4f, 0.4f, 0.4f)));
}

TEST(mesh_files_are_read_once) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pg_test_mesh";
    fs::create_directories(dir);
    const std::string path = (dir / "cube.obj").string();
    {
        std::ofstream out(path, std::ios::binary);
        out << kCube;
    }
    std::string error;
    const auto a = loadMesh(path, error);
    const auto b = loadMesh(path, error);
    CHECK(a && a == b);
    CHECK_EQ(a->mesh().triangles.size(), size_t(12));
    CHECK_EQ(a->path, path);
    // A file that changed is read again.
    {
        std::ofstream out(path, std::ios::binary);
        out << kCube << "v 5 5 5\nv 6 5 5\nv 5 6 5\nf -3 -2 -1\n";
    }
    const auto c = loadMesh(path, error);
    CHECK(c && c != a && c->mesh().triangles.size() == size_t(13));
    CHECK(!loadMesh((dir / "nothing.obj").string(), error));
    CHECK(error.find("no such file") != std::string::npos);
    fs::remove_all(dir);
}
