//
// The shader graph: its node library, its file format, the generator and the
// targets -- and above all that it can be extended without touching C++:
// a node is text, a language is one class.
//
// Whether the generated code actually compiles is checked by `pgshader check`,
// which runs every node through glslangValidator for every target (CTest runs
// it when the tool is installed).
//
#include "pg/shader/Generator.h"

#include "test_framework.h"

#include <algorithm>
#include <sstream>

using namespace pg::shader;

namespace {

const NodeLibrary& builtins() {
    static const NodeLibrary lib = NodeLibrary::withBuiltins();
    return lib;
}

const Target& target(const char* name) {
    const Target* t = TargetRegistry::instance().find(name);
    if (!t) ::testing::fail(__FILE__, __LINE__, std::string("no target ") + name);
    return *t;
}

bool contains(const std::string& text, const std::string& what) {
    return text.find(what) != std::string::npos;
}

void link(ShaderGraph& g, int from, const char* out, int to, const char* in,
          const NodeLibrary& lib = builtins()) {
    std::string error;
    if (!g.connect(from, out, to, in, lib, &error)) {
        ::testing::fail(__FILE__, __LINE__, "connect failed: " + error);
    }
}

/// Generates and fails the test on any error.
GeneratedShader build(const ShaderGraph& g, const char* targetName, const NodeLibrary& lib = builtins()) {
    GeneratedShader s = generate(g, lib, target(targetName));
    if (!s.ok()) {
        std::string msg = "generation failed:";
        for (const auto& e : s.errors) msg += "\n  node " + std::to_string(e.node) + ": " + e.message;
        ::testing::fail(__FILE__, __LINE__, msg);
    }
    return s;
}

std::string fragment(const GeneratedShader& s) { return s.fileFor(Stage::Fragment)->text; }
std::string vertex(const GeneratedShader& s) { return s.fileFor(Stage::Vertex)->text; }

/// checker -> lambert -> output, the graph most tests start from.
ShaderGraph litChecker(int* checker = nullptr, int* output = nullptr) {
    ShaderGraph g;
    const int c = g.addNode("checker", 0, 0, &builtins());
    const int l = g.addNode("lambert", 200, 0, &builtins());
    const int o = g.addNode("surface_output", 400, 0, &builtins());
    link(g, c, "color", l, "color");
    link(g, l, "result", o, "color");
    if (checker) *checker = c;
    if (output) *output = o;
    return g;
}

}  // namespace

// --- the library ---------------------------------------------------------------

TEST(the_builtin_library_defines_the_basic_nodes_as_text) {
    const NodeLibrary& lib = builtins();
    CHECK(lib.size() >= 30u);
    for (const char* name : {"uv", "time", "add", "multiply", "mix", "dot", "texture", "checker",
                             "noise", "lambert", "fresnel", "surface_output"}) {
        CHECK(lib.find(name) != nullptr);
    }
    const NodeDef* mix = lib.find("mix");
    CHECK_EQ(mix->inputs.size(), 3u);
    CHECK_EQ(mix->inputs[2].type, Type::Float);
    CHECK_EQ(mix->outputs[0].type, Type::Any);
    CHECK(lib.find("surface_output")->isOutput);
    CHECK(lib.find("texture")->outputs[0].perTarget.count("hlsl") == 1u);

    const auto cats = lib.categories();
    CHECK(std::find(cats.begin(), cats.end(), "Math") != cats.end());
    CHECK(std::find(cats.begin(), cats.end(), "Lighting") != cats.end());
}

