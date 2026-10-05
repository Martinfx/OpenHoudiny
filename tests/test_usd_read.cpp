// Reading USD without the library (pg/usd): layers in their three forms,
// the composition of a stage, transforms, geometry and cameras -- and the
// nodes that bring them into a network. The files in tests/data/usd were
// written by USD itself (make_fixtures.py).
#include "pg/core/CookEngine.h"
#include "pg/core/Instances.h"
#include "pg/io/Export.h"
#include "pg/io/Usda.h"
#include "pg/io/Vdb.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/usd/Geom.h"
#include "pg/usd/Layer.h"
#include "pg/usd/Stage.h"

#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

namespace {

const std::string kData = std::string(PG_TEST_DATA_DIR) + "/usd/";

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
    std::string write(const std::string& name, const std::string& text) const {
        const std::string p = *this / name;
        fs::create_directories(fs::path(p).parent_path());
        std::ofstream(p, std::ios::binary) << text;
        return p;
    }
};

std::shared_ptr<const usd::Stage> open(const std::string& path) {
    std::string error;
    auto s = usd::Stage::open(path, error);
    if (!s) ::testing::fail(__FILE__, __LINE__, "cannot open " + path + ": " + error);
    return s;
}

const usd::Stage::Prim& prim(const usd::Stage& s, const std::string& path) {
    const usd::Stage::Prim* p = s.find(path);
    if (!p) ::testing::fail(__FILE__, __LINE__, "no prim " + path);
    return *p;
}

double number(const usd::Stage& s, const std::string& path, const std::string& name, double time) {
    return s.value(prim(s, path), name, time).number(-12345.0);
}

Vec3 point(const usd::Matrix& m, double x, double y, double z) {
    const double in[3] = {x, y, z};
    double out[3];
    m.transformPoint(in, out);
    return Vec3(static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2]));
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-4f) { return length(a - b) <= eps; }

const usd::Property* property(const usd::Layer& l, const std::string& path, const std::string& name) {
    const usd::PrimSpec* p = l.prim(path);
    return p ? p->property(name) : nullptr;
}

}  // namespace

TEST(usd_text_reads_every_kind_of_value) {
    const std::string text = R"(#usda 1.0
(
    "a comment of the layer"
    defaultPrim = "World"
    metersPerUnit = 0.01
    subLayers = [
        @./anim.usda@ (offset = 1000; scale = 2),
        @@@path with \@@@ in it.usda@@@
    ]
)

# a comment
def Xform "World" (
    kind = "component"
    prepend references = [@./asset.usda@</Tree> (offset = 10), </World/other>]
    append payload = @./heavy.usdc@
    inherits = </_class_Tree>
    variants = {
        string lod = "high"
    }
    prepend variantSets = ["lod", "look"]
    customData = {
        string note = "matchmove v3"
        dictionary nested = {
            int deep = 5
        }
    }
    active = true
)
{
    float3 xformOp:translate = (1, -2.5e-3, inf)
    quatf xformOp:orient = (0.5, 0.1, 0.2, 0.3)
    matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 6, 7, 1) )
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:orient"]
    custom string label = "say \"hi\"\n\ttab"
    string multi = """two
lines"""
    asset tex = @./tex.png@ (
        colorSpace = "sRGB"
    )
    int[] counts = [4, 3]
    point3f[] points.timeSamples = {
        1: [(0, 0, 0)],
        2.5: None,
        3: [(1, 2, 3)],
    }
    texCoord2f[] primvars:st = [(0, 0), (1, 1)] (
        interpolation = "faceVarying"
        elementSize = 1
    )
    bool flag = true
    double blocked = None
    float declared
    rel material:binding = </World/Looks/red>
    prepend rel proxy = [</World/a>, </World/b>]
    float inputs:x.connect = </World/Shader.outputs:out>

    variantSet "lod" = {
        "high" (
            doc = "the close one"
        ) {
            def Sphere "ball"
            {
                double radius = 2
            }
        }
        "low" {
            def Cube "ball"
            {
            }
        }
    }

    def Mesh "child"
    {
        reorder nameChildren = ["b", "a"]
    }
}

over "Over"
{
}

