#include "pg/io/MaterialX.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>

namespace pg::io::mtlx {
namespace {

std::string escaped(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescaped(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t end = s.find(';', i);
        if (end == std::string::npos) {
            out += s[i];
            continue;
        }
        const std::string e = s.substr(i + 1, end - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else out += s.substr(i, end - i + 1);
        i = end;
    }
    return out;
}

/// An element of the XML: its tag and attributes; children follow it in
/// the order they come, each one level deeper.
struct Element {
    std::string tag;
    std::map<std::string, std::string> attributes;
    int depth = 0;
};

/// The elements of `text`, in order, each with how deep it is; comments,
/// declarations and text between them skipped. False for XML that does
/// not close as it opens.
bool elements(const std::string& text, std::vector<Element>& out, std::string& error) {
    size_t i = 0;
    int depth = 0;
    std::vector<std::string> open;
    while ((i = text.find('<', i)) != std::string::npos) {
        if (text.compare(i, 4, "<!--") == 0) {
            const size_t end = text.find("-->", i);
            if (end == std::string::npos) return error = "a comment that does not end", false;
            i = end + 3;
            continue;
        }
        if (text.compare(i, 2, "<?") == 0 || text.compare(i, 2, "<!") == 0) {
            const size_t end = text.find('>', i);
            if (end == std::string::npos) return error = "a declaration that does not end", false;
            i = end + 1;
            continue;
        }
        const size_t end = text.find('>', i);
        if (end == std::string::npos) return error = "a tag that does not end", false;
        std::string body = text.substr(i + 1, end - i - 1);
        i = end + 1;
        if (!body.empty() && body[0] == '/') {
            std::string tag = body.substr(1);
            while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back()))) tag.pop_back();
            if (open.empty() || open.back() != tag) return error = "</" + tag + "> closes what it did not open", false;
            open.pop_back();
            --depth;
            continue;
        }
        const bool selfClosing = !body.empty() && body.back() == '/';
        if (selfClosing) body.pop_back();
        Element e;
        e.depth = depth;
        size_t k = 0;
        while (k < body.size() && !std::isspace(static_cast<unsigned char>(body[k]))) e.tag += body[k++];
        // Attributes: name="value" (or 'value').
        while (k < body.size()) {
            while (k < body.size() && std::isspace(static_cast<unsigned char>(body[k]))) ++k;
            if (k >= body.size()) break;
            std::string name;
            while (k < body.size() && body[k] != '=' && !std::isspace(static_cast<unsigned char>(body[k]))) name += body[k++];
            while (k < body.size() && (std::isspace(static_cast<unsigned char>(body[k])) || body[k] == '=')) ++k;
            if (k >= body.size() || (body[k] != '"' && body[k] != '\'')) return error = "an attribute without a value in <" + e.tag + ">", false;
            const char quote = body[k++];
            const size_t close = body.find(quote, k);
            if (close == std::string::npos) return error = "an attribute that does not end in <" + e.tag + ">", false;
            e.attributes[name] = unescaped(body.substr(k, close - k));
            k = close + 1;
        }
        out.push_back(std::move(e));
        if (!selfClosing) {
            open.push_back(out.back().tag);
            ++depth;
        }
    }
    if (!open.empty()) return error = "<" + open.back() + "> is not closed", false;
    return true;
}

std::string attributeOf(const Element& e, const char* name) {
    const auto it = e.attributes.find(name);
    return it == e.attributes.end() ? std::string() : it->second;
}

}  // namespace

std::string number(float x) {
    if (!std::isfinite(x)) x = 0.0f;
    if (x == 0.0f) x = 0.0f;  // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, x);
    return std::string(buf, r.ptr);
}

std::string numbers(const Vec3& v) { return number(v.x) + ", " + number(v.y) + ", " + number(v.z); }