TEST(a_user_library_adds_nodes_and_overrides_builtin_ones) {
    NodeLibrary lib = NodeLibrary::withBuiltins();
    std::string error;
    const bool ok = lib.load(R"(
        node glow
            label Glow
            category Custom
            in color vec3 = 1 0.5 0 color
            in strength float = 3.0
            out result vec3 = {color} * {strength}

        node add
            label Plus
            category Math
            in a any = 0.0
            in b any = 0.0
            out result any = ({a}) + ({b})
    )", "mine.pgnodes", error);
    CHECK(ok);
    CHECK(error.empty());
    CHECK_EQ(lib.find("glow")->category, "Custom");
    CHECK_EQ(lib.find("add")->label, "Plus");

    ShaderGraph g;
    const int glow = g.addNode("glow", 0, 0, &lib);
    const int out = g.addNode("surface_output", 200, 0, &lib);
    link(g, glow, "result", out, "color", lib);
    const std::string fs = fragment(build(g, "glsl330", lib));
    CHECK(contains(fs, "vec3 n1_result = vec3(1.0, 0.5, 0.0) * 3.0;"));
    CHECK(contains(fs, "o_color = vec4(n1_result, 1.0);"));
}

TEST(library_errors_name_the_file_and_line_and_change_nothing) {
    NodeLibrary lib = NodeLibrary::withBuiltins();
    const size_t before = lib.size();
    std::string error;

    CHECK(!lib.load("node a\n    out x float = 1.0\n    wobble 3\n", "bad.pgnodes", error));
    CHECK(contains(error, "bad.pgnodes:3: unknown keyword 'wobble'"));

    CHECK(!lib.load("node a\n    in p float\n    out x float = {q} * 2.0\n", "bad.pgnodes", error));
    CHECK(contains(error, "bad.pgnodes:3:"));
    CHECK(contains(error, "{q}"));

    CHECK(!lib.load("node a\n    in p float = $nowhere\n    out x float = {p}\n", "bad.pgnodes", error));
    CHECK(contains(error, "unknown global '$nowhere'"));

    CHECK(!lib.load("node a\n    in p vec3 = 1 2\n    out x vec3 = {p}\n", "bad.pgnodes", error));
    CHECK(contains(error, "takes 1 or 3 numbers"));

    CHECK(!lib.load("node a\n    in p float\n    out x any = {p}\n", "bad.pgnodes", error));
    CHECK(contains(error, "no input is"));

    CHECK(!lib.load("node fine\n    out x float = 1.0\nnode broken\n    out y vec9 = 1.0\n", "b.pgnodes",
                    error));
    CHECK_EQ(lib.size(), before);  // not even the valid first node was added
    CHECK(lib.find("fine") == nullptr);
}

TEST(templates_split_into_code_placeholders_and_globals) {
    std::vector<TemplatePiece> pieces;
    std::string error;
    CHECK(parseTemplate("mix({a}, $uv.x, 0.5)", pieces, error));
    CHECK_EQ(pieces.size(), 5u);
    CHECK_EQ(pieces[0].text, "mix(");
    CHECK(pieces[1].kind == TemplatePiece::Kind::Placeholder);
    CHECK_EQ(pieces[1].text, "a");
    CHECK(pieces[3].kind == TemplatePiece::Kind::Global);
    CHECK_EQ(pieces[3].text, "uv");
    CHECK_EQ(pieces[4].text, ".x, 0.5)");

    CHECK(!parseTemplate("{a + 1", pieces, error));
    CHECK(!parseTemplate("a }", pieces, error));
    CHECK(!parseTemplate("$ + 1", pieces, error));
}

// --- the graph -------------------------------------------------------------------

