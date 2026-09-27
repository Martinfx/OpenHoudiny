//
// Shapes placed in the world (src/pg/sim/Shape.h): rotations and their
// angles, inside and outside, distances, where rays meet the surface, and
// the falloff sources emit with.
//
#include "pg/sim/Shape.h"

#include "test_framework.h"

#include <cmath>
#include <cstdint>

using namespace pg;
using namespace pg::sim;

namespace {

bool near(const Vec3& a, const Vec3& b, float tolerance) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

/// Reproducible numbers in [0, 1).
struct Random {
    uint32_t state = 12345u;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
    float between(float lo, float hi) { return lo + (hi - lo) * next(); }
};

}  // namespace

TEST(shape_rotations_go_to_angles_and_back) {
    // No turn is exactly no turn: an unturned shape costs no rounding.
    const Rotation none = Rotation::fromEuler(Vec3());
    CHECK(none.x == Vec3(1, 0, 0) && none.y == Vec3(0, 1, 0) && none.z == Vec3(0, 0, 1));
    CHECK(none.inverse(Vec3(0.3f, -0.7f, 1.1f)) == Vec3(0.3f, -0.7f, 1.1f));

    // A quarter turn about z takes x to y; about y, z to x; about x, y to z.
    CHECK(near(Rotation::fromEuler(Vec3(0, 0, 90)).apply(Vec3(1, 0, 0)), Vec3(0, 1, 0), 1e-6f));
    CHECK(near(Rotation::fromEuler(Vec3(0, 90, 0)).apply(Vec3(0, 0, 1)), Vec3(1, 0, 0), 1e-6f));
    CHECK(near(Rotation::fromEuler(Vec3(90, 0, 0)).apply(Vec3(0, 1, 0)), Vec3(0, 0, 1), 1e-6f));
    // x first, then z: (90, 0, 90) takes y to z (x's turn) and z stays.
    CHECK(near(Rotation::fromEuler(Vec3(90, 0, 90)).apply(Vec3(0, 1, 0)), Vec3(0, 0, 1), 1e-6f));
    CHECK(near(Rotation::fromEuler(Vec3(90, 0, 90)).apply(Vec3(1, 0, 0)), Vec3(0, 1, 0), 1e-6f));

    Random r;
    for (int i = 0; i < 200; ++i) {
        const Vec3 angles(r.between(-179, 179), r.between(-89, 89), r.between(-179, 179));
        const Rotation turn = Rotation::fromEuler(angles);
        // Orthonormal.
        CHECK(std::fabs(length(turn.x) - 1.0f) < 1e-5f && std::fabs(dot(turn.x, turn.y)) < 1e-5f);
        CHECK(near(cross(turn.x, turn.y), turn.z, 1e-5f));
        // The angles come back, and so does the rotation.
        const Vec3 back = turn.toEuler(angles);
        CHECK(near(back, angles, 2e-3f));
        const Rotation again = Rotation::fromEuler(turn.toEuler());
        CHECK(near(again.x, turn.x, 1e-4f) && near(again.y, turn.y, 1e-4f) && near(again.z, turn.z, 1e-4f));
        // Undone by its inverse.
        const Vec3 v(r.between(-1, 1), r.between(-1, 1), r.between(-1, 1));
        CHECK(near(turn.inverse(turn.apply(v)), v, 1e-5f));
    }
}

TEST(shape_turning_past_half_a_round_keeps_counting) {
    // A gizmo turns a shape about z by 10 degrees at a time: the angle it
    // shows goes on growing past 180 rather than jumping to -170.
    Vec3 angles(0, 0, 170);
    for (int step = 0; step < 4; ++step) {
        const Rotation turned = Rotation::about(Vec3(0, 0, 1), 10.0f).then(Rotation::fromEuler(angles));
        angles = turned.toEuler(angles);
    }
    CHECK(near(angles, Vec3(0, 0, 210), 1e-3f));
    // The same about x, where the other set of angles is the nearer one.
    angles = Vec3(170, 0, 0);
    for (int step = 0; step < 3; ++step) {
        angles = Rotation::about(Vec3(1, 0, 0), 10.0f).then(Rotation::fromEuler(angles)).toEuler(angles);
    }
    const Rotation expected = Rotation::fromEuler(Vec3(200, 0, 0));
    const Rotation got = Rotation::fromEuler(angles);
    CHECK(near(got.y, expected.y, 1e-4f) && near(got.z, expected.z, 1e-4f));
    CHECK(std::fabs(angles.x - 200.0f) < 1e-2f || std::fabs(angles.y - 180.0f) < 1e-2f);
    // A quarter turn about y locks x and z together: still the same rotation.
    const Rotation locked = Rotation::fromEuler(Vec3(30, 90, 0));
    const Rotation relocked = Rotation::fromEuler(locked.toEuler(Vec3(0, 80, 0)));
    CHECK(near(relocked.x, locked.x, 1e-4f) && near(relocked.y, locked.y, 1e-4f));
}