std::string nodeDef(const Node& n) {
    // Its version of 1.38: normalmap's scale a float.
    if (n.category == "normalmap") return "ND_normalmap";
    // Of the type it pushes the surface out by, a float along the normal.
    if (n.category == "displacement") {
        const Input* by = n.input("displacement");
        return std::string("ND_displacement_") + (by && by->type == "vector3" ? "vector3" : "float");
    }
    const Input* in = n.input("in");
    // Of the type it turns from and the type it turns to.
    if (n.category == "convert") return "ND_convert_" + (in ? in->type : std::string("float")) + "_" + n.type;
    // Of the type it takes apart.
    if (n.category == "extract") return "ND_extract_" + (in ? in->type : std::string("color4"));
    // Arithmetic of a vector or a colour with a float: FA.
    if (n.category == "multiply" || n.category == "add" || n.category == "subtract" || n.category == "divide" ||
        n.category == "min" || n.category == "max") {
        const Input* in2 = n.input("in2");
        if (in2 && in2->type == "float" && n.type != "float") return "ND_" + n.category + "_" + n.type + "FA";
    }
    return "ND_" + n.category + "_" + n.type;
}

std::vector<Node> portable(const std::vector<Node>& nodes) {
    std::vector<Node> out;
    out.reserve(nodes.size());
    for (const Node& n : nodes) {
        if (n.category != "normalmap") {
            out.push_back(n);
            continue;
        }
        const std::string& name = n.name;
        auto add = [&](const char* category, const std::string& as, const char* type, std::vector<Input> inputs) {
            out.push_back({category, as, type, std::move(inputs)});
            return as;
        };
        auto value = [](const char* input, const char* type, std::string v) { return Input{input, type, std::move(v), {}, {}, {}}; };
        auto from = [](const char* input, const char* type, const std::string& node) { return Input{input, type, {}, node, {}, {}}; };
        // What it bends by: its input, the flat 0.5, 0.5, 1 without one.
        Input in = n.input("in") ? *n.input("in") : value("in", "vector3", "0.5, 0.5, 1");
        in.name = "in1";
        in.type = "vector3";
        const std::string doubled = add("multiply", name + "_doubled", "vector3", {in, value("in2", "float", "2")});
        const std::string decoded = add("subtract", name + "_decoded", "vector3", {from("in1", "vector3", doubled), value("in2", "float", "1")});
        // x and y as strong as its scale says.
        Input scale = n.input("scale") ? *n.input("scale") : value("scale", "float", "1");
        std::string by;
        if (scale.nodename.empty()) {
            const std::string s = scale.value.empty() ? "1" : scale.value;
            by = add("multiply", name + "_scaled", "vector3", {from("in1", "vector3", decoded), value("in2", "vector3", s + ", " + s + ", 1")});
        } else {
            scale.name = "in1";
            Input same = scale;
            same.name = "in2";
            const std::string k = add("combine3", name + "_scale", "vector3", {scale, same, value("in3", "float", "1")});
            by = add("multiply", name + "_scaled", "vector3", {from("in1", "vector3", decoded), from("in2", "vector3", k)});
        }
        std::string axis[3];
        for (int a = 0; a < 3; ++a) {
            axis[a] = add("extract", name + "_" + "xyz"[a], "float", {from("in", "vector3", by), value("index", "integer", std::to_string(a))});
        }
        const std::string t = add("tangent", name + "_tangent", "vector3", {value("space", "string", "world")});
        const std::string normal = add("normal", name + "_normal", "vector3", {value("space", "string", "world")});
        const std::string b = add("crossproduct", name + "_bitangent", "vector3", {from("in1", "vector3", normal), from("in2", "vector3", t)});
        const std::string tx = add("multiply", name + "_tx", "vector3", {from("in1", "vector3", t), from("in2", "float", axis[0])});
        const std::string bty = add("multiply", name + "_by", "vector3", {from("in1", "vector3", b), from("in2", "float", axis[1])});
        const std::string nz = add("multiply", name + "_nz", "vector3", {from("in1", "vector3", normal), from("in2", "float", axis[2])});
        const std::string across = add("add", name + "_across", "vector3", {from("in1", "vector3", tx), from("in2", "vector3", bty)});
        const std::string sum = add("add", name + "_sum", "vector3", {from("in1", "vector3", across), from("in2", "vector3", nz)});
        add("normalize", name, "vector3", {from("in", "vector3", sum)});
    }
    return out;
}

