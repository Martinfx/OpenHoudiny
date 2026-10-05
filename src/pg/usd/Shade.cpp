#include "pg/usd/Shade.h"

#include "pg/usd/Layer.h"

#include <algorithm>
#include <map>
#include <set>

namespace pg::usd {
namespace {

using io::mtlx::Input;
using io::mtlx::Node;

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

/// One prim's binding of one purpose: the relationship `name`.
struct Binding {
    std::string material;
    bool stronger = false;
    bool found = false;
};

Binding bindingOf(const Stage& stage, const Stage::Prim& prim, const char* name) {
    const Property* rel = stage.property(prim, name);
    if (!rel || !rel->relationship) return {};
    const std::vector<std::string> targets = stage.targets(prim, name);
    // A binding to nothing binds nothing: what is above goes on.
    if (targets.empty() || targets.front().empty()) return {};
    Binding b;
    b.found = true;
    b.material = stripVariants(targets.front());
    const Value* strength = rel->meta("bindMaterialAs");
    b.stronger = strength && strength->text() == "strongerThanDescendants";
    return b;
}

/// What a connection reaches: a shader's output, or -- through node
/// graphs and interfaces -- a value, with the prim and input that hold it.
struct Reached {
    const Stage::Prim* shader = nullptr;
    std::string output;
    const Stage::Prim* holder = nullptr;  ///< for a value: the prim whose input has it
    std::string input;                    ///< ... and the input
};

/// The MaterialX type of a USD attribute's: color3f is color3, float3 and
/// its roles vector3, asset filename...
std::string mtlxType(const std::string& usdType) {
    std::string t = usdType;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
    if (startsWith(t, "color3")) return "color3";
    if (startsWith(t, "color4")) return "color4";
    if (t == "float" || t == "double" || t == "half") return "float";
    if (t == "float2" || t == "double2" || t == "half2" || startsWith(t, "texCoord2")) return "vector2";
    if (t == "float3" || t == "double3" || t == "half3" || startsWith(t, "vector3") || startsWith(t, "normal3") ||
        startsWith(t, "point3") || startsWith(t, "texCoord3")) {
        return "vector3";
    }
    if (t == "float4" || t == "double4" || t == "half4") return "vector4";
    if (t == "int" || t == "int64" || t == "uint" || t == "uchar") return "integer";
    if (t == "bool") return "boolean";
    if (t == "asset") return "filename";
    if (t == "matrix3d") return "matrix33";
    if (t == "matrix4d") return "matrix44";
    return "string";
}

class Network {
public:
    Network(const Stage& stage, double time) : stage_(stage), time_(time) {}

    /// Where the connection `target` -- "/M/Shader.outputs:rgb" -- leads.
    Reached follow(const std::string& target, int depth = 0) const {
        Reached out;
        if (depth > 64) return out;
        const size_t dot = target.rfind('.');
        if (dot == std::string::npos) return out;
        const Stage::Prim* prim = stage_.find(stripVariants(target.substr(0, dot)));
        if (!prim) return out;
        const std::string port = target.substr(dot + 1);
        if (prim->type == "Shader") {
            out.shader = prim;
            out.output = startsWith(port, "outputs:") ? port.substr(8) : port;
            return out;
        }
        // A node graph's or a material's: its output is connected on;
        // its interface input has the value, or is connected on.
        const std::vector<std::string> on = stage_.targets(*prim, port);
        if (!on.empty()) return follow(on.front(), depth + 1);
        if (startsWith(port, "inputs:")) {
            out.holder = prim;
            out.input = port;
        }
        return out;
    }