TEST(shape_insides_and_distances) {
    // A ball 0.4 across at (1, 2, 3): the distance is exact.
    const ShapeInstance ball(Shape::Sphere, Vec3(1, 2, 3), Vec3(), Vec3(0.4f));
    CHECK(ball.contains(Vec3(1.1f, 2.0f, 3.0f)));
    CHECK(!ball.contains(Vec3(1.25f, 2.0f, 3.0f)));
    CHECK(std::fabs(ball.distance(Vec3(1.5f, 2.0f, 3.0f)) - 0.3f) < 1e-6f);
    CHECK(std::fabs(ball.distance(Vec3(1.0f, 2.0f, 3.0f)) + 0.2f) < 1e-6f);
    CHECK(near(ball.normal(Vec3(1.0f, 2.5f, 3.0f)), Vec3(0, 1, 0), 1e-3f));

    // A box 1 x 0.2 x 0.2, turned a quarter about z: it stands upright.
    const ShapeInstance post(Shape::Box, Vec3(0, 1, 0), Vec3(0, 0, 90), Vec3(1.0f, 0.2f, 0.2f));
    CHECK(post.contains(Vec3(0.0f, 1.45f, 0.0f)));
    CHECK(!post.contains(Vec3(0.45f, 1.0f, 0.0f)));
    CHECK(std::fabs(post.distance(Vec3(0.3f, 1.0f, 0.0f)) - 0.2f) < 1e-5f);
    Vec3 lo, hi;
    post.bounds(lo, hi);
    CHECK(near(lo, Vec3(-0.1f, 0.5f, -0.1f), 1e-5f) && near(hi, Vec3(0.1f, 1.5f, 0.1f), 1e-5f));

    // A stretched ball: inside and outside exact, the distance close.
    const ShapeInstance egg(Shape::Sphere, Vec3(), Vec3(), Vec3(2.0f, 1.0f, 1.0f));
    CHECK(egg.contains(Vec3(0.95f, 0.0f, 0.0f)) && !egg.contains(Vec3(1.05f, 0.0f, 0.0f)));
    CHECK(egg.contains(Vec3(0.0f, 0.45f, 0.0f)) && !egg.contains(Vec3(0.0f, 0.55f, 0.0f)));
    CHECK(std::fabs(egg.distance(Vec3(1.2f, 0.0f, 0.0f)) - 0.2f) < 0.02f);

    // A column 0.5 across and 1 tall; a cone with its apex up; a ring.
    const ShapeInstance column(Shape::Cylinder, Vec3(0, 0.5f, 0), Vec3(), Vec3(0.5f, 1.0f, 0.5f));
    CHECK(column.contains(Vec3(0.2f, 0.95f, 0.0f)) && !column.contains(Vec3(0.2f, 1.05f, 0.0f)));
    CHECK(!column.contains(Vec3(0.2f, 0.5f, 0.2f)));  // 0.28 from the axis
    CHECK(std::fabs(column.distance(Vec3(0.5f, 0.5f, 0.0f)) - 0.25f) < 1e-5f);
    CHECK(std::fabs(column.distance(Vec3(0.0f, 1.3f, 0.0f)) - 0.3f) < 1e-5f);

    const ShapeInstance cone(Shape::Cone, Vec3(0, 0.5f, 0), Vec3(), Vec3(1.0f, 1.0f, 1.0f));
    CHECK(cone.contains(Vec3(0.45f, 0.02f, 0.0f)));   // near the base rim
    CHECK(!cone.contains(Vec3(0.3f, 0.9f, 0.0f)));    // near the apex it is thin
    CHECK(cone.contains(Vec3(0.0f, 0.95f, 0.0f)));
    CHECK(!cone.contains(Vec3(0.0f, -0.01f, 0.0f)));
    CHECK(cone.distance(Vec3(0.0f, 0.3f, 0.0f)) < 0.0f && cone.distance(Vec3(0.0f, 1.2f, 0.0f)) > 0.19f);

    const ShapeInstance ring(Shape::Torus, Vec3(), Vec3(), Vec3(1.0f, 0.2f, 1.0f));  // tube 0.1, centre line 0.4
    CHECK(ring.contains(Vec3(0.4f, 0.0f, 0.0f)) && ring.contains(Vec3(0.0f, 0.05f, -0.45f)));
    CHECK(!ring.contains(Vec3()));  // the hole
    CHECK(std::fabs(ring.tube() - 0.1f) < 1e-6f && std::fabs(ring.ring() - 0.4f) < 1e-6f);
    CHECK(std::fabs(ring.distance(Vec3(0.0f, 0.0f, 0.0f)) - 0.3f) < 1e-5f);
    // Stood on its edge by a quarter turn about x: the hole looks along z.
    const ShapeInstance wheel(Shape::Torus, Vec3(), Vec3(90, 0, 0), Vec3(1.0f, 0.2f, 1.0f));
    CHECK(wheel.contains(Vec3(0.0f, 0.4f, 0.0f)) && !wheel.contains(Vec3(0.0f, 0.0f, 0.4f)));
}