class "_class_Tree"
{
}
)";
    usd::Layer l;
    std::string error;
    CHECK(usd::parseText(text, l, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(l.text("defaultPrim"), std::string("World"));
    CHECK_NEAR(l.number("metersPerUnit", 0), 0.01, 1e-12);
    CHECK_EQ(l.subLayers.size(), 2u);
    CHECK_EQ(l.subLayers[1], std::string("path with @@@ in it.usda"));
    CHECK_NEAR(l.subLayerOffsets[0].first, 1000.0, 0);
    CHECK_NEAR(l.subLayerOffsets[0].second, 2.0, 0);
    const usd::PrimSpec* w = l.prim("/World");
    CHECK(w != nullptr);
    CHECK_EQ(w->typeName, std::string("Xform"));
    CHECK(w->specifier == usd::Specifier::Def);
    CHECK_EQ(w->meta("kind")->text(), std::string("component"));
    CHECK_EQ(w->references.prepended.size(), 2u);
    CHECK_EQ(w->references.prepended[0].text, std::string("./asset.usda"));
    CHECK_EQ(w->references.prepended[0].path, std::string("/Tree"));
    CHECK_NEAR(w->references.prepended[0].offset, 10.0, 0);
    CHECK_EQ(w->references.prepended[1].text, std::string());
    CHECK_EQ(w->references.prepended[1].path, std::string("/World/other"));
    CHECK_EQ(w->payloads.appended.size(), 1u);
    CHECK(w->inherits.isExplicit);
    CHECK_EQ(w->inherits.explicitItems[0].text, std::string("/_class_Tree"));
    CHECK_EQ(std::string(w->selection("lod")), std::string("high"));
    CHECK_EQ(w->variantSetNames.prepended.size(), 2u);
    const usd::Value* custom = w->meta("customData");
    CHECK(custom && custom->find("nested") && custom->find("nested")->find("deep"));
    CHECK_NEAR(custom->find("nested")->find("deep")->number(), 5.0, 0);
    const usd::Property* t = w->property("xformOp:translate");
    CHECK(t && t->hasDefault && t->value.numbers.size() == 3u);
    CHECK_NEAR(t->value.numbers[1], -2.5e-3, 1e-15);
    CHECK(std::isinf(t->value.numbers[2]));
    // Quaternions are written real first; kept imaginary first.
    const usd::Property* q = w->property("xformOp:orient");
    CHECK_NEAR(q->value.numbers[0], 0.1, 1e-12);
    CHECK_NEAR(q->value.numbers[3], 0.5, 1e-12);
    CHECK_NEAR(w->property("xformOp:transform")->value.numbers[12], 5.0, 0);
    CHECK_EQ(w->property("xformOpOrder")->value.strings.size(), 2u);
    CHECK(w->property("xformOpOrder")->uniform);
    CHECK_EQ(w->property("label")->value.text(), std::string("say \"hi\"\n\ttab"));
    CHECK(w->property("label")->custom);
    CHECK_EQ(w->property("multi")->value.text(), std::string("two\nlines"));
    CHECK_EQ(w->property("tex")->value.text(), std::string("./tex.png"));
    const usd::Property* pts = w->property("points");
    CHECK(pts->hasSamples && pts->times.size() == 3u);
    CHECK(pts->samples[1].blocked());
    CHECK_NEAR(pts->samples[2].numbers[2], 3.0, 0);
    CHECK_EQ(w->property("primvars:st")->meta("interpolation")->text(), std::string("faceVarying"));
    CHECK_NEAR(w->property("flag")->value.number(), 1.0, 0);
    CHECK(w->property("blocked")->value.blocked());
    CHECK(!w->property("declared")->hasDefault);
    CHECK(w->property("material:binding")->relationship);
    CHECK_EQ(w->property("material:binding")->targets.explicitItems[0].text, std::string("/World/Looks/red"));
    CHECK_EQ(w->property("proxy")->targets.prepended.size(), 2u);
    CHECK_EQ(w->property("inputs:x")->targets.explicitItems[0].text, std::string("/World/Shader.outputs:out"));
    CHECK(l.prim("/World{lod=high}ball") != nullptr);
    CHECK_EQ(l.prim("/World{lod=low}ball")->typeName, std::string("Cube"));
    CHECK(l.prim("/World/child") != nullptr);
    CHECK(l.prim("/Over")->specifier == usd::Specifier::Over);
    CHECK(l.prim("/_class_Tree")->specifier == usd::Specifier::Class);
}

TEST(usd_text_that_is_not_usd_says_where) {
    usd::Layer l;
    std::string error;
    CHECK(!usd::parseText("#usda 1.0\ndef Xform \"a\" {\n  float x = \n}\n", l, error));
    CHECK(error.find("line 4") != std::string::npos);
    CHECK(!usd::parseText("#usda 1.0\ndef \"a\" {", l, error));
    CHECK(!usd::parseText("not usd", l, error));
}

TEST(usd_crate_reads_as_the_text_of_the_same_stage) {
    std::string error;
    const auto text = usd::readLayer(kData + "values.usda", error);
    CHECK(text != nullptr);
    const auto crate = usd::readLayer(kData + "values.usdc", error);
    CHECK(crate != nullptr);
    CHECK_EQ(error, std::string());
    // Every property of every prim: the same values, samples, targets.
    int compared = 0;
    std::vector<std::pair<std::string, const usd::PrimSpec*>> todo{{"", &text->root}};
    while (!todo.empty()) {
        auto [path, spec] = todo.back();
        todo.pop_back();
        for (const auto& c : spec->children) todo.emplace_back(path + "/" + c->name, c.get());
        if (path.empty()) continue;
        const usd::PrimSpec* other = crate->prim(path);
        CHECK(other != nullptr);
        CHECK_EQ(other->typeName, spec->typeName);
        for (const usd::Property& a : spec->properties) {
            const usd::Property* b = other->property(a.name);
            if (!b) ::testing::fail(__FILE__, __LINE__, "the crate has no " + path + "." + a.name);
            CHECK_EQ(b->hasDefault, a.hasDefault);
            CHECK_EQ(b->times.size(), a.times.size());
            auto same = [&](const usd::Value& x, const usd::Value& y) {
                CHECK_EQ(x.blocked(), y.blocked());
                CHECK_EQ(x.strings, y.strings);
                CHECK_EQ(x.numbers.size(), y.numbers.size());
                for (size_t k = 0; k < x.numbers.size() && k < y.numbers.size(); ++k) {
                    CHECK_NEAR(static_cast<float>(x.numbers[k]), static_cast<float>(y.numbers[k]),
                               1e-6 * std::max(1.0, std::abs(x.numbers[k])));
                }
            };
            if (a.hasDefault) same(a.value, b->value);
            for (size_t k = 0; k < a.times.size() && k < b->times.size(); ++k) {
                CHECK_NEAR(a.times[k], b->times[k], 0);
                same(a.samples[k], b->samples[k]);
            }
            CHECK_EQ(a.targets.items(), b->targets.items());
            ++compared;
        }
    }
    if (compared < 50) ::testing::fail(__FILE__, __LINE__, "compared only " + std::to_string(compared));
    // And as USD wrote them.
    const usd::Layer& c = *crate;
    CHECK_NEAR(c.number("metersPerUnit", 0), 0.01, 1e-12);
    CHECK_EQ(c.text("upAxis"), std::string("Z"));
    CHECK_NEAR(property(c, "/World", "ints")->value.numbers[5], 15.0, 0);
    CHECK_NEAR(property(c, "/World", "int64s")->value.numbers[2], 1099511627776.0, 0);
    CHECK_NEAR(property(c, "/World", "uints")->value.numbers[1], 3999999993.0, 0);
    CHECK_NEAR(property(c, "/World", "big64")->value.number(), 1099511627776.0, 0);
    CHECK_NEAR(property(c, "/World", "small64")->value.number(), -7.0, 0);
    CHECK_NEAR(property(c, "/World", "floats_whole")->value.numbers[18], 1.0, 0);
    CHECK_NEAR(property(c, "/World", "floats_table")->value.numbers[4], 0.25, 0);
    CHECK_NEAR(property(c, "/World", "doubles_table")->value.numbers[5], 1e10, 0);
    CHECK_NEAR(property(c, "/World", "halfs")->value.numbers[2], 2.25, 0);
    CHECK_NEAR(property(c, "/World", "double")->value.number(), 0.1, 1e-17);
    CHECK_NEAR(property(c, "/World", "m4")->value.numbers[13], 14.0, 0);
    CHECK_NEAR(property(c, "/World", "m4_diagonal")->value.numbers[15], 2.0, 0);
    CHECK_NEAR(property(c, "/World", "m4_diagonal")->value.numbers[1], 0.0, 0);
    CHECK_NEAR(property(c, "/World", "m3")->value.numbers[8], -4.0, 0);
    CHECK_NEAR(property(c, "/World", "q")->value.numbers[3], 0.5, 1e-7);
    CHECK_NEAR(property(c, "/World", "q")->value.numbers[0], 0.1, 1e-7);
    CHECK_NEAR(property(c, "/World", "i2")->value.numbers[0], 1000.0, 0);
    CHECK_EQ(property(c, "/World", "tokens")->value.strings.size(), 3u);
    CHECK_EQ(property(c, "/World", "string")->value.text(), std::string("say \"hi\"\n\ttab \\ back"));
    CHECK_EQ(property(c, "/World", "asset")->value.text(), std::string("./tex.png"));
    CHECK_EQ(property(c, "/World", "assets")->value.strings[1], std::string("./b.png"));
    CHECK(property(c, "/World", "empty")->value.numbers.empty());
    CHECK_NEAR(property(c, "/World", "byte")->value.number(), 200.0, 0);
    CHECK(property(c, "/World", "blocked")->value.blocked());
    const usd::Property* anim = property(c, "/World", "anim");
    CHECK_EQ(anim->times.size(), 4u);
    CHECK(anim->samples[2].blocked());
    const usd::PrimSpec* ref = c.prim("/World/ref");
    const auto refs = ref->references.items();
    CHECK_EQ(refs.size(), 2u);
    CHECK_EQ(refs[0].text, std::string("./other.usda"));
    CHECK_EQ(refs[0].path, std::string("/Thing"));
    CHECK_NEAR(refs[0].offset, 10.0, 0);
    CHECK_NEAR(refs[0].scale, 2.0, 0);
    CHECK_EQ(refs[1].path, std::string("/World/mesh"));
    CHECK_NEAR(ref->payloads.items()[0].offset, 5.0, 0);
    CHECK_EQ(ref->inherits.items()[0].text, std::string("/_class_Thing"));
    CHECK_EQ(std::string(ref->selection("lod")), std::string("high"));
    CHECK_EQ(c.prim("/World/ref{lod=high}shape")->typeName, std::string("Sphere"));
    const usd::PrimSpec* cam = c.prim("/World/cam");
    CHECK_EQ(cam->meta("customData")->find("nested")->find("deep")->number(), 5.0);
    CHECK_EQ(property(c, "/World", "targets")->targets.items().size(), 2u);
}

TEST(usd_crate_of_the_oldest_version_read) {
    std::vector<uint8_t> bytes;
    std::string error;
    CHECK(usd::readFileBytes(kData + "old.usdc", bytes, error));
    CHECK(bytes.size() > 16 && bytes[9] == 4);  // version 0.4.0
    const auto stage = open(kData + "old.usdc");
    const usd::Stage::Prim& mesh = prim(*stage, "/World/mesh");
    const usd::Value points = stage->value(mesh, "points", 0.0);
    CHECK_EQ(points.size(), 64u);
    CHECK_NEAR(points.numbers[3 * 63], 31.5, 0);
    const usd::Value indices = stage->value(mesh, "faceVertexIndices", 0.0);
    CHECK_EQ(indices.size(), 186u);
    CHECK_NEAR(indices.numbers[185], 63.0, 0);
    CHECK_EQ(prim(*stage, "/World/ref").type, std::string("Mesh"));
}

TEST(usd_package_reads_the_files_in_it) {
    std::vector<std::string> names;
    std::string error;
    CHECK(usd::packageFiles(kData + "package.usdz", names, error));
    CHECK_EQ(names.size(), 2u);
    const auto stage = open(kData + "package.usdz");
    CHECK(stage->warnings().empty());
    const usd::Stage::Prim& part = prim(*stage, "/World/part");
    CHECK_EQ(part.type, std::string("Cube"));
    CHECK(near(point(usd::worldTransform(*stage, part, 0.0), 0, 0, 0), Vec3(1, 2, 3)));
}

TEST(usd_stage_composes_sublayers_references_variants_and_classes) {
    TempFolder dir("usd_compose");
    dir.write("asset.usda", R"(#usda 1.0
(
    defaultPrim = "Tree"
)

def Xform "Tree" (
    variants = {
        string lod = "high"
    }
    prepend variantSets = "lod"
)
{
    double height = 4
    double spin.timeSamples = {
        0: 0,
        10: 90,
    }

    def Xform "leaf" (
        prepend inherits = </_class_leaf>
    )
    {
    }

    def "copy" (
        prepend references = </Tree/trunk>
    )
    {
    }

    def Cylinder "trunk"
    {
        double radius = 0.3
    }
    variantSet "lod" = {
        "high" {
            def Sphere "crown"
            {
                double radius = 2
            }
        }
        "low" {
            def Cube "crown"
            {
                double size = 2.5
            }
        }
    }
}

class "_class_leaf"
{
    float leafiness = 0.5
}
)");
    dir.write("anim.usda", R"(#usda 1.0
(
    timeCodesPerSecond = 48
)

over "World"
{
    over "tree1"
    {
        double wobble.timeSamples = {
            0: 0,
            20: 20,
        }
    }
}
)");
    const std::string shot = dir.write("shot.usda", R"(#usda 1.0
(
    defaultPrim = "World"
    subLayers = [
        @./anim.usda@ (offset = 1000; scale = 2)
    ]
    timeCodesPerSecond = 24
    upAxis = "Z"
)

def Xform "World"
{
    def "tree1" (
        prepend references = @./asset.usda@ (offset = 10)
    )
    {
        over "trunk"
        {
            double radius = 0.6
        }
    }

    def "tree2" (
        prepend references = @./asset.usda@
        variants = {
            string lod = "low"
        }
    )
    {
        over "leaf"
        {
            float leafiness = 0.9
        }
    }

    def "tree3" (
        active = false
        prepend references = @./asset.usda@
    )
    {
    }

    def "special" (
        prepend specializes = </World/tree1/leaf>
    )
    {
    }

    def "payload" (
        prepend payload = @./asset.usda@</Tree/trunk>
    )
    {
    }

    def "missing" (
        prepend references = @./nowhere.usda@
    )
    {
    }
}
)");
    const auto s = open(shot);
    CHECK(s->zUp());
    CHECK_NEAR(s->metersPerUnit(), 0.01, 1e-12);  // USD's own when the stage says none
    // The asset's own variant, and the one the shot chooses over it.
    CHECK_EQ(prim(*s, "/World/tree1/crown").type, std::string("Sphere"));
    CHECK_EQ(prim(*s, "/World/tree2/crown").type, std::string("Cube"));
    CHECK_NEAR(number(*s, "/World/tree2/crown", "size", 0), 2.5, 0);
    // The shot's override is stronger than the asset.
    CHECK_NEAR(number(*s, "/World/tree1/trunk", "radius", 0), 0.6, 0);
    CHECK_NEAR(number(*s, "/World/tree2/trunk", "radius", 0), 0.3, 0);
    // A reference inside the asset, a class, a specialize of a referenced prim.
    CHECK_EQ(prim(*s, "/World/tree1/copy").type, std::string("Cylinder"));
    CHECK_NEAR(number(*s, "/World/tree1/copy", "radius", 0), 0.3, 0);
    CHECK_NEAR(number(*s, "/World/tree1/leaf", "leafiness", 0), 0.5, 1e-7);
    CHECK_NEAR(number(*s, "/World/tree2/leaf", "leafiness", 0), 0.9, 1e-7);
    CHECK_NEAR(number(*s, "/World/special", "leafiness", 0), 0.5, 1e-7);
    // Time through the reference's offset, and the sublayer's -- its scale
    // of 2 halved by its 48 time codes a second against the shot's 24.
    CHECK_NEAR(number(*s, "/World/tree1", "spin", 15), 45.0, 1e-9);
    CHECK_NEAR(number(*s, "/World/tree2", "spin", 5), 45.0, 1e-9);
    CHECK_NEAR(number(*s, "/World/tree1", "wobble", 1005), 5.0, 1e-9);
    CHECK_NEAR(number(*s, "/World/tree1", "wobble", 1010), 10.0, 1e-9);
    CHECK_NEAR(number(*s, "/World/tree1", "height", 0), 4.0, 0);
    // Inactive: there, without what is under it.
    CHECK(!prim(*s, "/World/tree3").active);
    CHECK(!prim(*s, "/World/tree3").defined);
    CHECK(s->find("/World/tree3/crown") == nullptr);
    CHECK_EQ(prim(*s, "/World/payload").type, std::string("Cylinder"));
    // What cannot be read is a warning, not a failure.
    CHECK_EQ(s->warnings().size(), 1u);
    CHECK(s->warnings()[0].find("nowhere.usda") != std::string::npos);
    CHECK(s->find("/_class_leaf") == nullptr);  // the asset's class stays in the asset
}

TEST(usd_value_clips_give_a_value_a_frame_interpolated_between) {
    TempFolder dir("usd_clips");
    dir.write("clip1.usda", "#usda 1.0\nover \"p\"\n{\n    float w.timeSamples = {\n        1: 1,\n    }\n"
                            "    point3f[] points.timeSamples = {\n        1: [(10, 0, 0)],\n    }\n}\n");
    dir.write("clip2.usda", "#usda 1.0\nover \"p\"\n{\n    float w.timeSamples = {\n        2: 2,\n    }\n"
                            "    point3f[] points.timeSamples = {\n        2: [(20, 0, 0)],\n    }\n}\n");
    dir.write("clip3.usda", "#usda 1.0\nover \"p\"\n{\n    point3f[] points.timeSamples = {\n        3: [(30, 0, 0)],\n    }\n}\n");
    dir.write("manifest.usda", "#usda 1.0\nover \"p\"\n{\n    float w\n    point3f[] points\n}\n");
    const std::string stage = dir.write("stage.usda", R"(#usda 1.0

def Points "p" (
    clips = {
        dictionary default = {
            double2[] active = [(1, 0), (2, 1), (3, 2)]
            asset[] assetPaths = [@./clip1.usda@, @./clip2.usda@, @./clip3.usda@]
            asset manifestAssetPath = @./manifest.usda@
            string primPath = "/p"
            double2[] times = [(1, 1), (2, 2), (3, 3)]
        }
    }
)
{
}
)");
    const auto s = open(stage);
    const usd::Stage::Prim& p = prim(*s, "/p");
    CHECK_NEAR(s->value(p, "w", 1.0).number(), 1.0, 0);
    CHECK_NEAR(s->value(p, "w", 1.5).number(), 1.5, 1e-9);  // across two clips, as USD does
    CHECK_NEAR(s->value(p, "w", 2.0).number(), 2.0, 0);
    CHECK(s->value(p, "w", 3.0).empty());  // not in the third clip: the manifest's block
    CHECK_NEAR(s->value(p, "points", 1.5).numbers[0], 15.0, 1e-6);
    CHECK_NEAR(s->value(p, "points", 3.0).numbers[0], 30.0, 0);
    CHECK(s->varies(p, "points"));
    CHECK_EQ(s->property(p, "w")->typeName, std::string("float"));
}

// Where clips come in the order of strength, as USD has it (the values
// checked are what USD itself gives): right after the layer that names
// them -- a stronger layer's value wins over them, they win over a weaker
// layer's -- on the prim and, through it, on the prims under it; a clip
// set's fields composed over the layers, each from the strongest.
TEST(usd_value_clips_are_as_strong_as_the_layer_that_names_them) {
    TempFolder dir("usd_clip_strength");
    for (int i = 1; i <= 2; ++i) {
        const auto n = [&](int base) { return std::to_string(i * base); };
        const auto m = [&](int base) { return std::to_string(i * base + 1); };
        dir.write("clip" + std::to_string(i) + ".usda",
                  "#usda 1.0\ndef \"Root\"\n{\n"
                  "    double a.timeSamples = { 0: " + n(10) + ", 1: " + m(10) + " }\n"
                  "    double b.timeSamples = { 0: " + n(100) + ", 1: " + m(100) + " }\n"
                  "    def \"kid\"\n    {\n"
                  "        double c.timeSamples = { 0: " + n(1000) + ", 1: " + m(1000) + " }\n"
                  "        double d.timeSamples = { 0: " + n(1000) + ", 1: " + m(1000) + " }\n"
                  "    }\n}\n");
    }
    dir.write("strong.usda", "#usda 1.0\nover \"M\"\n{\n    double b = -1\n    over \"kid\"\n    {\n        double d = -2\n    }\n}\n");
    dir.write("fx.usda", R"(#usda 1.0
over "M" (
    clips = {
        dictionary default = {
            asset[] assetPaths = [@./clip1.usda@, @./clip2.usda@]
            string primPath = "/Root"
            double2[] active = [(0, 0), (10, 1)]
            double2[] times = [(0, 0), (10, 1), (20, 1)]
        }
    }
)
{
}
)");
    dir.write("weak.usda", "#usda 1.0\ndef Xform \"M\"\n{\n    double a = 7\n    double b = 8\n"
                           "    def \"kid\"\n    {\n        double c = 9\n        double d = 10\n    }\n}\n");
    const auto s = open(dir.write("shot.usda", "#usda 1.0\n(\n    subLayers = [@./strong.usda@, @./fx.usda@, @./weak.usda@]\n)\n"));
    const usd::Stage::Prim& m = prim(*s, "/M");
    const usd::Stage::Prim& kid = prim(*s, "/M/kid");
    CHECK_NEAR(s->value(m, "a", 0).number(), 10.0, 0);     // over weak.usda's 7
    CHECK_NEAR(s->value(m, "a", 5).number(), 15.5, 1e-12);  // clip 1 at 0 to clip 2 at 10
    CHECK_NEAR(s->value(m, "a", 15).number(), 21.0, 0);
    CHECK_NEAR(s->value(m, "b", 5).number(), -1.0, 0);     // strong.usda's, over the clips
    CHECK_NEAR(s->value(kid, "c", 0).number(), 1000.0, 0);  // the parent's clips, one prim down
    CHECK_NEAR(s->value(kid, "c", 5).number(), 1500.5, 1e-9);
    CHECK_NEAR(s->value(kid, "d", 5).number(), -2.0, 0);
    const std::vector<double> times = s->sampleTimes(m, "a");
    CHECK_EQ(times.size(), 3u);  // 0, 10 and 20: a held stretch of the mapping has both ends
    CHECK(times.size() == 3 && times[1] == 10.0 && times[2] == 20.0);
    CHECK(s->sampleTimes(m, "b").empty());
    CHECK(!s->varies(kid, "d"));

    // The fields of one set in two layers: the stronger one's active, the
    // weaker one's assets -- which anchor the set in the weaker layer.
    dir.write("split_a.usda", "#usda 1.0\nover \"S\" (\n    clips = {\n        dictionary default = {\n"
                              "            double2[] active = [(0, 1)]\n        }\n    }\n)\n{\n}\n");
    dir.write("split_b.usda", R"(#usda 1.0
def "S" (
    clips = {
        dictionary default = {
            asset[] assetPaths = [@./clip1.usda@, @./clip2.usda@]
            string primPath = "/Root"
            double2[] active = [(0, 0)]
        }
    }
)
{
}
)");
    const auto split = open(dir.write("split.usda", "#usda 1.0\n(\n    subLayers = [@./split_a.usda@, @./split_b.usda@]\n)\n"));
    CHECK_NEAR(split->value(prim(*split, "/S"), "a", 0).number(), 20.0, 0);
    CHECK_NEAR(split->value(prim(*split, "/S"), "a", 5).number(), 21.0, 0);
}

// Clips a template names -- a file a frame, one missing -- with and without
// an active offset, and a mapping that jumps back to loop a clip: as USD.
TEST(usd_value_clips_from_a_template_and_in_a_loop) {
    TempFolder dir("usd_clip_template");
    dir.write("frames/frame.001.usda", "#usda 1.0\nover \"Root\"\n{\n    double x.timeSamples = { 1: 10 }\n}\n");
    dir.write("frames/frame.002.usda", "#usda 1.0\nover \"Root\"\n{\n    double x.timeSamples = { 2: 20, 2.5: 25 }\n}\n");
    dir.write("frames/frame.004.usda", "#usda 1.0\nover \"Root\"\n{\n    double x.timeSamples = { 4: 40 }\n}\n");
    dir.write("loop.usda", "#usda 1.0\nover \"Root\"\n{\n    double x.timeSamples = { 0: 0, 10: 100 }\n}\n");
    const auto s = open(dir.write("shot.usda", R"(#usda 1.0
def "T" (
    clips = {
        dictionary default = {
            string templateAssetPath = "./frames/frame.###.usda"
            double templateStartTime = 1
            double templateEndTime = 4
            double templateStride = 1
            string primPath = "/Root"
        }
    }
)
{
}
def "O" (
    clips = {
        dictionary default = {
            string templateAssetPath = "./frames/frame.###.usda"
            double templateStartTime = 1
            double templateEndTime = 4
            double templateStride = 1
            double templateActiveOffset = 0.5
            string primPath = "/Root"
        }
    }
)
{
}
def "L" (
    clips = {
        dictionary default = {
            asset[] assetPaths = [@./loop.usda@]
            double2[] active = [(0, 0)]
            double2[] times = [(0, 0), (10, 10), (10, 0), (20, 10)]
            string primPath = "/Root"
        }
    }
)
{
}
)"));
    const usd::Stage::Prim& t = prim(*s, "/T");
    const std::vector<std::pair<double, double>> plain = {{0.5, 10}, {1, 10},   {1.5, 15}, {2, 20}, {2.25, 22.5},
                                                          {2.5, 25}, {3, 30}, {3.5, 35}, {4, 40}, {5, 40}};
    for (const auto& [time, x] : plain) CHECK_NEAR(s->value(t, "x", time).number(), x, 1e-12);
    const usd::Stage::Prim& o = prim(*s, "/O");
    const std::vector<std::pair<double, double>> offset = {{0.5, 10}, {1.4, 10}, {2, 10},  {2.6, 25},
                                                           {3.5, 25}, {4, 25},   {4.5, 40}, {5, 40}};
    for (const auto& [time, x] : offset) CHECK_NEAR(s->value(o, "x", time).number(), x, 1e-12);
    CHECK_EQ(s->sampleTimes(o, "x").size(), 7u);  // 0.5 1 1.5 2 2.5 4 4.5
    const usd::Stage::Prim& l = prim(*s, "/L");
    const std::vector<std::pair<double, double>> loop = {{0, 0},      {5, 50},   {9.5, 95},  {10, 0},
                                                         {12.5, 25}, {20, 100}, {25, 100}};
    for (const auto& [time, x] : loop) CHECK_NEAR(s->value(l, "x", time).number(), x, 1e-9);
    const std::vector<double> times = s->sampleTimes(l, "x");
    CHECK_EQ(times.size(), 4u);  // 0, just before 10, 10, 20
    CHECK(times.size() == 4 && times[1] < 10.0 && times[1] > 9.99999 && times[2] == 10.0);

    // Clips that cannot be: warnings, no values from them -- and no end to
    // wait for, nor a crash.
    const auto bad = open(dir.write("bad.usda", R"(#usda 1.0
def "A" (
    clips = {
        dictionary a = { string templateAssetPath = "./frames/frame.###.usda"
            double templateStartTime = 1e300
            double templateEndTime = 1e300
            double templateStride = 1
            string primPath = "/Root" }
        dictionary b = { string templateAssetPath = "./frames/frame.###.usda"
            double templateStartTime = 1e8
            double templateEndTime = 1e8
            double templateStride = 1e-300
            string primPath = "/Root" }
        dictionary c = { asset[] assetPaths = [@./loop.usda@]
            double2[] active = [(0, 1e300), (1, nan)]
            string primPath = "/Root" }
        dictionary d = { asset[] assetPaths = [@./loop.usda@]
            double2[] active = [(0, 0)]
            string primPath = "Root/../x" }
    }
)
{
    double x = 7
}
)"));
    CHECK_NEAR(bad->value(prim(*bad, "/A"), "x", 5).number(), 7.0, 0);
    CHECK(bad->warnings().size() >= 2);
}

// Strength and the specifier as USD composes them: what an asset
// specializes is weaker than everything else, the shot's other references
// too; a class a prim inherits does not make it a class.
TEST(usd_specializes_come_last_and_an_inherited_class_is_no_class) {
    TempFolder dir("usd_specializes");
    dir.write("asset1.usda", R"(#usda 1.0
(
    defaultPrim = "A"
)
def "A" (
    specializes = </A_s>
)
{
    double onlyRef1 = 1
}
class "A_s"
{
    double v = 10
    double w = 10
}
)");
    dir.write("asset2.usda", "#usda 1.0\n(\n    defaultPrim = \"B\"\n)\ndef \"B\"\n{\n    double v = 20\n}\n");
    const auto s = open(dir.write("root.usda", R"(#usda 1.0
def "X" (
    specializes = </_s1>
    references = [@./asset1.usda@, @./asset2.usda@]
)
{
}
class "_s1"
{
    double w = 30
}
over "Y" (
    inherits = </_c>
    references = @./asset2.usda@
)
{
}
class "_c"
{
}
)"));
    const usd::Stage::Prim& x = prim(*s, "/X");
    CHECK_NEAR(s->value(x, "v", 0).number(), 20.0, 0);  // asset2's, over what asset1 specializes
    CHECK_NEAR(s->value(x, "w", 0).number(), 10.0, 0);  // asset1's specialized class, over the shot's
    CHECK_NEAR(s->value(x, "onlyRef1", 0).number(), 1.0, 0);
    const usd::Stage::Prim& y = prim(*s, "/Y");
    CHECK(y.specifier == usd::Specifier::Def);  // over, class (inherited), def: a def
    CHECK(y.defined);
    CHECK(prim(*s, "/_c").specifier == usd::Specifier::Class);
    CHECK(!prim(*s, "/_c").defined);
}

TEST(usd_transforms_apply_the_last_op_first) {
    TempFolder dir("usd_xform");
    const std::string path = dir.write("x.usda", R"(#usda 1.0

def Xform "a"
{
    double3 xformOp:translate = (1, 2, 3)
    float3 xformOp:rotateXYZ = (90, 0, 0)
    float3 xformOp:scale = (2, 2, 2)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]

    def Xform "b"
    {
        double3 xformOp:translate:pivot = (1, 0, 0)
        float xformOp:rotateZ = 90
        uniform token[] xformOpOrder = ["xformOp:translate:pivot", "xformOp:rotateZ", "!invert!xformOp:translate:pivot"]
    }

    def Xform "c"
    {
        quatf xformOp:orient = (0.70710677, 0, 0.70710677, 0)
        uniform token[] xformOpOrder = ["!resetXformStack!", "xformOp:orient"]
    }

    def Scope "scope"
    {
        double3 xformOp:translate = (100, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]

        def Xform "d"
        {
            double3 xformOp:translate.timeSamples = {
                0: (0, 0, 0),
                10: (10, 0, 0),
            }
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }
    }
}
)");
    const auto s = open(path);
    // Scale, then turn about x, then move.
    const usd::Matrix a = usd::worldTransform(*s, prim(*s, "/a"), 0.0);
    CHECK(near(point(a, 1, 0, 0), Vec3(3, 2, 3)));
    CHECK(near(point(a, 0, 1, 0), Vec3(1, 2, 5)));
    // About a pivot, then through the parent.
    CHECK(near(point(usd::worldTransform(*s, prim(*s, "/a/b"), 0.0), 2, 0, 0), Vec3(3, 2, 5)));
    // Its parents left out: 90 degrees about y takes x to -z.
    CHECK(near(point(usd::worldTransform(*s, prim(*s, "/a/c"), 0.0), 1, 0, 0), Vec3(0, 0, -1)));
    // A Scope is not transformed; samples interpolate.
    CHECK(near(point(usd::worldTransform(*s, prim(*s, "/a/scope/d"), 2.5), 0, 0, 0), point(a, 2.5, 0, 0)));
    CHECK(usd::transformVaries(*s, prim(*s, "/a/scope/d")));
    CHECK(!usd::transformVaries(*s, prim(*s, "/a/c")));
}

TEST(usd_import_reads_meshes_points_curves_and_shapes_in_metres_y_up) {
    TempFolder dir("usd_geo");
    const std::string path = dir.write("geo.usda", R"(#usda 1.0
(
    metersPerUnit = 0.01
    upAxis = "Z"
)

def Xform "W"
{
    def Mesh "m"
    {
        point3f[] points = [(0, 0, 0), (100, 0, 0), (100, 100, 0), (0, 100, 0), (50, 50, 100)]
        int[] faceVertexCounts = [4, 3, 3]
        int[] faceVertexIndices = [0, 1, 2, 3, 0, 1, 4, 1, 2, 4]
        int[] holeIndices = [2]
        uniform token orientation = "leftHanded"
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1)] (
            interpolation = "faceVarying"
        )
        int[] primvars:st:indices = [0, 1, 2, 0, 0, 1, 2, 0, 1, 2]
        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] (
            interpolation = "uniform"
        )
        float[] primvars:heat = [1, 2, 3, 4, 5] (
            interpolation = "vertex"
        )

        def GeomSubset "roof"
        {
            uniform token elementType = "face"
            int[] indices = [1]
        }
    }

    def Points "pts"
    {
        point3f[] points = [(100, 200, 300)]
        float[] widths = [4]
        int64[] ids = [7]
    }

    def BasisCurves "hair"
    {
        point3f[] points = [(0, 0, 0), (0, 0, 10), (0, 0, 20)]
        int[] curveVertexCounts = [3]
        uniform token type = "linear"
    }

    def Cube "box"
    {
        double size = 100
        double3 xformOp:translate = (0, 0, 500)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Mesh "proxy"
    {
        uniform token purpose = "proxy"
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }

    def Mesh "hidden"
    {
        token visibility = "invisible"
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
}
)");
    const auto s = open(path);
    std::vector<std::string> notes;
    const auto geo = usd::importGeometry(*s, 0.0, usd::ImportOptions{}, &notes);
    CHECK(notes.empty());
    // 5 + 1 + 3 + 8 points; 2 faces (the hole left out) + 1 curve + 6 faces.
    CHECK_EQ(geo->pointCount(), 17u);
    CHECK_EQ(geo->primitiveCount(), 9u);
    const auto P = geo->positions();
    CHECK(near(P[1], Vec3(1, 0, 0)));
    CHECK(near(P[2], Vec3(1, 0, -1)));
    CHECK(near(P[4], Vec3(0.5f, 1, -0.5f)));
    CHECK(near(P[5], Vec3(1, 3, -2)));
    // Left-handed: the corners turned round, the uv with them.
    const auto first = geo->primitivePoints(0);
    CHECK_EQ(first[0], 3u);
    CHECK_EQ(first[3], 0u);
    const auto uv = geo->vertices().find("uv")->read<Vec3>();
    CHECK(near(uv[1], Vec3(1, 1, 0)));
    CHECK(near(uv[2], Vec3(1, 0, 0)));
    const auto cd = geo->primitives().find("Cd")->read<Vec3>();
    CHECK(near(cd[1], Vec3(0, 1, 0)));
    CHECK_NEAR(geo->points().find("heat")->read<float>()[4], 5.0f, 0);
    const Group* roof = geo->findGroup("roof");
    CHECK(roof && roof->contains(1) && !roof->contains(0));
    CHECK_NEAR(geo->points().find("pscale")->read<float>()[5], 0.02f, 1e-7);
    CHECK_EQ(geo->points().find("id")->read<int32_t>()[5], 7);
    CHECK(!geo->primitiveClosed(2));
    const AttributeArray* pathAttr = geo->primitives().find("path");
    CHECK_EQ(pathAttr->stringValue(pathAttr->read<int32_t>()[0]), std::string("/W/m"));
    CHECK_EQ(pathAttr->stringValue(pathAttr->read<int32_t>()[8]), std::string("/W/box"));
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 9; i < 17; ++i) lo = std::min(lo, P[i].y), hi = std::max(hi, P[i].y);
    CHECK_NEAR(lo, 4.5f, 1e-5);
    CHECK_NEAR(hi, 5.5f, 1e-5);
    // The proxy with its purpose asked for; only what is under a root.
    usd::ImportOptions proxy;
    proxy.proxy = true;
    CHECK_EQ(usd::importGeometry(*s, 0.0, proxy)->primitiveCount(), 10u);
    usd::ImportOptions one;
    one.roots = {"/W/pts"};
    CHECK_EQ(usd::importGeometry(*s, 0.0, one)->pointCount(), 1u);
    usd::ImportOptions raw;
    raw.metresYUp = false;
    CHECK(near(usd::importGeometry(*s, 0.0, raw)->positions()[1], Vec3(100, 0, 0)));
    CHECK(!usd::geometryVaries(*s, usd::ImportOptions{}));
}

TEST(usd_import_puts_a_primvar_given_on_several_classes_on_the_corners) {
    // One mesh's st on its points, another's on its corners, a third with
    // none; a colour on faces, on points, on loose points: what reads the
    // corners' first finds each prim's own there.
    TempFolder dir("usd_classes");
    const std::string path = dir.write("classes.usda", R"(#usda 1.0
def Mesh "a"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
        interpolation = "vertex"
    )
    color3f[] primvars:displayColor = [(1, 0, 0)]
    float[] primvars:heat = [7]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
        interpolation = "faceVarying"
    )
}