std::string usdType(const std::string& type) {
    if (type == "color3") return "color3f";
    if (type == "color4") return "color4f";
    if (type == "vector2") return "float2";
    if (type == "vector3") return "float3";
    if (type == "vector4") return "float4";
    if (type == "integer") return "int";
    if (type == "boolean") return "bool";
    if (type == "filename") return "asset";
    if (type == "surfaceshader" || type == "displacementshader" || type == "volumeshader" || type == "material") {
        return "token";
    }
    return type;
}

const Input* Node::input(const std::string& name) const {
    for (const Input& i : inputs) {
        if (i.name == name) return &i;
    }
    return nullptr;
}

std::string document(const std::vector<Material>& materials) {
    std::ostringstream out;
    out << "<?xml version=\"1.0\"?>\n";
    out << "<materialx version=\"1.38\" colorspace=\"lin_rec709\">\n";
    for (const Material& m : materials) {
        out << "  <!-- " << escaped(m.name) << " -->\n";
        for (const Node& n : m.nodes) {
            out << "  <" << n.category << " name=\"" << escaped(n.name) << "\" type=\"" << n.type << "\"";
            if (n.inputs.empty()) {
                out << " />\n";
                continue;
            }
            out << ">\n";
            for (const Input& i : n.inputs) {
                out << "    <input name=\"" << i.name << "\" type=\"" << i.type << "\"";
                if (!i.nodename.empty()) {
                    out << " nodename=\"" << escaped(i.nodename) << "\"";
                    if (!i.output.empty()) out << " output=\"" << escaped(i.output) << "\"";
                } else {
                    out << " value=\"" << escaped(i.value) << "\"";
                }
                if (!i.colorspace.empty()) out << " colorspace=\"" << i.colorspace << "\"";
                out << " />\n";
            }
            out << "  </" << n.category << ">\n";
        }
    }
    out << "</materialx>\n";
    return out.str();
}