TEST(shape_rays_meet_the_surfaces) {
    struct Case {
        ShapeInstance shape;
        Vec3 origin, dir;
        float t;      // expected, or < 0 for a miss
        Vec3 normal;
    };
    const Case cases[] = {
        {ShapeInstance(Shape::Sphere, Vec3(0, 1, 0), Vec3(), Vec3(0.5f)), Vec3(-2, 1, 0), Vec3(1, 0, 0), 1.75f, Vec3(-1, 0, 0)},
        {ShapeInstance(Shape::Sphere, Vec3(0, 1, 0), Vec3(), Vec3(0.5f)), Vec3(-2, 1.3f, 0), Vec3(1, 0, 0), -1.0f, Vec3()},
        // A direction twice as long: t halves.
        {ShapeInstance(Shape::Sphere, Vec3(0, 1, 0), Vec3(), Vec3(0.5f)), Vec3(-2, 1, 0), Vec3(2, 0, 0), 0.875f, Vec3(-1, 0, 0)},
        {ShapeInstance(Shape::Box, Vec3(), Vec3(), Vec3(1, 2, 1)), Vec3(0, 5, 0), Vec3(0, -1, 0), 4.0f, Vec3(0, 1, 0)},
        // Turned 45 degrees about y: the corner comes first, at sqrt(2)/2.
        {ShapeInstance(Shape::Box, Vec3(), Vec3(0, 45, 0), Vec3(1)), Vec3(-3, 0, 0), Vec3(1, 0, 0), 3.0f - 0.70710678f, Vec3()},
        {ShapeInstance(Shape::Cylinder, Vec3(), Vec3(), Vec3(1, 2, 1)), Vec3(-3, 0.5f, 0), Vec3(1, 0, 0), 2.5f, Vec3(-1, 0, 0)},
        {ShapeInstance(Shape::Cylinder, Vec3(), Vec3(), Vec3(1, 2, 1)), Vec3(0.2f, 4, 0), Vec3(0, -1, 0), 3.0f, Vec3(0, 1, 0)},
        {ShapeInstance(Shape::Cylinder, Vec3(), Vec3(), Vec3(1, 2, 1)), Vec3(-3, 1.5f, 0), Vec3(1, 0, 0), -1.0f, Vec3()},
        // The cone's base, from below, and its side halfway up.
        {ShapeInstance(Shape::Cone, Vec3(), Vec3(), Vec3(2, 2, 2)), Vec3(0.3f, -3, 0), Vec3(0, 1, 0), 2.0f, Vec3(0, -1, 0)},
        {ShapeInstance(Shape::Cone, Vec3(), Vec3(), Vec3(2, 2, 2)), Vec3(-3, 0, 0), Vec3(1, 0, 0), 2.5f, Vec3()},
        // Through the ring's hole: a miss; onto the tube: a hit.
        {ShapeInstance(Shape::Torus, Vec3(), Vec3(), Vec3(1, 0.2f, 1)), Vec3(0, 3, 0), Vec3(0, -1, 0), -1.0f, Vec3()},
        {ShapeInstance(Shape::Torus, Vec3(), Vec3(), Vec3(1, 0.2f, 1)), Vec3(0.4f, 3, 0), Vec3(0, -1, 0), 2.9f, Vec3(0, 1, 0)},
        {ShapeInstance(Shape::Torus, Vec3(), Vec3(), Vec3(1, 0.2f, 1)), Vec3(-3, 0, 0), Vec3(1, 0, 0), 2.5f, Vec3(-1, 0, 0)},
    };
    int index = 0;
    for (const Case& c : cases) {
        float t = 0.0f;
        Vec3 n;
        const bool hit = c.shape.intersect(c.origin, c.dir, 0.0f, t, n);
        if (c.t < 0.0f) {
            if (hit) ::testing::fail(__FILE__, __LINE__, "case " + std::to_string(index) + " should miss");
        } else {
            if (!hit || std::fabs(t - c.t) > 2e-3f) {
                ::testing::fail(__FILE__, __LINE__, "case " + std::to_string(index) + ": t = " + std::to_string(t));
            }
            if (length(c.normal) > 0.0f && !near(n, c.normal, 2e-2f)) {
                ::testing::fail(__FILE__, __LINE__, "case " + std::to_string(index) + ": wrong normal");
            }
            // The point is on the surface.
            CHECK(std::fabs(c.shape.distance(c.origin + c.dir * t)) < 3e-3f);
        }
        ++index;
    }
    // From inside, the way out.
    float t = 0.0f;
    Vec3 n;
    const ShapeInstance ball(Shape::Sphere, Vec3(), Vec3(), Vec3(2));
    CHECK(ball.intersect(Vec3(), Vec3(0, 0, 1), 0.0f, t, n));
    CHECK(std::fabs(t - 1.0f) < 1e-5f && near(n, Vec3(0, 0, 1), 1e-4f));
}

