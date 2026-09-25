#include "pg/shader/Generator.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace pg::shader {
namespace {

std::string varName(int node, const std::string& port) {
    return "n" + std::to_string(node) + "_" + port;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

/// Globals in the order the targets declare them.
constexpr const char* kGlobalOrder[] = {"position", "normal", "uv", "view", "light", "time"};

class Compiler {
public:
    Compiler(const ShaderGraph& g, const NodeLibrary& lib, const Target& t) : g_(g), lib_(lib), t_(t) {}

    GeneratedShader run() {
        GeneratedShader out;
        out.target = t_.name();

        NodeState* root = findOutputNode();
        if (!root) return fail(out);

        // One root per stage: the first fragment input is the colour, the first
        // vertex input the offset.
        const NodeDef& od = *root->def;
        int fragmentRoot = -1, vertexRoot = -1;
        for (size_t i = 0; i < od.inputs.size(); ++i) {
            int& slot = od.inputs[i].stage == Stage::Fragment ? fragmentRoot : vertexRoot;
            if (slot >= 0) error(root->id(), "an output node has at most one input per stage");
            else slot = static_cast<int>(i);
        }
        if (fragmentRoot < 0) error(root->id(), "the output node needs a fragment-stage input");
        if (!errors_.empty() || !resolve(*root)) return fail(out);

        Assembly a;
        StageCtx vertex(Stage::Vertex, &a.vertex), fragment(Stage::Fragment, &a.fragment);
        if (vertexRoot >= 0 && !isZeroDefault(*root, static_cast<size_t>(vertexRoot))) {
            a.vertex.result = rootExpression(*root, static_cast<size_t>(vertexRoot), Type::Vec3, vertex);
        }
        a.fragment.result = rootExpression(*root, static_cast<size_t>(fragmentRoot), Type::Vec4, fragment);
        if (!errors_.empty()) return fail(out);

        for (const char* g : kGlobalOrder) {
            if (vertex.globals.count(g)) a.vertex.globals.push_back(g);
            if (fragment.globals.count(g)) a.fragment.globals.push_back(g);
        }
        // What the fragment stage reads per surface point has to be passed on
        // from the vertex stage; $view is computed from the position.
        if (fragment.globals.count("position") || fragment.globals.count("view")) {
            a.varyings.push_back("position");
        }
        if (fragment.globals.count("normal")) a.varyings.push_back("normal");
        if (fragment.globals.count("uv")) a.varyings.push_back("uv");

        int binding = 0;
        for (auto& [name, u] : uniforms_) {
            if (u.type == Type::Sampler2D) u.binding = binding++;
            a.uniforms.push_back(u);
        }
        a.vertex.uniforms.assign(vertex.uniforms.begin(), vertex.uniforms.end());
        a.fragment.uniforms.assign(fragment.uniforms.begin(), fragment.uniforms.end());

        out.uniforms = a.uniforms;
        out.files = t_.assemble(a);
        return out;
    }

private:
    struct NodeState {
        const GraphNode* node = nullptr;
        const NodeDef* def = nullptr;
        Type anyType = Type::Float;
        enum class Mark : uint8_t { None, Visiting, Done } mark = Mark::None;
        bool failed = false;

        int id() const { return node->id; }
        Type inputType(size_t i) const {
            const Type t = def->inputs[i].type;
            return t == Type::Any ? anyType : t;
        }
        Type outputType(size_t i) const {
            const Type t = def->outputs[i].type;
            return t == Type::Any ? anyType : t;
        }
    };

    /// What one stage has collected so far.
    struct StageCtx {
        StageCtx(Stage s, StageBody* b) : stage(s), body(b) {}

        Stage stage;
        StageBody* body;
        std::set<std::pair<int, int>> needed;  ///< (node, output) the stage uses
        std::set<int> emitted;
        std::set<std::string> globals;
        std::set<std::string> uniforms;
        std::set<std::string> functions;
    };

    GeneratedShader& fail(GeneratedShader& out) {
        out.errors = errors_;
        out.files.clear();
        return out;
    }

    void error(int node, const std::string& msg) {
        for (const auto& e : errors_) {
            if (e.node == node && e.message == msg) return;  // once is enough
        }
        errors_.push_back({node, msg});
    }

    NodeState* state(int id) {
        auto it = states_.find(id);
        if (it != states_.end()) return &it->second;
        const GraphNode* n = g_.node(id);
        if (!n) return nullptr;
        NodeState& s = states_[id];
        s.node = n;
        s.def = lib_.find(n->type);
        if (!s.def) {
            s.failed = true;
            error(id, "unknown node type '" + n->type + "'");
        } else if (n->version > s.def->version) {
            s.failed = true;
            error(id, "'" + s.def->label + "' was saved by a newer version of its definition (" +
                          std::to_string(n->version) + " > " + std::to_string(s.def->version) + ")");
        }
        return &s;
    }

    NodeState* findOutputNode() {
        NodeState* found = nullptr;
        for (const auto& n : g_.nodes()) {
            const NodeDef* def = lib_.find(n.type);
            if (!def || !def->isOutput) continue;
            if (found) {
                error(n.id, "a graph has one output node; this is a second one");
                continue;
            }
            found = state(n.id);
        }
        if (!found) error(-1, "the graph has no output node -- add a Surface Output");
        return errors_.empty() ? found : nullptr;
    }

    /// Resolves `any` types upstream of `s`, depth first, and rejects loops
    /// and links the library no longer allows.
    bool resolve(NodeState& s) {
        if (s.failed) return false;
        if (s.mark == NodeState::Mark::Done) return true;
        if (s.mark == NodeState::Mark::Visiting) {
            error(s.id(), "the graph loops back into '" + s.def->label + "'");
            s.failed = true;
            return false;
        }
        s.mark = NodeState::Mark::Visiting;
        bool ok = true;
        int width = 0;
        for (size_t i = 0; i < s.def->inputs.size(); ++i) {
            const PortDef& p = s.def->inputs[i];
            int components = 0;
            if (const Link* l = g_.linkInto(s.id(), p.name)) {
                NodeState* src = state(l->fromNode);
                if (!src || !resolve(*src)) {
                    ok = false;
                    continue;
                }
                const int o = src->def->outputIndex(l->fromPort);
                if (o < 0) {
                    error(s.id(), "input '" + p.name + "' is linked to '" + l->fromPort +
                                      "', which '" + src->def->label + "' does not have");
                    ok = false;
                    continue;
                }
                const Type have = src->outputType(static_cast<size_t>(o));
                if (!convertible(have, p.type)) {
                    error(s.id(), std::string("input '") + p.name + "' cannot take a " + typeName(have));
                    ok = false;
                    continue;
                }
                components = componentCount(have);
            } else if (auto it = s.node->inputs.find(p.name); it != s.node->inputs.end()) {
                components = componentCount(it->second.type);
            } else if (!p.defaultGlobal.empty()) {
                components = componentCount(findGlobal(p.defaultGlobal)->type);
            } else {
                components = componentCount(p.defaultValue.type);
            }
            if (p.type == Type::Any) width = std::max(width, components);
        }
        s.anyType = vectorType(std::max(1, width));
        s.mark = NodeState::Mark::Done;
        if (!ok) s.failed = true;
        return ok;
    }

    /// The template of output `o` for this target.
    const std::string& templateOf(const NodeState& s, size_t o) const {
        const OutputDef& out = s.def->outputs[o];
        auto it = out.perTarget.find(t_.name());
        return it != out.perTarget.end() ? it->second : out.code;
    }

    /// Marks output `o` of `s` as used by this stage, and everything it reads.
    /// Outputs nobody reads are never written, so the code stays readable.
    void need(NodeState& s, size_t o, StageCtx& ctx) {
        if (s.failed || !ctx.needed.insert({s.id(), static_cast<int>(o)}).second) return;
        std::vector<TemplatePiece> pieces;
        std::string why;
        if (!parseTemplate(templateOf(s, o), pieces, why)) {
            error(s.id(), why);
            return;
        }
        for (const auto& p : pieces) {
            if (p.kind != TemplatePiece::Kind::Placeholder) continue;
            if (const int earlier = s.def->outputIndex(p.text); earlier >= 0) {
                need(s, static_cast<size_t>(earlier), ctx);
            } else if (s.def->inputIndex(p.text) >= 0) {
                needInput(s, p.text, ctx);
            }
        }
    }

    void needInput(NodeState& s, const std::string& input, StageCtx& ctx) {
        const Link* l = g_.linkInto(s.id(), input);
        if (!l) return;
        NodeState* src = state(l->fromNode);
        if (!src || src->failed) return;
        const int o = src->def->outputIndex(l->fromPort);
        if (o >= 0) need(*src, static_cast<size_t>(o), ctx);
    }

    /// Writes the needed outputs of `s`, after whatever they read.
    void emit(NodeState& s, StageCtx& ctx) {
        if (s.failed || !ctx.emitted.insert(s.id()).second) return;
        for (const auto& f : s.def->functions) addFunction(f, ctx, s.id(), 0);
        addUniforms(s, ctx);
        for (size_t o = 0; o < s.def->outputs.size(); ++o) {
            if (!ctx.needed.count({s.id(), static_cast<int>(o)})) continue;
            const std::string code = expand(s, templateOf(s, o), ctx);
            ctx.body->statements.push_back(
                Statement{s.outputType(o), varName(s.id(), s.def->outputs[o].name), t_.translate(code)});
        }
    }

    std::string expand(NodeState& s, const std::string& tmpl, StageCtx& ctx) {
        std::vector<TemplatePiece> pieces;
        std::string why;
        if (!parseTemplate(tmpl, pieces, why)) {
            error(s.id(), why);
            return "0.0";
        }
        std::string out;
        for (const auto& p : pieces) {
            switch (p.kind) {
                case TemplatePiece::Kind::Code:
                    out += p.text;
                    break;
                case TemplatePiece::Kind::Global:
                    ctx.globals.insert(p.text);
                    out += "g_" + p.text;
                    break;
                case TemplatePiece::Kind::Placeholder:
                    if (const int i = s.def->inputIndex(p.text); i >= 0) {
                        out += inputExpression(s, static_cast<size_t>(i), ctx);
                    } else if (const ParamDef* param = s.def->param(p.text)) {
                        out += paramText(s, *param);
                    } else if (s.def->outputIndex(p.text) >= 0) {
                        out += varName(s.id(), p.text);
                    } else {
                        error(s.id(), "unknown placeholder {" + p.text + "}");
                    }
                    break;
            }
        }
        return out;
    }

    /// The expression an input reads: the linked output, converted to the
    /// input's type -- or, unconnected, its value or global.
    std::string inputExpression(NodeState& s, size_t i, StageCtx& ctx) {
        const PortDef& p = s.def->inputs[i];
        const Type want = s.inputType(i);
        if (const Link* l = g_.linkInto(s.id(), p.name)) {
            NodeState* src = state(l->fromNode);
            if (!src || src->failed) return t_.literal(p.defaultValue, want);  // already reported
            emit(*src, ctx);
            const int o = src->def->outputIndex(l->fromPort);
            return t_.convert(varName(src->id(), l->fromPort), src->outputType(static_cast<size_t>(o)), want);
        }
        if (auto it = s.node->inputs.find(p.name); it != s.node->inputs.end()) {
            return t_.literal(it->second, want);
        }
        if (!p.defaultGlobal.empty()) {
            ctx.globals.insert(p.defaultGlobal);
            return t_.convert("g_" + p.defaultGlobal, findGlobal(p.defaultGlobal)->type, want);
        }
        return t_.literal(p.defaultValue, want);
    }

    std::string rootExpression(NodeState& root, size_t input, Type type, StageCtx& ctx) {
        needInput(root, root.def->inputs[input].name, ctx);
        return t_.convert(inputExpression(root, input, ctx), root.inputType(input), type);
    }

    /// An unconnected vertex offset of zero writes no code at all.
    bool isZeroDefault(const NodeState& root, size_t input) const {
        const PortDef& p = root.def->inputs[input];
        if (g_.linkInto(root.id(), p.name) || !p.defaultGlobal.empty()) return false;
        auto it = root.node->inputs.find(p.name);
        const Value v = it != root.node->inputs.end() ? it->second : p.defaultValue;
        return std::all_of(v.v.begin(), v.v.end(), [](float x) { return x == 0.0f; });
    }

    std::string paramText(NodeState& s, const ParamDef& p) {
        if (p.isString) {
            auto it = s.node->params.find(p.name);
            const std::string text = it != s.node->params.end() ? it->second : p.text;
            if (!isIdentifier(text)) {
                error(s.id(), "'" + text + "' is not a valid name for '" + p.name + "'");
                return p.text;
            }
            return text;
        }
        return t_.literal(paramValue(s, p), p.type);
    }

    Value paramValue(NodeState& s, const ParamDef& p) {
        auto it = s.node->params.find(p.name);
        if (it == s.node->params.end()) return p.value;
        std::istringstream in(it->second);
        std::vector<float> numbers;
        for (std::string w; in >> w;) {
            float f = 0.0f;
            if (!parseFloat(w, f)) {
                error(s.id(), "param '" + p.name + "' is not a number: '" + it->second + "'");
                return p.value;
            }
            numbers.push_back(f);
        }
        if (numbers.empty() || numbers.size() > 4) return p.value;
        Value v;
        v.type = vectorType(static_cast<int>(numbers.size()));
        std::copy(numbers.begin(), numbers.end(), v.v.begin());
        return v.as(p.type);
    }

    void addUniforms(NodeState& s, StageCtx& ctx) {
        for (const UniformDef& u : s.def->uniforms) {
            std::vector<TemplatePiece> pieces;
            std::string why, name;
            if (!parseTemplate(u.nameTemplate, pieces, why)) {
                error(s.id(), why);
                continue;
            }
            for (const auto& p : pieces) {
                if (p.kind == TemplatePiece::Kind::Code) name += p.text;
                else if (const ParamDef* param = s.def->param(p.text)) name += paramText(s, *param);
            }
            if (!isIdentifier(name)) {
                error(s.id(), "uniform name '" + name + "' is not a valid identifier");
                continue;
            }
            if (isBuiltinUniform(name)) {
                error(s.id(), "'" + name + "' is reserved for the renderer");
                continue;
            }
            UniformInfo info;
            info.name = name;
            info.type = u.type;
            // `= {param}` makes the param's value the uniform's default.
            if (parseTemplate(u.defaultTemplate, pieces, why) && pieces.size() == 1 &&
                pieces[0].kind == TemplatePiece::Kind::Placeholder) {
                if (const ParamDef* param = s.def->param(pieces[0].text); param && !param->isString) {
                    info.defaultValue = paramValue(s, *param).as(u.type);
                }
            }
            auto [it, fresh] = uniforms_.emplace(name, info);
            if (!fresh && it->second.type != info.type) {
                error(s.id(), "uniform '" + name + "' is declared as both " +
                                  typeName(it->second.type) + " and " + typeName(info.type));
                continue;
            }
            ctx.uniforms.insert(name);
        }
    }

    /// Adds a helper function, after the functions it calls.
    void addFunction(const std::string& name, StageCtx& ctx, int node, int depth) {
        if (ctx.functions.count(name)) return;
        const FunctionDef* f = lib_.findFunction(name);
        if (!f) {
            error(node, "uses the unknown function '" + name + "'");
            return;
        }
        if (depth > 32) {
            error(node, "function '" + name + "' uses itself");
            return;
        }
        ctx.functions.insert(name);
        for (const auto& callee : f->uses) addFunction(callee, ctx, node, depth + 1);
        auto it = f->perTarget.find(t_.name());
        const std::string& code = it != f->perTarget.end() ? it->second : f->code;
        if (code.empty()) {
            error(node, "function '" + name + "' has no code for target '" + t_.name() + "'");
            return;
        }
        ctx.body->functions.push_back(t_.translate(code));
    }

    const ShaderGraph& g_;
    const NodeLibrary& lib_;
    const Target& t_;
    std::map<int, NodeState> states_;
    std::map<std::string, UniformInfo> uniforms_;  ///< sorted by name: stable layout
    std::vector<Diagnostic> errors_;
};

}  // namespace

const ShaderFile* GeneratedShader::fileFor(Stage stage) const {
    for (const auto& f : files) {
        for (const auto& [s, entry] : f.entryPoints) {
            if (s == stage) return &f;
        }
    }
    return nullptr;
}

std::string GeneratedShader::entryPoint(Stage stage) const {
    for (const auto& f : files) {
        for (const auto& [s, entry] : f.entryPoints) {
            if (s == stage) return entry;
        }
    }
    return {};
}

GeneratedShader generate(const ShaderGraph& graph, const NodeLibrary& library, const Target& target) {
    return Compiler(graph, library, target).run();
}

}  // namespace pg::shader