bool parse(const std::string& text, std::vector<Node>& nodes, std::string& error) {
    std::vector<Element> all;
    if (!elements(text, all, error)) return false;
    if (all.empty() || all[0].tag != "materialx") return error = "not a MaterialX document", false;
    // A node: any element under <materialx> (or a nodegraph in it) but an
    // input, an output or a nodegraph -- its inputs the elements right under
    // it. An input from a node graph's output: from the node that output is
    // of; one a graph's own input stands for (interfacename): that input's.
    using Key = std::pair<std::string, std::string>;  // graph, name
    std::map<Key, std::string> outputs;               // a graph's output -> its node
    std::map<Key, Input> interface;                   // a graph's own inputs
    struct Pending {
        size_t node, input;
        Key from;
    };
    std::vector<Pending> fromGraphs, fromInterface;
    size_t current = SIZE_MAX;
    int nodeDepth = -1;
    std::string graph;
    int graphDepth = -1;
    std::string nodeGraph;  // the graph the current node is in
    for (size_t k = 1; k < all.size(); ++k) {
        const Element& e = all[k];
        if (graphDepth >= 0 && e.depth <= graphDepth) graph.clear(), graphDepth = -1;
        if (current != SIZE_MAX && e.depth == nodeDepth + 1 && e.tag == "input") {
            Input in;
            in.name = attributeOf(e, "name");
            in.type = attributeOf(e, "type");
            in.value = attributeOf(e, "value");
            in.nodename = attributeOf(e, "nodename");
            in.colorspace = attributeOf(e, "colorspace");
            if (!in.nodename.empty()) in.output = attributeOf(e, "output");
            const std::string fromGraph = attributeOf(e, "nodegraph");
            const std::string standsFor = attributeOf(e, "interfacename");
            const size_t at = nodes[current].inputs.size();
            if (!fromGraph.empty()) fromGraphs.push_back({current, at, {fromGraph, attributeOf(e, "output")}});
            else if (!standsFor.empty() && !nodeGraph.empty()) fromInterface.push_back({current, at, {nodeGraph, standsFor}});
            nodes[current].inputs.push_back(std::move(in));
            continue;
        }
        if (e.tag == "nodegraph") {
            graph = attributeOf(e, "name");
            graphDepth = e.depth;
            current = SIZE_MAX;
            continue;
        }
        if (e.tag == "output") {
            if (!graph.empty()) outputs[{graph, attributeOf(e, "name")}] = attributeOf(e, "nodename");
            current = SIZE_MAX;
            continue;
        }
        if (e.tag == "input" && graphDepth >= 0 && e.depth == graphDepth + 1) {
            Input in;
            in.name = attributeOf(e, "name");
            in.type = attributeOf(e, "type");
            in.value = attributeOf(e, "value");
            in.nodename = attributeOf(e, "nodename");
            in.colorspace = attributeOf(e, "colorspace");
            interface[{graph, in.name}] = std::move(in);
            current = SIZE_MAX;
            continue;
        }
        if (e.tag == "input" || e.tag == "nodedef" || e.tag == "look" || e.tag == "materialassign" || e.tag == "collection" ||
            e.tag == "parameter" || e.tag == "token" || e.tag == "geominfo" || e.tag == "geomprop" || e.tag == "typedef" ||
            e.tag == "implementation" || e.tag == "propertyset" || e.tag == "variant" || e.tag == "variantset") {
            current = SIZE_MAX;
            continue;
        }
        nodes.push_back({e.tag, attributeOf(e, "name"), attributeOf(e, "type"), {}});
        current = nodes.size() - 1;
        nodeDepth = e.depth;
        nodeGraph = graph;
    }
    for (const Pending& p : fromGraphs) {
        auto it = outputs.find(p.from);
        if (it == outputs.end() && p.from.second.empty()) {
            // No output named: the graph's only one.
            for (auto o = outputs.begin(); o != outputs.end(); ++o) {
                if (o->first.first == p.from.first) it = o;
            }
        }
        nodes[p.node].inputs[p.input].nodename = it == outputs.end() ? std::string() : it->second;
    }
    for (const Pending& p : fromInterface) {
        const auto it = interface.find(p.from);
        if (it == interface.end()) continue;
        Input& in = nodes[p.node].inputs[p.input];
        if (in.value.empty()) in.value = it->second.value;
        if (in.nodename.empty()) in.nodename = it->second.nodename;
        if (in.colorspace.empty()) in.colorspace = it->second.colorspace;
    }
    return true;
}