TEST(shape_sources_ease_out_at_their_surface) {
    // A ball and a box emit as they always did: the ball over its outer 40 %,
    // the box over the outer quarter of each half edge.
    const ShapeInstance ball(Shape::Sphere, Vec3(0, 0.2f, 0), Vec3(), Vec3(0.2f));
    CHECK_EQ(ball.falloff(Vec3(0.0f, 0.2f, 0.0f)), 1.0f);
    CHECK_EQ(ball.falloff(Vec3(0.05f, 0.2f, 0.0f)), 1.0f);
    CHECK(ball.falloff(Vec3(0.08f, 0.2f, 0.0f)) > 0.0f && ball.falloff(Vec3(0.08f, 0.2f, 0.0f)) < 1.0f);
    CHECK_EQ(ball.falloff(Vec3(0.1f, 0.2f, 0.0f)), 0.0f);
    const ShapeInstance box(Shape::Box, Vec3(), Vec3(), Vec3(0.4f, 0.2f, 0.2f));
    CHECK_EQ(box.falloff(Vec3(0.14f, 0.0f, 0.0f)), 1.0f);
    CHECK(box.falloff(Vec3(0.18f, 0.0f, 0.0f)) < 1.0f);
    CHECK_EQ(box.falloff(Vec3(0.0f, 0.11f, 0.0f)), 0.0f);
    // Every shape: nothing outside, all of it deep inside.
    Random r;
    for (const Shape s : {Shape::Sphere, Shape::Box, Shape::Cylinder, Shape::Cone, Shape::Torus}) {
        const ShapeInstance shape(s, Vec3(0.1f, 0.5f, -0.2f), Vec3(20, 30, 40), Vec3(0.6f, 0.4f, 0.5f));
        Vec3 lo, hi;
        shape.bounds(lo, hi);
        for (int i = 0; i < 400; ++i) {
            const Vec3 p(r.between(lo.x - 0.1f, hi.x + 0.1f), r.between(lo.y - 0.1f, hi.y + 0.1f),
                         r.between(lo.z - 0.1f, hi.z + 0.1f));
            const float w = shape.falloff(p);
            CHECK(w >= 0.0f && w <= 1.0f);
            if (!shape.contains(p)) CHECK(w < 1e-4f);  // on the surface, rounding may tip either way
            // Inside, it lies within its bounds.
            if (shape.contains(p)) CHECK(p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x <= hi.x && p.y <= hi.y && p.z <= hi.z);
        }
        CHECK(shape.falloff(shape.toWorld(s == Shape::Torus ? Vec3(shape.ring(), 0, 0) : Vec3(0, s == Shape::Cone ? -0.1f : 0.0f, 0))) > 0.9f);
    }
}