def Mesh "b"
{
    point3f[] points = [(2, 0, 0), (3, 0, 0), (3, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    texCoord2f[] primvars:st = [(0.5, 0.5), (0.25, 0.75), (0.125, 0.5)] (
        interpolation = "faceVarying"
    )
    color3f[] primvars:displayColor = [(0, 1, 0), (0, 0, 1), (1, 1, 1)] (
        interpolation = "vertex"
    )
    float3[] primvars:heat = [(1, 2, 3), (4, 5, 6), (7, 8, 9)] (
        interpolation = "vertex"
    )
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]
}

def Points "dust"
{
    point3f[] points = [(5, 5, 5), (6, 6, 6)]
    color3f[] primvars:displayColor = [(0.5, 0.5, 0.5), (0.25, 0.25, 0.25)] (
        interpolation = "vertex"
    )
}

def Mesh "c"
{
    point3f[] points = [(0, 0, 1), (1, 0, 1), (0, 1, 1)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
}
)");
    const auto s = open(path);
    std::vector<std::string> notes;
    const auto geo = usd::importGeometry(*s, 0.0, usd::ImportOptions{}, &notes);
    CHECK_EQ(geo->pointCount(), 12u);
    CHECK_EQ(geo->vertexCount(), 10u);
    CHECK(!geo->points().find("uv"));
    const auto uv = geo->vertices().find("uv")->read<Vec3>();
    CHECK(near(uv[2], Vec3(1, 1, 0)));
    CHECK(near(uv[3], Vec3(0, 1, 0)));
    CHECK(near(uv[5], Vec3(0.25f, 0.75f, 0)));
    CHECK(near(uv[9], Vec3(0, 0, 0)));
    CHECK(!geo->primitives().find("Cd"));
    const auto cd = geo->vertices().find("Cd")->read<Vec3>();
    CHECK(near(cd[0], Vec3(1, 0, 0)));
    CHECK(near(cd[3], Vec3(1, 0, 0)));
    CHECK(near(cd[5], Vec3(0, 0, 1)));
    CHECK(near(cd[6], Vec3(1, 1, 1)));
    // The loose points keep theirs on the points.
    const auto loose = geo->points().find("Cd")->read<Vec3>();
    CHECK(near(loose[8], Vec3(0.25f, 0.25f, 0.25f)));
    // A name of one width on the faces and another on the points stays so;
    // the normals meet on the corners too.
    CHECK(geo->primitives().find("heat") && geo->points().find("heat"));
    CHECK(!geo->points().find("N") && geo->vertices().find("N"));
    CHECK(near(geo->vertices().find("N")->read<Vec3>()[5], Vec3(0, 0, 1)));
    // A material's colour goes onto the corners too; the loose points keep
    // theirs.
    const std::string bound = dir.write("bound.usda", R"(#usda 1.0
def Mesh "a"
{
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    color3f[] primvars:displayColor = [(0, 1, 0), (0, 0, 1), (1, 1, 1)] (
        interpolation = "vertex"
    )
}

def Mesh "b" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    point3f[] points = [(2, 0, 0), (3, 0, 0), (3, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    rel material:binding = </Red>
}

def Points "dust"
{
    point3f[] points = [(5, 5, 5)]
    color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)] (
        interpolation = "vertex"
    )
}

def Material "Red"
{
    token outputs:surface.connect = </Red/Surface.outputs:surface>

    def Shader "Surface"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 0, 0)
        token outputs:surface
    }
}
)");
    const auto red = usd::importGeometry(*open(bound), 0.0, usd::ImportOptions{});
    const auto corner = red->vertices().find("Cd")->read<Vec3>();
    CHECK(near(corner[1], Vec3(0, 0, 1)));
    CHECK(near(corner[4], Vec3(1, 0, 0)));
    const AttributeArray* dust = red->points().find("Cd");
    CHECK(dust && near(dust->read<Vec3>()[6], Vec3(0.5f, 0.5f, 0.5f)));
    CHECK_EQ(notes.size(), 1u);
    CHECK(!notes.empty() && notes[0].find("heat") != std::string::npos);
}

