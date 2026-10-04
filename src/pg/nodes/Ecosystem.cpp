// Ecosystem: a plant community grown over years (pg/core/Ecosystem.h).
//
//   places   points where a plant could stand -- a Scatter over a terrain,
//            as dense as plants could be -- each as wet as its attribute
//            Moisture Attribute says (0 dry, 1 wet; none: 0.5).
//   output   the plants alive after Years: a point each, on its place --
//            species (0, 1, 2), a group of each (species1, species2,
//            species3), age, pscale (how grown: 0.15 a seedling, 1 grown),
//            orient (turned about +y as it happens), id (its place) --
//            for Tree or Copy to Points to grow a kind on each group.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Ecosystem.h"
#include "pg/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

class EcosystemNode : public Node {
public:
    explicit EcosystemNode(std::string name) : Node("ecosystem", std::move(name)) {
        setInputCount(1);
        params_.setInt("years", 80);
        params_.setInt("seed", 1);
        params_.setFloat("start", 0.02f);
        params_.setString("moistureattribute", "moisture");
        // Three kinds: a pioneer that grows fast and dies young and bears
        // no shade (a birch), a slow giant of the dry (an oak), a shade
        // bearer of the wet (a spruce, a beech).
        const Species pioneer{1.0f, 2.5f, 10.0f, 60.0f, 0.1f, 0.45f, 0.45f, 12.0f, 1.2f};
        const Species giant{0.6f, 5.0f, 40.0f, 300.0f, 0.35f, 0.3f, 0.3f, 6.0f, 0.4f};
        const Species bearer{0.6f, 3.0f, 30.0f, 200.0f, 0.85f, 0.75f, 0.3f, 7.0f, 0.6f};
        const Species kinds[3] = {pioneer, giant, bearer};
        for (int k = 0; k < 3; ++k) {
            const std::string p = "s" + std::to_string(k + 1) + "_";
            const Species& d = kinds[k];
            params_.setBool(p + "on", true);
            params_.setFloat(p + "share", d.share);
            params_.setFloat(p + "crown", d.crown);
            params_.setFloat(p + "growth", d.growth);
            params_.setFloat(p + "life", d.life);
            params_.setFloat(p + "shade", d.shade);
            params_.setFloat(p + "moisture", d.moisture);
            params_.setFloat(p + "tolerance", d.tolerance);
            params_.setFloat(p + "seeding", d.seeding);
            params_.setFloat(p + "seeds", d.seeds);
        }
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> inputs) override {
        auto out = std::make_shared<Geometry>();
        if (inputs.empty() || !inputs[0] || inputs[0]->pointCount() == 0) return out;
        const Geometry& in = *inputs[0];
        EcosystemSettings s;
        s.years = std::clamp(params_.evalInt("years", ctx, 80), 0, 2000);
        s.seed = static_cast<uint64_t>(params_.evalInt("seed", ctx, 1));
        s.start = std::max(params_.evalFloat("start", ctx, 0.02f), 0.0f);
        std::vector<int> kindOf;  // the species' slot (1, 2, 3) of each kind grown
        for (int k = 0; k < 3; ++k) {
            const std::string p = "s" + std::to_string(k + 1) + "_";
            if (!params_.evalBool(p + "on", ctx, true)) continue;
            Species sp;
            sp.share = std::max(params_.evalFloat(p + "share", ctx, 1.0f), 0.0f);
            sp.crown = std::max(params_.evalFloat(p + "crown", ctx, 3.0f), 0.05f);
            sp.growth = std::max(params_.evalFloat(p + "growth", ctx, 25.0f), 1.0f);
            sp.life = std::max(params_.evalFloat(p + "life", ctx, 150.0f), 1.0f);
            sp.shade = std::clamp(params_.evalFloat(p + "shade", ctx, 0.3f), 0.0f, 1.0f);
            sp.moisture = std::clamp(params_.evalFloat(p + "moisture", ctx, 0.5f), 0.0f, 1.0f);
            sp.tolerance = std::max(params_.evalFloat(p + "tolerance", ctx, 0.35f), 0.01f);
            sp.seeding = std::max(params_.evalFloat(p + "seeding", ctx, 8.0f), 0.1f);
            sp.seeds = std::max(params_.evalFloat(p + "seeds", ctx, 0.6f), 0.0f);
            s.species.push_back(sp);
            kindOf.push_back(k);
        }
        std::vector<float> wet;
        const std::string wetName = params_.getString("moistureattribute", "moisture");
        if (const AttributeArray* a = wetName.empty() ? nullptr : in.points().find(wetName); a && a->type() == AttrType::Float) {
            wet.assign(a->read<float>().begin(), a->read<float>().end());
        }
        const std::vector<EcoPlant> plants = growEcosystem(in.positions(), wet, s);

        out->addPoints(plants.size());
        auto P = out->positionsForWrite();
        auto species = out->points().create("species", AttrType::Int).write<int32_t>();
        auto age = out->points().create("age", AttrType::Float).write<float>();
        auto pscale = out->points().create("pscale", AttrType::Float).write<float>();
        auto orient = out->points().create("orient", AttrType::Vec4).write<Vec4>();
        auto id = out->points().create("id", AttrType::Int).write<int32_t>();
        Group* groups[3] = {&out->createGroup("species1", AttrClass::Point), &out->createGroup("species2", AttrClass::Point),
                            &out->createGroup("species3", AttrClass::Point)};
        for (size_t i = 0; i < plants.size(); ++i) {
            const EcoPlant& p = plants[i];
            P[i] = in.positions()[p.place];
            const int kind = kindOf[static_cast<size_t>(p.species)];
            species[i] = kind;
            age[i] = p.age;
            // A young one smaller: its crown's share of its kind's grown.
            pscale[i] = 0.15f + 0.85f * p.size;
            const float yaw = 2.0f * kPi * static_cast<float>((static_cast<uint64_t>(p.place) * 0x9E3779B97F4A7C15ull) >> 40) /
                              static_cast<float>(1u << 24);
            orient[i] = Vec4(0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw));
            id[i] = static_cast<int32_t>(p.place);
            groups[kind]->set(i, true);
        }
        return out;
    }
};

}  // namespace

void registerEcosystemNodes() {
    NodeRegistry::instance().add("ecosystem", [](const std::string& n) { return std::make_unique<EcosystemNode>(n); });
}

}  // namespace pg