    /// The nodes behind the material's outputs: a surfacematerial first.
    std::vector<Node> build(const Stage::Prim& material) {
        Node top;
        top.category = "surfacematerial";
        top.name = material.path;
        top.type = "material";
        auto output = [&](const char* mtlx, const char* universal) -> Reached {
            for (const char* name : {mtlx, universal}) {
                const std::vector<std::string> targets = stage_.targets(material, name);
                if (targets.empty()) continue;
                const Reached r = follow(targets.front());
                if (r.shader) return r;
            }
            return {};
        };
        const Reached surface = output("outputs:mtlx:surface", "outputs:surface");
        if (!surface.shader) return {};
        Input s;
        s.name = "surfaceshader";
        s.type = "surfaceshader";
        s.nodename = surface.shader->path;
        top.inputs.push_back(s);
        const Reached displacement = output("outputs:mtlx:displacement", "outputs:displacement");
        if (displacement.shader) {
            Input d;
            d.name = "displacementshader";
            d.type = "displacementshader";
            d.nodename = displacement.shader->path;
            top.inputs.push_back(d);
        }
        nodes_.push_back(top);
        add(*surface.shader);
        if (displacement.shader) add(*displacement.shader);
        return std::move(nodes_);
    }

private:
    /// A file as USD resolves the asset: from the layer that names it.
    std::string resolved(const Stage::Prim& prim, const std::string& name, const std::string& asset) const {
        for (const Stage::Opinion& o : prim.opinions) {
            const Property* p = o.spec->property(name);
            if (p && (p->hasDefault || p->hasSamples)) return resolveAsset(asset, o.layer->identifier);
        }
        return resolveAsset(asset, stage_.rootLayer().identifier);
    }

    /// The value of `prim`'s input `name` as MaterialX writes it: numbers
    /// "a, b, c", a file resolved, text as it is.
    std::string valueOf(const Stage::Prim& prim, const std::string& name, const std::string& type) const {
        const Value v = stage_.value(prim, name, time_);
        if (v.isNumbers()) {
            std::string text;
            for (size_t i = 0; i < v.numbers.size() && i < 16; ++i) {
                if (i) text += ", ";
                text += io::mtlx::number(static_cast<float>(v.numbers[i]));
            }
            return text;
        }
        if (v.isStrings()) return type == "filename" ? resolved(prim, name, v.text()) : v.text();
        return {};
    }

    void add(const Stage::Prim& shader) {
        if (!seen_.insert(shader.path).second || nodes_.size() > 4096) return;
        Node node;
        node.name = shader.path;
        shaderKind(stage_.value(shader, "info:id", time_).text(), node.category, node.type);
        std::vector<const Stage::Prim*> behind;
        for (const std::string& name : stage_.propertyNames(shader)) {
            if (!startsWith(name, "inputs:")) continue;
            const Property* spec = stage_.property(shader, name);
            if (!spec || spec->relationship) continue;
            Input in;
            in.name = name.substr(7);
            in.type = mtlxType(spec->typeName);
            if (const Value* cs = spec->meta("colorSpace")) in.colorspace = cs->text();
            const std::vector<std::string> targets = stage_.targets(shader, name);
            if (!targets.empty()) {
                const Reached r = follow(targets.front());
                if (r.shader) {
                    in.nodename = r.shader->path;
                    in.output = r.output == "out" ? std::string() : r.output;
                    behind.push_back(r.shader);
                } else if (r.holder) {
                    in.value = valueOf(*r.holder, r.input, in.type);
                }
            } else {
                in.value = valueOf(shader, name, in.type);
            }
            if (in.nodename.empty() && in.value.empty()) continue;
            node.inputs.push_back(std::move(in));
        }
        nodes_.push_back(std::move(node));
        for (const Stage::Prim* b : behind) add(*b);
    }

    const Stage& stage_;
    double time_;
    std::vector<Node> nodes_;
    std::set<std::string> seen_;
};

}  // namespace

std::string boundMaterial(const Stage& stage, const Stage::Prim& prim) {
    // The purpose full's bindings first -- over all of the prims above --,
    // else those of all purposes. Of each, from the top down: the nearest
    // wins, unless one above it is stronger than what is below it.
    std::vector<const Stage::Prim*> chain;
    for (const Stage::Prim* p = &prim; p; p = p->parent) chain.push_back(p);
    for (const char* name : {"material:binding:full", "material:binding"}) {
        std::string chosen;
        bool strong = false;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const Binding b = bindingOf(stage, **it, name);
            if (!b.found || (!chosen.empty() && strong)) continue;
            chosen = b.material;
            strong = b.stronger;
        }
        if (chosen.empty()) continue;
        const Stage::Prim* m = stage.find(chosen);
        return m && m->type == "Material" ? m->path : std::string();
    }
    return {};
}