TEST(usd_camera_node_follows_the_file_frame_by_frame) {
    TempFolder dir("usd_camera");
    const std::string path = dir.write("cam.usda", R"(#usda 1.0
(
    endTimeCode = 1003
    metersPerUnit = 0.01
    startTimeCode = 1001
    timeCodesPerSecond = 24
    upAxis = "Z"
)

def Camera "cam"
{
    float focalLength = 35
    float horizontalAperture = 36
    float verticalAperture = 24
    double3 xformOp:translate.timeSamples = {
        1001: (0, -500, 150),
        1003: (200, -500, 150),
    }
    float3 xformOp:rotateXYZ = (90, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
}
)");
    sim::Network net;
    const int cam = net.add("usd_camera");
    const int out = net.add("output");
    CHECK(net.setText(cam, "file", path));
    net.setParam(cam, "width", "1200");
    net.setParam(out, "fps", "24");
    net.setParam(out, "frames", "3");
    CHECK(net.connect(cam, "camera", out, "camera"));
    const sim::Compiled c = net.compile();
    CHECK(c.hasCamera);
    CHECK(c.fileAnimation);
    CHECK_EQ(c.poses.size(), 3u);
    // Centimetres, Z up, to metres, Y up; frame 1 is time code 1001.
    CHECK(near(c.cameraAt(1).position, Vec3(0, 1.5f, 5)));
    CHECK(near(c.cameraAt(2).position, Vec3(1, 1.5f, 5)));
    CHECK(near(c.cameraAt(3).position, Vec3(2, 1.5f, 5)));
    // Looking along +y in the file: along -z here, level.
    CHECK(near(c.cameraAt(2).forward(), Vec3(0, 0, -1)));
    CHECK(near(c.cameraAt(2).up(), Vec3(0, 1, 0)));
    // The picture shaped as the film back; the view as wide.
    CHECK_EQ(c.cameraAt(1).height, 800);
    CHECK_NEAR(c.cameraAt(1).focal, 35.0f, 1e-4);
    // At 12 frames a second a frame is two time codes on.
    net.setParam(out, "fps", "12");
    CHECK(near(net.compile().cameraAt(2).position, Vec3(2, 1.5f, 5)));
    // The offset moves it by frames.
    net.setParam(out, "fps", "24");
    net.setParam(cam, "offset", "1");
    CHECK(near(net.compile().cameraAt(1).position, Vec3(1, 1.5f, 5)));
    // A prim that is not a camera says so.
    CHECK(net.setText(cam, "prim", "/nothing"));
    const sim::Compiled bad = net.compile();
    bool said = false;
    for (const sim::Problem& p : bad.problems) said = said || p.message.find("/nothing") != std::string::npos;
    CHECK(said);
}

TEST(usd_import_node_cooks_the_stage_at_the_frame) {
    TempFolder dir("usd_import");
    const std::string path = dir.write("anim.usda", R"(#usda 1.0
(
    metersPerUnit = 1
    startTimeCode = 1
    timeCodesPerSecond = 24
)

def Points "p"
{
    point3f[] points.timeSamples = {
        1: [(0, 0, 0), (1, 0, 0)],
        3: [(0, 2, 0), (1, 2, 0)],
    }
}
)");
    sim::Network net;
    const int n = net.add("usd_import");
    CHECK(net.setText(n, "file", path));
    sim::GeometryGraph graph;
    graph.sync(net);
    const GeometryPtr a = graph.cook(n, 1, 1.0f / 24.0f);
    const GeometryPtr b = graph.cook(n, 2, 1.0f / 24.0f);
    CHECK_EQ(a->pointCount(), 2u);
    CHECK_NEAR(a->positions()[1].y, 0.0f, 0);
    CHECK_NEAR(b->positions()[1].y, 1.0f, 1e-6);
    CHECK_EQ(graph.error(n), std::string());
    // A file that is not there.
    CHECK(net.setText(n, "file", dir / "nowhere.usda"));
    graph.sync(net);
    CHECK_EQ(graph.cook(n, 1, 1.0f / 24.0f)->pointCount(), 0u);
    CHECK(graph.error(n).find("nowhere.usda") != std::string::npos);
}

TEST(usd_what_the_program_writes_it_reads_back) {
    TempFolder dir("usd_back");
    Geometry geo;
    geo.addPoints(4);
    auto P = geo.positionsForWrite();
    P[0] = Vec3(0, 0, 0);
    P[1] = Vec3(1, 0, 0);
    P[2] = Vec3(1, 1, 0);
    P[3] = Vec3(0, 1, 0);
    const uint32_t quad[4] = {0, 1, 2, 3};
    geo.addPrimitive(quad, true);
    std::string error;
    const std::string path = dir / "quad.usda";
    CHECK(io::usda::writeStage(io::usda::geometryStage(geo, "quad"), path, error));
    const auto s = open(path);
    const auto back = usd::importGeometry(*s, 1.0, usd::ImportOptions{});
    CHECK_EQ(back->pointCount(), 4u);
    CHECK_EQ(back->primitiveCount(), 1u);
    for (size_t i = 0; i < 4; ++i) CHECK(near(back->positions()[i], geo.positions()[i]));
}

TEST(usd_import_reads_a_point_instancers_instances) {
    // What is under a PointInstancer is its prototypes, not geometry where
    // it stands: they are the geometry's prototypes, the instances points
    // that stand for them, placed as the instancer places them.
    TempFolder dir("usd_instancer");
    const std::string path = dir.write("chips.usda", R"(#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Xform "W"
{
    def Points "loose"
    {
        point3f[] points = [(1, 2, 3)]
    }

    def PointInstancer "chips" (
        append inactiveIds = [13]
    )
    {
        double3 xformOp:translate = (10, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        point3f[] positions = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (3, 0, 0), (4, 0, 0)]
        quath[] orientations = [(1, 0, 0, 0), (0.70710677, 0, 0.70710677, 0), (1, 0, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
        float3[] scales = [(1, 1, 1), (2, 2, 2), (1, 1, 1), (1, 1, 1), (1, 2, 1)]
        int[] protoIndices = [0, 1, 0, 0, 1]
        int64[] ids = [10, 11, 12, 13, 14]
        int64[] invisibleIds = [12]
        color3f[] primvars:tint = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1)] (
            interpolation = "vertex"
        )
        rel prototypes = [</W/chips/Prototypes/chip>, </W/chips/Prototypes/rock>]

        def Scope "Prototypes"
        {
            def Mesh "chip"
            {
                point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
                int[] faceVertexCounts = [3]
                int[] faceVertexIndices = [0, 1, 2]
            }

            def Xform "rock"
            {
                double3 xformOp:translate = (0, 1, 0)
                uniform token[] xformOpOrder = ["xformOp:translate"]

                def Mesh "m"
                {
                    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
                    int[] faceVertexCounts = [3]
                    int[] faceVertexIndices = [0, 1, 2]
                }
            }
        }
    }
}
)");
    const auto s = open(path);
    CHECK(s != nullptr);
    if (!s) return;
    std::vector<std::string> notes;
    const auto back = usd::importGeometry(*s, 1.0, usd::ImportOptions{}, &notes);
    // The loose point, and an instance each of ids 10, 11 and 14: 12 is
    // invisible, 13 inactive. 14 stretches the rock: a copy of its own.
    CHECK_EQ(back->primitiveCount(), 0u);
    CHECK_EQ(back->pointCount(), 4u);
    CHECK_EQ(back->prototypeCount(), 3u);
    CHECK_EQ(instanceCount(*back), 3u);
    if (back->pointCount() != 4 || back->prototypeCount() != 3) return;
    const auto stands = back->points().find("instance")->read<int32_t>();
    CHECK(stands[0] == -1 && stands[1] == 0 && stands[2] == 1 && stands[3] == 2);
    const auto P = back->positions();
    CHECK(near(P[1], Vec3(10, 0, 0)) && near(P[2], Vec3(11, 0, 0)) && near(P[3], Vec3(14, 0, 0)));
    const auto size = back->points().find("pscale")->read<float>();
    CHECK_NEAR(size[2], 2.0f, 1e-6);
    CHECK_NEAR(size[3], 1.0f, 1e-6);
    const Vec4 turn = back->points().find("orient")->read<Vec4>()[2];
    CHECK(std::abs(std::abs(turn.y) - 0.70710677f) < 1e-3f && std::abs(std::abs(turn.w) - 0.70710677f) < 1e-3f);
    const auto ids = back->points().find("id")->read<int32_t>();
    CHECK(ids[1] == 10 && ids[2] == 11 && ids[3] == 14);
    const auto tint = back->points().find("tint")->read<Vec3>();
    CHECK(near(tint[1], Vec3(1, 0, 0)) && near(tint[2], Vec3(0, 1, 0)) && near(tint[3], Vec3(0, 1, 1)));
    CHECK(std::any_of(notes.begin(), notes.end(),
                      [](const std::string& n) { return n.find("stretch, shear or mirror") != std::string::npos; }));
    // Made into copies: the rock's root's own transform kept, then the
    // instance's scale, turn and place, then the instancer's.
    const auto copies = unpackInstances(*back);
    auto has = [&](const Vec3& want) {
        for (const Vec3& p : copies->positions()) {
            if (glm::length(p - want) < 2e-3f) return true;
        }
        return false;
    };
    CHECK(has(Vec3(10, 0, 0)) && has(Vec3(11, 0, 0)) && has(Vec3(10, 1, 0)));  // the chip
    CHECK(has(Vec3(11, 2, 0)) && has(Vec3(11, 2, -2)) && has(Vec3(11, 4, 0)));  // the rock, turned
    CHECK(has(Vec3(14, 2, 0)) && has(Vec3(15, 2, 0)) && has(Vec3(14, 4, 0)));  // the rock, stretched
    CHECK_EQ(copies->primitiveCount(), 3u);
    // Read again only when something of them changes in time.
    CHECK(!usd::geometryVaries(*s, usd::ImportOptions{}));
    const std::string moving = dir.write("moving.usda", R"(#usda 1.0
def PointInstancer "chips"
{
    point3f[] positions.timeSamples = {
        1: [(0, 0, 0)],
        2: [(1, 0, 0)],
    }
    int[] protoIndices = [0]
    rel prototypes = </chips/Prototypes/chip>
    def Scope "Prototypes"
    {
        def Mesh "chip"
        {
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
        }
    }
}
)");
    const auto m = open(moving);
    CHECK(m && usd::geometryVaries(*m, usd::ImportOptions{}));
}

TEST(usd_import_reads_back_the_instances_the_program_writes) {
    // A clump on three points -- turned, sized, tinted --, written as a
    // PointInstancer and read back: the same instances.
    Geometry geo;
    auto clump = std::make_shared<Geometry>();
    clump->addPoints(3);
    auto C = clump->positionsForWrite();
    C[0] = Vec3(0, 0, 0), C[1] = Vec3(0.2f, 0, 0), C[2] = Vec3(0, 0.5f, 0);
    const uint32_t tri[3] = {0, 1, 2};
    clump->addPrimitive(tri, true);
    geo.addPrototype(clump);
    geo.addPoints(3);
    auto P = geo.positionsForWrite();
    P[0] = Vec3(1, 0, 0), P[1] = Vec3(0, 0, 2), P[2] = Vec3(-1, 0.5f, 0);
    auto stands = geo.points().create("instance", AttrType::Int).write<int32_t>();
    std::fill(stands.begin(), stands.end(), 0);
    auto turn = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
    turn[0] = Vec4(0, 0, 0, 1);
    turn[1] = Vec4(0, 0.38268343f, 0, 0.9238795f);  // 45 degrees about y
    turn[2] = Vec4(0.5f, 0.5f, 0.5f, 0.5f);
    auto size = geo.points().create("pscale", AttrType::Float).write<float>();
    size[0] = 1.0f, size[1] = 0.5f, size[2] = 2.0f;
    auto tint = geo.points().create("tint", AttrType::Vec3).write<Vec3>();
    tint[0] = Vec3(1, 1, 1), tint[1] = Vec3(0.8f, 1, 0.6f), tint[2] = Vec3(0.5f, 0.5f, 0.5f);
    TempFolder dir("usd_instances_back");
    const std::string path = (dir.path / "clumps.usda").string();
    std::string error;
    CHECK(io::usda::writeStage(io::usda::geometryStage(geo, "clumps"), path, error));
    const auto s = open(path);
    CHECK(s != nullptr);
    if (!s) return;
    const auto back = usd::importGeometry(*s, 0.0, usd::ImportOptions{});
    CHECK_EQ(back->prototypeCount(), 1u);
    CHECK_EQ(instanceCount(*back), 3u);
    if (instanceCount(*back) != 3 || back->prototypeCount() != 1) return;
    CHECK_EQ(back->prototypes()[0]->pointCount(), 3u);
    const auto was = placementsOf(geo), is = placementsOf(*back);
    const auto tints = back->points().find("tint")->read<Vec3>();
    for (size_t i = 0; i < 3; ++i) {
        CHECK(near(was[i].at, is[i].at));
        CHECK_NEAR(was[i].scale, is[i].scale, 1e-5);
        // The same turn, whichever sign the quaternion has.
        for (const Vec3& corner : clump->positions()) CHECK(near(was[i].point(corner), is[i].point(corner)));
        CHECK(near(tints[i], tint[i]));
    }
}

TEST(usd_import_reads_a_volumes_fields_from_their_vdb_files) {
    // A puff of smoke and a wind through it in a VDB file, in centimetres,
    // z up: a Volume moved along x, and the same puff in one turned about z.
    TempFolder dir("usd_volume");
    const int n = 16;
    std::vector<float> d(static_cast<size_t>(n * n * n)), wx(d.size(), 1.0f), wy(d.size(), 2.0f), wz(d.size(), 3.0f);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const float r2 = static_cast<float>((i - 7.5) * (i - 7.5) + (j - 7.5) * (j - 7.5) + (k - 7.5) * (k - 7.5));
                d[static_cast<size_t>(i + n * (j + n * k))] = std::exp(-r2 / 9.0f);
            }
        }
    }
    const std::vector<Volume> grids = {Volume::make("density", Vec3(0, 0, 0), 2.0f, n, n, n, d),
                                       Volume::make("vel.x", Vec3(0, 0, 0), 2.0f, n, n, n, wx),
                                       Volume::make("vel.y", Vec3(0, 0, 0), 2.0f, n, n, n, wy),
                                       Volume::make("vel.z", Vec3(0, 0, 0), 2.0f, n, n, n, wz)};
    std::string error;
    CHECK(io::writeVdb(grids, (dir.path / "puff.vdb").string(), error));
    const std::string path = dir.write("puff.usda", R"(#usda 1.0
(
    metersPerUnit = 0.01
    upAxis = "Z"
)

def Volume "puff"
{
    double3 xformOp:translate = (100, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    rel field:density = </puff/density>
    rel field:wind = </puff/vel>

    def OpenVDBAsset "density"
    {
        asset filePath = @puff.vdb@
        token fieldName = "density"
    }

    def OpenVDBAsset "vel"
    {
        asset filePath = @./puff.vdb@
        token fieldName = "vel"
        token fieldDataType = "float3"
    }
}

def Volume "turned"
{
    float xformOp:rotateZ = 30
    uniform token[] xformOpOrder = ["xformOp:rotateZ"]
    rel field:density = </turned/density>

    def OpenVDBAsset "density"
    {
        asset filePath = @puff.vdb@
        token fieldName = "density"
    }
}
)");
    const auto s = open(path);
    CHECK(s != nullptr);
    if (!s) return;
    std::vector<std::string> notes;
    const auto geo = usd::importGeometry(*s, 1.0, usd::ImportOptions{}, &notes);
    CHECK(!usd::geometryVaries(*s, usd::ImportOptions{}));
    std::map<std::string, const Volume*> byName;
    for (const Volume& v : geo->volumes()) byName[v.name] = &v;
    CHECK_EQ(geo->volumes().size(), 5u);  // density, wind.x, wind.y, wind.z; the turned density
    if (!byName.count("density") || !byName.count("wind.y")) return;
    // Moved a metre along x, in metres, y up: its middle -- (16, 16, 16) cm
    // in the file -- at (1.16, 0.16, -0.16), voxels of 2 cm.
    const Volume& puff = geo->volumes()[0];
    CHECK_EQ(puff.name, std::string("density"));
    CHECK_NEAR(puff.voxel, 0.02f, 1e-7);
    // Between the eight voxels round the middle, each exp(-0.75 / 9).
    const float top = std::exp(-0.75f / 9.0f);
    CHECK_NEAR(puff.sample(Vec3(1.16f, 0.16f, -0.16f)), top, 1e-5);
    CHECK_NEAR(puff.sample(Vec3(1.16f, 0.16f, 0.16f)), 0.0f, 1e-6);
    // The wind turned with the world and made metres a second: (1, 2, 3)
    // cm/s to (0.01, 0.03, -0.02).
    CHECK_NEAR(byName["wind.x"]->sample(Vec3(1.16f, 0.16f, -0.16f)), 0.01f, 1e-6);
    CHECK_NEAR(byName["wind.y"]->sample(Vec3(1.16f, 0.16f, -0.16f)), 0.03f, 1e-6);
    CHECK_NEAR(byName["wind.z"]->sample(Vec3(1.16f, 0.16f, -0.16f)), -0.02f, 1e-6);
    // Turned off the axes: laid out anew, as much smoke, its middle turned.
    const Volume& turned = geo->volumes()[4];
    double mass = 0.0, was = 0.0;
    for (const float x : *turned.values) mass += x;
    for (const float x : d) was += x;
    mass *= static_cast<double>(turned.voxel) * turned.voxel * turned.voxel;
    was *= 0.02 * 0.02 * 0.02;
    CHECK(std::abs(mass - was) < 0.03 * was);
    const float c = std::cos(0.5235988f), sn = std::sin(0.5235988f);
    const Vec3 middle(0.16f * c - 0.16f * sn, 0.16f, -(0.16f * sn + 0.16f * c));
    // Laid out anew and sampled again -- trilinear twice --, a little lower.
    CHECK_NEAR(turned.sample(middle), top, 0.05);
    CHECK(std::any_of(notes.begin(), notes.end(), [](const std::string& w) { return w.find("resampled") != std::string::npos; }));
}