TEST(a_graph_round_trips_through_its_text_format) {
    int checker = 0;
    ShaderGraph g = litChecker(&checker);
    g.setInput(checker, "scale", Value::scalar(12.0f));
    g.setInput(checker, "color2", Value::vector(0.9f, 0.4f, 0.1f));
    const int p = g.addNode("float_parameter", 10, 300, &builtins());
    g.setParam(p, "name", "roughness");

    const std::string text = g.save();
    ShaderGraph loaded;
    std::string error;
    CHECK(ShaderGraph::load(text, loaded, error));
    CHECK_EQ(loaded.save(), text);
    CHECK(contains(text, "  in scale 12.0\n"));
    CHECK(contains(text, "  in color2 0.9 0.4 0.1\n"));
    CHECK(contains(text, "  param name roughness\n"));
    CHECK(contains(text, "link 1.color -> 2.color\n"));

    // New nodes continue after the highest id in the file.
    CHECK_EQ(loaded.addNode("uv"), 5);

    CHECK(!ShaderGraph::load("pgshadergraph 99\n", loaded, error));
    CHECK(contains(error, "version 99"));
    CHECK(!ShaderGraph::load("pgshadergraph 1\nnode 1 uv 1\nlink 1.uv -> 7.uv\n", loaded, error));
    CHECK(contains(error, "line 3"));
}

TEST(connect_replaces_a_link_and_refuses_loops_and_type_errors) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int a = g.addNode("add", 0, 0, &lib);
    const int b = g.addNode("add", 100, 0, &lib);
    const int c = g.addNode("constant", 0, 100, &lib);
    std::string error;

    CHECK(g.connect(a, "result", b, "a", lib, &error));
    CHECK(!g.connect(b, "result", a, "a", lib, &error));  // b already reads a
    CHECK(contains(error, "loop"));
    CHECK(!g.connect(a, "result", a, "b", lib, &error));
    CHECK(!g.connect(c, "nope", b, "b", lib, &error));
    CHECK(contains(error, "no output 'nope'"));

    CHECK(g.connect(c, "result", b, "a", lib, &error));  // replaces a -> b.a
    CHECK_EQ(g.links().size(), 1u);
    CHECK_EQ(g.linkInto(b, "a")->fromNode, c);

    CHECK(g.removeNode(c));
    CHECK(g.links().empty());
}

// --- the generator -----------------------------------------------------------------

TEST(only_nodes_that_reach_the_output_are_compiled) {
    ShaderGraph g = litChecker();
    const int dangling = g.addNode("noise", 0, 300, &builtins());
    const std::string fs = fragment(build(g, "glsl330"));
    CHECK(!contains(fs, "n" + std::to_string(dangling) + "_"));
    CHECK(!contains(fs, "pg_noise"));
    CHECK(contains(fs, "pg_checker"));
}

TEST(any_ports_take_the_widest_type_connected_to_them) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int color = g.addNode("color", 0, 0, &lib);
    const int k = g.addNode("constant", 0, 100, &lib);
    const int add = g.addNode("add", 200, 0, &lib);
    const int out = g.addNode("surface_output", 400, 0, &lib);
    link(g, color, "color", add, "a");
    link(g, k, "result", add, "b");
    link(g, add, "result", out, "color");

    const std::string fs = fragment(build(g, "glsl330"));
    CHECK(contains(fs, "vec3 n3_result = n1_color + vec3(n2_result);"));
    CHECK(contains(fs, "o_color = vec4(n3_result, 1.0);"));

    const std::string hlsl = fragment(build(g, "hlsl"));
    CHECK(contains(hlsl, "float3 n3_result = n1_color + ((float3)(n2_result));"));
}

TEST(conversions_truncate_pad_and_splat) {
    const Target& glsl = target("glsl330");
    CHECK_EQ(glsl.convert("v", Type::Vec4, Type::Vec3), "v.xyz");
    CHECK_EQ(glsl.convert("a + b", Type::Vec3, Type::Float), "(a + b).x");
    CHECK_EQ(glsl.convert("v", Type::Vec3, Type::Vec4), "vec4(v, 1.0)");
    CHECK_EQ(glsl.convert("v", Type::Vec2, Type::Vec4), "vec4(v, 0.0, 1.0)");
    CHECK_EQ(glsl.convert("x", Type::Float, Type::Vec3), "vec3(x)");
    CHECK_EQ(glsl.literal(Value::scalar(-0.5f), Type::Float), "(-0.5)");
    CHECK_EQ(glsl.literal(Value::scalar(2.0f), Type::Vec2), "vec2(2.0, 2.0)");

    const Target& hlsl = target("hlsl");
    CHECK_EQ(hlsl.convert("v", Type::Vec3, Type::Vec4), "float4(v, 1.0)");
    CHECK_EQ(hlsl.convert("x", Type::Float, Type::Vec3), "((float3)(x))");
}

