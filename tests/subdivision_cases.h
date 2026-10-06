#pragma once
//
// The meshes of tests/data/subdivision/subdivision.txt and the points
// Blender's Subdivision Surface -- OpenSubdiv -- makes of them
// (make_subdivision.py): what Subdivide and USD Import are held to.
//
#include "pg/core/Types.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace opensubdiv {

using pg::Vec3;

struct Case {
    std::string name;
    int levels = 0;
    bool edgeOnly = false;  ///< the boundary's corners smooth too
    std::vector<Vec3> points;
    std::vector<std::vector<uint32_t>> faces;
    std::vector<std::tuple<uint32_t, uint32_t, float>> edges;  ///< the sharp ones: their ends, how sharp
    std::vector<std::pair<uint32_t, float>> corners;           ///< the sharp points
    std::vector<Vec3> result;                                  ///< Blender's, in its order
};

inline std::vector<Case> cases() {
    std::ifstream in(PG_TEST_DATA_DIR "/subdivision/subdivision.txt");
    std::vector<Case> all;
    std::string word, boundary;
    size_t n = 0;
    while (in >> word && word == "case") {
        Case c;
        in >> c.name >> c.levels >> boundary;
        c.edgeOnly = boundary == "edgeOnly";
        in >> word >> n;
        c.points.resize(n);
        for (Vec3& p : c.points) in >> p.x >> p.y >> p.z;
        in >> word >> n;
        c.faces.resize(n);
        for (auto& f : c.faces) {
            size_t k = 0;
            in >> k;
            f.resize(k);
            for (uint32_t& i : f) in >> i;
        }
        in >> word >> n;
        c.edges.resize(n);
        for (auto& [a, b, s] : c.edges) in >> a >> b >> s;
        in >> word >> n;
        c.corners.resize(n);
        for (auto& [i, s] : c.corners) in >> i >> s;
        in >> word >> n;
        c.result.resize(n);
        for (Vec3& p : c.result) in >> p.x >> p.y >> p.z;
        all.push_back(std::move(c));
    }
    return all;
}

/// How far the furthest point of `a` is from the nearest of `b`.
template <class A, class B>
float furthestFrom(const A& a, const B& b) {
    float worst = 0.0f;
    for (const Vec3& p : a) {
        float best = 1e30f;
        for (const Vec3& q : b) best = std::min(best, glm::length(p - q));
        worst = std::max(worst, best);
    }
    return worst;
}

}  // namespace opensubdiv