TEST(usd_corners_normals_go_out_and_back_as_face_varying) {
    // A fold whose corners have normals of their own: written as faceVarying
    // normals, read back onto the corners.
    Geometry geo;
    geo.addPoints(6);
    auto P = geo.positionsForWrite();
    P[0] = Vec3(0, 0, 0), P[1] = Vec3(1, 0, 0), P[2] = Vec3(1, 0, 1), P[3] = Vec3(0, 0, 1);
    P[4] = Vec3(2, 0.5f, 0), P[5] = Vec3(2, 0.5f, 1);
    const uint32_t a[4] = {0, 3, 2, 1}, b[4] = {1, 2, 5, 4};
    geo.addPrimitive(a, true);
    geo.addPrimitive(b, true);
    auto N = geo.vertices().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t k = 0; k < 8; ++k) N[k] = normalize(k < 4 ? Vec3(0, 1, 0) : Vec3(-0.5f, 1, 0));
    TempFolder dir("usd_corner_normals");
    const std::string path = (dir.path / "fold.usda").string();
    std::string error;
    CHECK(io::usda::writeStage(io::usda::geometryStage(geo, "fold"), path, error));
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("normal3f[] normals = [") != std::string::npos);
    CHECK(text.str().find("interpolation = \"faceVarying\"") != std::string::npos);
    const auto s = open(path);
    CHECK(s != nullptr);
    if (!s) return;
    const auto back = usd::importGeometry(*s, 0.0, usd::ImportOptions{});
    const AttributeArray* own = back->vertices().find("N");
    CHECK(own && own->size() == 8 && !back->points().find("N"));
    if (!own || own->size() != 8) return;
    for (size_t k = 0; k < 8; ++k) CHECK(near(own->read<Vec3>()[k], N[k]));
}

TEST(usd_broken_files_are_refused_not_crashed_on) {
    std::vector<uint8_t> good;
    std::string error;
    CHECK(usd::readFileBytes(kData + "values.usdc", good, error));
    std::mt19937 random(7);
    int refused = 0;
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<uint8_t> bytes = good;
        if (trial % 3 == 0) {
            bytes.resize(random() % bytes.size());
        } else {
            for (int k = 0; k < 1 + trial % 8; ++k) bytes[random() % bytes.size()] ^= static_cast<uint8_t>(1u << (random() % 8));
        }
        usd::Layer layer;
        refused += usd::readLayerBytes(bytes, layer, error) ? 0 : 1;
    }
    CHECK(refused > 50);
    std::vector<uint8_t> textBytes;
    CHECK(usd::readFileBytes(kData + "values.usda", textBytes, error));
    for (int trial = 0; trial < 200; ++trial) {
        std::vector<uint8_t> bytes = textBytes;
        bytes.resize(8 + random() % (bytes.size() - 8));
        usd::Layer layer;
        (void)usd::readLayerBytes(bytes, layer, error);
    }
}