TEST(hlsl_renames_types_and_intrinsics_but_not_members_or_numbers) {
    const Target& hlsl = target("hlsl");
    CHECK_EQ(hlsl.translate("mix(a.mix, vec3(1e-5), fract(x)) // mix"),
             "lerp(a.mix, float3(1e-5), frac(x)) // mix");
    CHECK_EQ(hlsl.translate("vec2 mixer = vec2(1.5e+3, .5);"), "float2 mixer = float2(1.5e+3, .5);");
    CHECK_EQ(target("glsl330").translate("mix(a, b, t)"), "mix(a, b, t)");
}

TEST(a_node_can_give_one_target_its_own_template) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int tex = g.addNode("texture", 0, 0, &lib);
    const int out = g.addNode("surface_output", 200, 0, &lib);
    g.setParam(tex, "name", "albedo");
    link(g, tex, "rgba", out, "color");

    const GeneratedShader gl = build(g, "glsl330");
    CHECK(contains(fragment(gl), "vec4 n1_rgba = texture(u_albedo, g_uv);"));
    CHECK(contains(fragment(gl), "uniform sampler2D u_albedo;"));
    CHECK_EQ(gl.uniforms.size(), 1u);
    CHECK_EQ(gl.uniforms[0].binding, 0);

    const std::string hlsl = fragment(build(g, "hlsl"));
    CHECK(contains(hlsl, "float4 n1_rgba = u_albedo.Sample(u_albedo_sampler, g_uv);"));
    CHECK(contains(hlsl, "Texture2D u_albedo : register(t0);"));
    CHECK(contains(hlsl, "SamplerState u_albedo_sampler : register(s0);"));

    CHECK(contains(fragment(build(g, "vulkan")), "layout(set = 0, binding = 1) uniform sampler2D u_albedo;"));
}

TEST(outputs_nobody_reads_are_not_written) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int tex = g.addNode("texture", 0, 0, &lib);
    const int out = g.addNode("surface_output", 200, 0, &lib);
    link(g, tex, "rgb", out, "color");
    const std::string fs = fragment(build(g, "glsl330"));
    CHECK(contains(fs, "n1_rgba"));        // rgb is made from rgba
    CHECK(contains(fs, "vec3 n1_rgb = n1_rgba.rgb;"));
    CHECK(!contains(fs, "n1_alpha"));
}

TEST(parameter_nodes_become_uniforms_with_defaults) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int rough = g.addNode("float_parameter", 0, 0, &lib);
    const int tint = g.addNode("color_parameter", 0, 100, &lib);
    const int mul = g.addNode("multiply", 200, 0, &lib);
    const int out = g.addNode("surface_output", 400, 0, &lib);
    g.setParam(rough, "name", "a_rough");
    g.setParam(rough, "default", "0.25");
    g.setParam(tint, "name", "b_tint");
    link(g, tint, "color", mul, "a");
    link(g, rough, "value", mul, "b");
    link(g, mul, "result", out, "color");

    const GeneratedShader gl = build(g, "glsl330");
    CHECK_EQ(gl.uniforms.size(), 2u);
    CHECK_EQ(gl.uniforms[0].name, "u_a_rough");
    CHECK_EQ(gl.uniforms[0].defaultValue.v[0], 0.25f);
    CHECK_EQ(gl.uniforms[1].type, Type::Vec3);
    CHECK_EQ(gl.uniforms[1].defaultValue.v[1], 0.5f);
    CHECK(contains(fragment(gl), "uniform float u_a_rough;"));
    CHECK(contains(fragment(gl), "uniform vec3 u_b_tint;"));
    CHECK(!contains(vertex(gl), "u_a_rough"));  // only the stage that reads it declares it

    // Vulkan packs them into the std140 block after the renderer's uniforms.
    const std::string vk = fragment(build(g, "vulkan"));
    CHECK(contains(vk, "vec3 u_lightDir;  // offset 144"));
    CHECK(contains(vk, "float u_a_rough;  // offset 156"));
    CHECK(contains(vk, "vec3 u_b_tint;  // offset 160"));
    CHECK(contains(vk, "};  // 172 bytes"));
}