Surface surfaceOf(const std::vector<Node>& nodes, const std::string& name) {
    Surface out;
    std::map<std::string, const Node*> byName;
    for (const Node& n : nodes) byName[n.name] = &n;
    auto node = [&](const std::string& n) -> const Node* {
        const auto it = byName.find(n);
        return it == byName.end() ? nullptr : it->second;
    };
    auto number = [](const Input* in, float otherwise) {
        if (!in || !in->nodename.empty() || in->value.empty()) return otherwise;
        char* end = nullptr;
        const float x = std::strtof(in->value.c_str(), &end);
        return end != in->value.c_str() && std::isfinite(x) ? x : otherwise;
    };
    const Node* material = nullptr;
    for (const Node& n : nodes) {
        if (n.category == "surfacematerial" && (name.empty() || n.name == name)) {
            material = &n;
            break;
        }
    }
    const Node* shader = nullptr;
    if (material) {
        if (const Input* s = material->input("surfaceshader")) shader = node(s->nodename);
    } else if (name.empty()) {
        for (const Node& n : nodes) {
            if (n.category == "standard_surface" || n.category == "UsdPreviewSurface" || n.category == "open_pbr_surface" ||
                n.category == "gltf_pbr") {
                shader = &n;
                break;
            }
        }
    }
    if (!shader) return out;
    out.found = true;
    out.shader = shader->category;
    const bool preview = shader->category == "UsdPreviewSurface";
    const bool openPbr = shader->category == "open_pbr_surface";
    const bool gltf = shader->category == "gltf_pbr";
    // What is behind an input: the first picture on the way back -- every
    // input of every node on it followed, in order -- and what the way does
    // to it.
    struct Trail {
        std::string file;
        bool alpha = false, tinted = false, flipped = false;
        float size = -1.0f;
        Vec3 scale{-1.0f, -1.0f, -1.0f};
    };
    auto isPicture = [](const Node* n) {
        return n && (n->category == "image" || n->category == "tiledimage" || n->category == "triplanarprojection" ||
                     n->category == "UsdUVTexture");
    };
    auto trail = [&](const Input* from) {
        Trail t;
        std::vector<const Node*> seen;
        std::vector<std::pair<const Input*, int>> todo;
        if (from) todo.push_back({from, 0});
        while (!todo.empty()) {
            const auto [in, depth] = todo.back();
            todo.pop_back();
            if (in->nodename.empty() || depth > 32) continue;
            const Node* n = node(in->nodename);
            if (!n || std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
            seen.push_back(n);
            if (in->output == "a" && t.file.empty()) t.alpha = true;
            if (n->category == "image" || n->category == "tiledimage" || n->category == "UsdUVTexture") {
                if (const Input* f = n->input("file"); f && t.file.empty()) t.file = f->value;
                continue;
            }
            if (n->category == "triplanarprojection") {
                if (const Input* f = n->input("filex"); f && t.file.empty()) t.file = f->value;
                // So many metres a picture: its position times one over them.
                const Input* p = n->input("position");
                const Node* scaled = p ? node(p->nodename) : nullptr;
                if (scaled && scaled->category == "multiply") {
                    const float k = number(scaled->input("in2"), -1.0f);
                    if (k > 0.0f) t.size = 1.0f / k;
                }
                continue;
            }
            if (n->category == "geompropvalue" || n->category == "UsdPrimvarReader_float3") {
                t.tinted = true;
                continue;
            }
            if (n->category == "extract" && number(n->input("index"), -1.0f) == 3.0f) t.alpha = true;
            if (n->category == "multiply") {
                // The picture times a colour straight away.
                const Input* first = n->input("in1");
                const Input* by = n->input("in2");
                if (first && by && by->nodename.empty() && by->type == "color3" && isPicture(node(first->nodename))) {
                    float v[3];
                    const char* at = by->value.c_str();
                    int read = 0;
                    for (; read < 3; ++read) {
                        char* end = nullptr;
                        v[read] = std::strtof(at, &end);
                        if (end == at) break;
                        at = end;
                        while (*at == ',' || *at == ' ') ++at;
                    }
                    if (read == 3) t.scale = Vec3(v[0], v[1], v[2]);
                }
                // The green turned over: times (1, -1, 1).
                const Input* k = n->input("in2");
                if (k && k->nodename.empty() && k->type == "vector3") {
                    const size_t comma = k->value.find(',');
                    if (comma != std::string::npos && std::strtof(k->value.c_str() + comma + 1, nullptr) < 0.0f) t.flipped = true;
                }
            }
            // Its inputs, the first first.
            for (auto i = n->inputs.rbegin(); i != n->inputs.rend(); ++i) {
                if (!i->nodename.empty()) todo.push_back({&*i, depth + 1});
            }
        }
        return t;
    };
    const Trail color = trail(shader->input(preview ? "diffuseColor" : "base_color"));
    out.color = color.file;
    out.tinted = color.tinted;
    out.size = color.size;
    out.scale = color.scale;
    const Trail normal = trail(shader->input(openPbr ? "geometry_normal" : "normal"));
    out.normal = normal.file;
    out.normalDirectX = normal.flipped;
    const char* opaque = openPbr ? "geometry_opacity" : gltf ? "alpha" : "opacity";
    const Trail opacity = trail(shader->input(opaque));
    out.opacity = opacity.file;
    out.opacityFromAlpha = opacity.alpha;
    const char* rough = preview || gltf ? "roughness" : "specular_roughness";
    out.roughnessFile = trail(shader->input(rough)).file;
    out.roughness = number(shader->input(rough), -1.0f);
    const char* metal = preview || gltf ? "metallic" : openPbr ? "base_metalness" : "metalness";
    out.metalness = number(shader->input(metal), -1.0f);
    // The values, each shader's defaults where it gives none (MaterialX's
    // node definitions; USD's for UsdPreviewSurface).
    auto three = [](const Input* in, Vec3& v) {
        if (!in || !in->nodename.empty() || in->value.empty()) return false;
        float x[3];
        const char* at = in->value.c_str();
        int read = 0;
        for (; read < 3; ++read) {
            char* end = nullptr;
            x[read] = std::strtof(at, &end);
            if (end == at) break;
            at = end;
            while (*at == ',' || *at == ' ') ++at;
        }
        if (read == 1) x[1] = x[2] = x[0];
        else if (read != 3) return false;
        v = Vec3(x[0], x[1], x[2]);
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    Surface::Values& v = out.values;
    const Input* base = shader->input(preview ? "diffuseColor" : "base_color");
    Vec3 baseColor = preview ? Vec3(0.18f) : gltf ? Vec3(1.0f) : Vec3(0.8f);
    if (!base || three(base, baseColor)) {
        const float weight = preview || gltf ? 1.0f : number(shader->input(openPbr ? "base_weight" : "base"), 1.0f);
        v.color = baseColor * weight;
    }
    v.roughness = out.roughness >= 0.0f ? out.roughness : preview ? 0.5f : gltf ? 1.0f : openPbr ? 0.3f : 0.2f;
    v.metalness = out.metalness >= 0.0f ? out.metalness : gltf ? 1.0f : 0.0f;
    Vec3 opacityColor(1.0f);
    if (three(shader->input(opaque), opacityColor)) v.opacity = (opacityColor.x + opacityColor.y + opacityColor.z) / 3.0f;
    v.transmission = number(shader->input(openPbr ? "transmission_weight" : "transmission"), 0.0f);
    v.ior = number(shader->input(preview ? "ior" : openPbr ? "specular_ior" : gltf ? "ior" : "specular_IOR"), 1.5f);
    // How high: the displacement's picture, as deep as its scale says --
    // a UsdPreviewSurface's, as its picture's scale says.
    auto pictureDepth = [&](const Input* in) {
        const Node* picture = in ? node(in->nodename) : nullptr;
        if (!picture || picture->category != "UsdUVTexture") return -1.0f;
        const float k = number(picture->input("scale"), -1.0f);
        return k > 0.0f ? k : -1.0f;
    };
    auto heightOf = [&](const Node* displacement) {
        if (!displacement) return;
        const Input* by = displacement->input("displacement");
        out.height = trail(by).file;
        if (out.height.empty()) return;
        const float k = displacement->category == "UsdPreviewSurface" ? pictureDepth(by)
                                                                      : number(displacement->input("scale"), -1.0f);
        if (k > 0.0f) out.depth = k;
    };
    if (material) {
        if (const Input* d = material->input("displacementshader")) heightOf(node(d->nodename));
    }
    if (out.height.empty() && preview) heightOf(shader);
    return out;
}

}  // namespace pg::io::mtlx