std::vector<BoundFaces> boundSubsets(const Stage& stage, const Stage::Prim& mesh, double time) {
    std::vector<BoundFaces> out;
    for (const Stage::Prim* child : mesh.children) {
        if (child->type != "GeomSubset" || !child->defined) continue;
        const std::string element = stage.value(*child, "elementType", time).text();
        if (!element.empty() && element != "face") continue;
        if (stage.value(*child, "familyName", time).text() != "materialBind") continue;
        BoundFaces b;
        b.subset = child->path;
        b.material = boundMaterial(stage, *child);
        if (b.material.empty()) continue;
        for (const double i : stage.value(*child, "indices", time).numbers) {
            if (i >= 0.0 && i < 4294967295.0) b.faces.push_back(static_cast<uint32_t>(i));
        }
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<io::mtlx::Node> materialNodes(const Stage& stage, const Stage::Prim& material, double time) {
    return Network(stage, time).build(material);
}

io::mtlx::Surface materialSurface(const Stage& stage, const Stage::Prim& material, double time) {
    const std::vector<io::mtlx::Node> nodes = materialNodes(stage, material, time);
    if (nodes.empty()) return {};
    return io::mtlx::surfaceOf(nodes);
}

void shaderKind(const std::string& id, std::string& category, std::string& type) {
    category.clear();
    type.clear();
    if (!startsWith(id, "ND_")) {
        // USD's own: UsdPreviewSurface, UsdUVTexture, UsdPrimvarReader_float3...
        category = id;
        if (id == "UsdPreviewSurface") type = "surfaceshader";
        else if (startsWith(id, "UsdPrimvarReader_")) type = id.substr(17);
        else if (id == "UsdUVTexture") type = "color4";
        return;
    }
    static const char* const kTypes[] = {"float",   "integer", "boolean",  "string",   "filename",      "color3",
                                         "color4",  "vector2", "vector3",  "vector4",  "matrix33",      "matrix44",
                                         "surfaceshader", "displacementshader", "volumeshader", "lightshader",
                                         "material", "BSDF",  "EDF",      "VDF"};
    auto typeOf = [&](std::string token) -> std::string {
        // "color3FA", "vector3I": a type and how its other input is.
        for (const char* suffix : {"FA", "I", "B"}) {
            const size_t n = std::char_traits<char>::length(suffix);
            if (token.size() > n && token.compare(token.size() - n, n, suffix) == 0) {
                const std::string bare = token.substr(0, token.size() - n);
                for (const char* t : kTypes) {
                    if (bare == t) return bare;
                }
            }
        }
        for (const char* t : kTypes) {
            if (token == t) return token;
        }
        return {};
    };
    // "ND_" then the category -- which may have _ in it -- then the types.
    std::vector<std::string> tokens;
    std::string rest = id.substr(3);
    for (size_t at = 0; at <= rest.size();) {
        const size_t next = rest.find('_', at);
        tokens.push_back(rest.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) break;
        at = next + 1;
    }
    size_t first = tokens.size();
    for (size_t k = 1; k < tokens.size(); ++k) {
        if (!typeOf(tokens[k]).empty()) {
            first = k;
            break;
        }
    }
    for (size_t k = 0; k < first; ++k) category += (k ? "_" : "") + tokens[k];
    for (size_t k = first; k < tokens.size(); ++k) {
        if (const std::string t = typeOf(tokens[k]); !t.empty()) type = t;
    }
    if (category == "extract") type = "float";
    if (category == "displacement") type = "displacementshader";
    if (type.empty() && category == "normalmap") type = "vector3";
}

}  // namespace pg::usd