TEST(uniform_names_are_checked) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int a = g.addNode("float_parameter", 0, 0, &lib);
    const int b = g.addNode("color_parameter", 0, 100, &lib);
    const int add = g.addNode("add", 200, 0, &lib);
    const int out = g.addNode("surface_output", 400, 0, &lib);
    g.setParam(a, "name", "same");
    g.setParam(b, "name", "same");
    link(g, a, "value", add, "a");
    link(g, b, "color", add, "b");
    link(g, add, "result", out, "color");
    GeneratedShader s = generate(g, lib, target("glsl330"));
    CHECK(!s.ok());
    CHECK(contains(s.errors[0].message, "both float and vec3"));

    g.setParam(b, "name", "not a name");
    s = generate(g, lib, target("glsl330"));
    CHECK(!s.ok());
    CHECK_EQ(s.errors[0].node, b);
}

TEST(an_offset_puts_its_nodes_in_the_vertex_stage) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int time = g.addNode("time", 0, 0, &lib);
    const int wave = g.addNode("sine", 100, 0, &lib);
    const int normal = g.addNode("normal", 100, 100, &lib);
    const int scale = g.addNode("multiply", 200, 0, &lib);
    const int out = g.addNode("surface_output", 400, 0, &lib);
    link(g, time, "time", wave, "x");
    link(g, wave, "result", scale, "a");
    link(g, normal, "normal", scale, "b");
    link(g, scale, "result", out, "offset");

    const GeneratedShader s = build(g, "glsl330");
    const std::string vs = vertex(s), fs = fragment(s);
    CHECK(contains(vs, "float n2_result = sin(n1_time);"));
    CHECK(contains(vs, "g_position += n4_result;"));
    CHECK(contains(vs, "uniform float u_time;"));
    CHECK(!contains(fs, "u_time"));
    CHECK(contains(fs, "o_color = vec4(0.8, 0.8, 0.8, 1.0);"));
}

TEST(what_the_fragment_stage_reads_is_passed_on_by_the_vertex_stage) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int fres = g.addNode("fresnel", 0, 0, &lib);
    const int out = g.addNode("surface_output", 200, 0, &lib);
    link(g, fres, "result", out, "color");
    const GeneratedShader s = build(g, "vulkan");
    // $view needs the position, $normal the normal; nobody reads the UVs.
    CHECK(contains(vertex(s), "layout(location = 0) out vec3 v_position;"));
    CHECK(contains(vertex(s), "layout(location = 1) out vec3 v_normal;"));
    CHECK(!contains(vertex(s), "v_uv"));
    CHECK(contains(fragment(s), "vec3 g_view = normalize(u_cameraPos - v_position);"));
}

TEST(helper_functions_come_once_and_callees_first) {
    ShaderGraph g;
    const NodeLibrary& lib = builtins();
    const int n1 = g.addNode("noise", 0, 0, &lib);
    const int n2 = g.addNode("noise", 0, 100, &lib);
    const int add = g.addNode("add", 200, 0, &lib);
    const int out = g.addNode("surface_output", 400, 0, &lib);
    g.setInput(n2, "scale", Value::scalar(16.0f));
    link(g, n1, "value", add, "a");
    link(g, n2, "value", add, "b");
    link(g, add, "result", out, "color");
    const std::string fs = fragment(build(g, "glsl330"));
    const size_t hash = fs.find("float pg_hash(");
    const size_t noise = fs.find("float pg_noise(");
    CHECK(hash != std::string::npos && noise != std::string::npos);
    CHECK(hash < noise);
    CHECK_EQ(fs.find("float pg_noise(", noise + 1), std::string::npos);
    CHECK(contains(fs, "o_color = vec4(n3_result);"));  // float -> vec4 splat
}

TEST(broken_graphs_are_diagnosed_per_node) {
    const NodeLibrary& lib = builtins();
    ShaderGraph g;
    g.addNode("checker", 0, 0, &lib);
    GeneratedShader s = generate(g, lib, target("glsl330"));
    CHECK(!s.ok());
    CHECK(contains(s.errors[0].message, "no output node"));
    CHECK(s.files.empty());

    ShaderGraph h;
    std::string error;
    CHECK(ShaderGraph::load("pgshadergraph 1\n"
                            "node 1 wobbler 1\n"
                            "node 2 surface_output 1\n"
                            "node 3 surface_output 1\n"
                            "link 1.out -> 2.color\n",
                            h, error));
    s = generate(h, lib, target("glsl330"));
    CHECK(!s.ok());
    CHECK_EQ(s.errors[0].node, 3);
    CHECK(contains(s.errors[0].message, "second one"));

    h.removeNode(3);
    s = generate(h, lib, target("glsl330"));
    CHECK_EQ(s.errors[0].node, 1);
    CHECK(contains(s.errors[0].message, "unknown node type 'wobbler'"));
}

TEST(the_same_graph_always_generates_the_same_text) {
    ShaderGraph g = litChecker();
    ShaderGraph copy;
    std::string error;
    CHECK(ShaderGraph::load(g.save(), copy, error));
    for (const Target* t : TargetRegistry::instance().all()) {
        const GeneratedShader a = build(g, t->name().c_str());
        const GeneratedShader b = build(copy, t->name().c_str());
        CHECK_EQ(a.files.size(), b.files.size());
        for (size_t i = 0; i < a.files.size(); ++i) CHECK_EQ(a.files[i].text, b.files[i].text);
    }
}

// --- extensibility: a new language is one class --------------------------------

namespace {

/// A toy target: writes every stage as a list of assignments. Enough to show
/// that nothing but this class is needed for a new language.
class ListingTarget : public Target {
public:
    std::string name() const override { return "test-listing"; }
    std::string description() const override { return "listing for tests"; }
    std::string translate(const std::string& code) const override {
        return renameIdentifiers(code, {{"mix", "blend"}});
    }
    std::vector<ShaderFile> assemble(const Assembly& a) const override {
        std::ostringstream o;
        for (const auto& s : a.fragment.statements) o << s.name << " := " << s.expression << '\n';
        o << "color := " << a.fragment.result << '\n';
        return {ShaderFile{".txt", o.str(), {{Stage::Vertex, "none"}, {Stage::Fragment, "all"}}}};
    }
};

}  // namespace

TEST(a_new_target_plugs_in_as_one_class) {
    TargetRegistry::instance().add(std::make_unique<ListingTarget>());
    const Target* t = TargetRegistry::instance().find("test-listing");
    CHECK(t != nullptr);

    const GeneratedShader s = generate(litChecker(), builtins(), *t);
    CHECK(s.ok());
    const std::string text = s.files[0].text;
    CHECK(contains(text, "n1_color := blend(vec3(0.15, 0.15, 0.15)"));
    CHECK(contains(text, "color := vec4(n2_result, 1.0)"));
    CHECK_EQ(s.entryPoint(Stage::Fragment), "all");
}

TEST(generated_files_are_named_after_the_graph_and_the_target) {
    CHECK_EQ(outputFileName("marble", "vulkan", ShaderFile{".frag", "", {}}), "marble.vulkan.frag");
    CHECK_EQ(outputFileName("marble", "hlsl", ShaderFile{".hlsl", "", {}}), "marble.hlsl");
}
