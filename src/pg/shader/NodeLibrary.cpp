#include "pg/shader/NodeLibrary.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace pg::shader {
namespace {

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool isIdentifier(const std::string& s) {
    if (s.empty() || !isIdentStart(s[0])) return false;
    return std::all_of(s.begin(), s.end(), isIdentChar);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

/// Cursor over one line: words, an expected character, the rest.
class Line {
public:
    explicit Line(const std::string& s) : s_(s) {}

    std::string word() {
        skipSpace();
        const size_t b = pos_;
        while (pos_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[pos_])) &&
               s_[pos_] != '=') {
            ++pos_;
        }
        return s_.substr(b, pos_ - b);
    }
    bool accept(char c) {
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    std::string rest() {
        skipSpace();
        std::string r = trim(s_.substr(pos_));
        pos_ = s_.size();
        return r;
    }
    bool atEnd() {
        skipSpace();
        return pos_ >= s_.size();
    }

private:
    void skipSpace() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    std::string s_;
    size_t pos_ = 0;
};

/// "0.1 0.2, 0.3" -> Value of 1..4 components.
bool parseNumbers(const std::vector<std::string>& words, Value& out) {
    if (words.empty() || words.size() > 4) return false;
    Value v;
    v.type = vectorType(static_cast<int>(words.size()));
    for (size_t i = 0; i < words.size(); ++i) {
        if (!parseFloat(words[i], v.v[i])) return false;
    }
    out = v;
    return true;
}

std::vector<std::string> splitNumbers(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream in(text);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

/// A default or param value for `type`: one number is splatted, otherwise the
/// count must match.
bool valueFor(Type type, const std::vector<std::string>& words, Value& out, std::string& why) {
    Value v;
    if (!parseNumbers(words, v)) {
        why = "expected 1 to 4 numbers";
        return false;
    }
    if (type == Type::Any) {
        out = v;
        return true;
    }
    const int n = componentCount(type);
    if (componentCount(v.type) != 1 && componentCount(v.type) != n) {
        why = std::string("a ") + typeName(type) + " takes 1 or " + std::to_string(n) + " numbers";
        return false;
    }
    out = v.as(type);
    return true;
}

class LibraryParser {
public:
    LibraryParser(const std::string& origin, std::map<std::string, NodeDef>& nodes,
                  std::map<std::string, FunctionDef>& functions)
        : origin_(origin), nodes_(nodes), functions_(functions) {}

    bool parse(const std::string& text, std::string& error) {
        std::istringstream in(text);
        std::string raw;
        while (std::getline(in, raw)) {
            ++line_;
            const std::string stripped = trim(stripComment(raw));
            if (stripped.empty()) continue;
            Line l(stripped);
            const std::string key = l.word();

            if (key == "node") {
                if (!finishNode()) return report(error);
                const std::string name = l.word();
                if (!isIdentifier(name)) return fail("expected a node name", error);
                if (!l.atEnd()) return fail("unexpected text after the node name", error);
                current_ = NodeDef{};
                current_.name = current_.label = name;
                current_.origin = where();
                inNode_ = true;
                continue;
            }
            if (key == "function") {
                if (!finishNode()) return report(error);
                if (!parseFunction(l, in)) return report(error);
                continue;
            }
            if (!inNode_) return fail("expected 'node' or 'function', found '" + key + "'", error);
            if (!parseNodeLine(key, l)) return report(error);
        }
        if (!finishNode()) return report(error);
        return true;
    }

private:
    static std::string stripComment(const std::string& s) {
        const size_t hash = s.find('#');
        return hash == std::string::npos ? s : s.substr(0, hash);
    }
    std::string where() const { return origin_ + ":" + std::to_string(line_); }
    bool fail(const std::string& msg) {
        if (error_.empty()) error_ = where() + ": " + msg;
        return false;
    }
    bool fail(const std::string& msg, std::string& error) {
        fail(msg);
        return report(error);
    }
    bool report(std::string& error) {
        error = error_;
        return false;
    }

    bool uniqueName(const std::string& name) {
        if (!isIdentifier(name)) return fail("'" + name + "' is not a valid name");
        if (current_.input(name) || current_.param(name) || current_.output(name)) {
            return fail("'" + name + "' is already used in node '" + current_.name + "'");
        }
        return true;
    }

    bool parseNodeLine(const std::string& key, Line& l) {
        NodeDef& n = current_;
        if (key == "label") { n.label = l.rest(); return true; }
        if (key == "category") { n.category = l.rest(); return true; }
        if (key == "description") { n.description = l.rest(); return true; }
        if (key == "version") {
            const std::string v = l.word();
            try {
                n.version = std::stoi(v);
            } catch (...) {
                return fail("expected a version number");
            }
            return true;
        }
        if (key == "kind") {
            if (l.word() != "output") return fail("the only kind is 'output'");
            n.isOutput = true;
            return true;
        }
        if (key == "uses") {
            for (std::string f = l.word(); !f.empty(); f = l.word()) n.functions.push_back(f);
            return true;
        }
        if (key == "in") return parseInput(l);
        if (key == "param") return parseParam(l);
        if (key == "uniform") return parseUniform(l);
        if (key == "out") return parseOutput(l);
        if (key == "impl") return parseImpl(l);
        return fail("unknown keyword '" + key + "'");
    }

    /// in <name> <type> [= <numbers> | = $<global>] [color] [stage vertex|fragment]
    bool parseInput(Line& l) {
        PortDef p;
        p.name = l.word();
        if (!uniqueName(p.name)) return false;
        const auto type = typeFromName(l.word());
        if (!type) return fail("expected a type for input '" + p.name + "'");
        if (*type == Type::Sampler2D) {
            return fail("inputs cannot be samplers -- declare a 'uniform ... sampler2D' instead");
        }
        p.type = *type;
        p.defaultValue = Value::scalar(0.0f).as(p.type == Type::Any ? Type::Float : p.type);

        std::vector<std::string> words;
        const bool hasDefault = l.accept('=');
        for (std::string w = l.word(); !w.empty(); w = l.word()) words.push_back(w);

        // Trailing flags, then whatever is left is the default.
        for (;;) {
            if (words.size() >= 2 && words[words.size() - 2] == "stage") {
                const std::string s = words.back();
                if (s == "vertex") p.stage = Stage::Vertex;
                else if (s == "fragment") p.stage = Stage::Fragment;
                else return fail("stage is 'vertex' or 'fragment'");
                words.resize(words.size() - 2);
            } else if (!words.empty() && words.back() == "color") {
                p.color = true;
                words.pop_back();
            } else {
                break;
            }
        }
        if (hasDefault) {
            if (words.size() == 1 && words[0].size() > 1 && words[0][0] == '$') {
                const GlobalDef* g = findGlobal(words[0].substr(1));
                if (!g) return fail("unknown global '" + words[0] + "'");
                if (!convertible(g->type, p.type)) return fail("global type does not fit the input");
                p.defaultGlobal = g->name;
            } else {
                std::string why;
                std::vector<std::string> nums;
                for (const auto& w : words) {
                    for (const auto& x : splitNumbers(w)) nums.push_back(x);
                }
                if (!valueFor(p.type, nums, p.defaultValue, why)) {
                    return fail("bad default for input '" + p.name + "': " + why);
                }
            }
        } else if (!words.empty()) {
            return fail("unexpected '" + words[0] + "' -- a default needs '='");
        }
        current_.inputs.push_back(std::move(p));
        return true;
    }

    /// param <name> <float|vec2|vec3|vec4|string> [= <value>] [color]
    /// param <name> enum <choice> <choice>... [= <choice>]
    bool parseParam(Line& l) {
        ParamDef p;
        p.name = l.word();
        if (!uniqueName(p.name)) return false;
        const std::string typeWord = l.word();
        if (typeWord == "enum") return parseEnumParam(l, std::move(p));
        if (typeWord == "string") {
            p.isString = true;
        } else {
            const auto type = typeFromName(typeWord);
            if (!type || !isNumeric(*type)) return fail("a param is a number, vector or string");
            p.type = *type;
            p.value = Value::scalar(0.0f).as(p.type);
        }
        std::vector<std::string> words;
        const bool hasValue = l.accept('=');
        for (std::string w = l.word(); !w.empty(); w = l.word()) words.push_back(w);
        if (!words.empty() && words.back() == "color") {
            p.color = true;
            words.pop_back();
        }
        if (hasValue) {
            if (p.isString) {
                if (words.size() != 1 || !isIdentifier(words[0])) {
                    return fail("a string param holds one name, like 'albedo'");
                }
                p.text = words[0];
            } else {
                std::string why;
                std::vector<std::string> nums;
                for (const auto& w : words) {
                    for (const auto& x : splitNumbers(w)) nums.push_back(x);
                }
                if (!valueFor(p.type, nums, p.value, why)) return fail("bad param value: " + why);
            }
        } else if (!words.empty()) {
            return fail("unexpected '" + words[0] + "' -- a value needs '='");
        }
        current_.params.push_back(std::move(p));
        return true;
    }

    bool parseEnumParam(Line& l, ParamDef p) {
        p.isString = true;
        for (std::string w = l.word(); !w.empty(); w = l.word()) {
            if (!isIdentifier(w)) return fail("'" + w + "' is not a valid choice name");
            if (std::find(p.choices.begin(), p.choices.end(), w) != p.choices.end()) {
                return fail("choice '" + w + "' is listed twice");
            }
            p.choices.push_back(w);
        }
        if (p.choices.size() < 2) return fail("an enum param needs at least two choices");
        p.text = p.choices.front();
        if (l.accept('=')) {
            p.text = l.word();
            if (std::find(p.choices.begin(), p.choices.end(), p.text) == p.choices.end()) {
                return fail("'" + p.text + "' is not one of the choices of '" + p.name + "'");
            }
        }
        if (!l.atEnd()) return fail("unexpected text after the enum param");
        current_.params.push_back(std::move(p));
        return true;
    }

    /// uniform <name-template> <type> [= <default-template>]
    bool parseUniform(Line& l) {
        UniformDef u;
        u.nameTemplate = l.word();
        const auto type = typeFromName(l.word());
        if (u.nameTemplate.empty() || !type || *type == Type::Any) {
            return fail("expected 'uniform <name> <type>'");
        }
        u.type = *type;
        if (l.accept('=')) u.defaultTemplate = l.rest();
        if (!l.atEnd()) return fail("unexpected text after the uniform");
        if (!checkTemplate(u.nameTemplate, /*outputsSoFar=*/0, /*paramsOnly=*/true)) return false;
        if (!u.defaultTemplate.empty() &&
            !checkTemplate(u.defaultTemplate, 0, /*paramsOnly=*/true)) {
            return false;
        }
        current_.uniforms.push_back(std::move(u));
        return true;
    }

    /// out <name> <type> = <template>
    bool parseOutput(Line& l) {
        OutputDef o;
        o.name = l.word();
        if (!uniqueName(o.name)) return false;
        const auto type = typeFromName(l.word());
        if (!type || *type == Type::Sampler2D) return fail("expected a numeric type or 'any'");
        o.type = *type;
        if (!l.accept('=')) return fail("expected '=' and a template");
        o.code = l.rest();
        if (o.code.empty()) return fail("empty template");
        if (!checkTemplate(o.code, current_.outputs.size(), false)) return false;
        current_.outputs.push_back(std::move(o));
        return true;
    }

    /// impl <target> <output> = <template>
    bool parseImpl(Line& l) {
        const std::string target = l.word();
        const std::string output = l.word();
        const int index = current_.outputIndex(output);
        if (target.empty() || index < 0) {
            return fail("expected 'impl <target> <output>' after the output is declared");
        }
        if (!l.accept('=')) return fail("expected '=' and a template");
        const std::string code = l.rest();
        if (!checkTemplate(code, static_cast<size_t>(index), false)) return false;
        current_.outputs[static_cast<size_t>(index)].perTarget[target] = code;
        return true;
    }

    /// Every {name} must be an input, a param or an earlier output; every
    /// $name a known global.
    bool checkTemplate(const std::string& tmpl, size_t outputsSoFar, bool paramsOnly) {
        std::vector<TemplatePiece> pieces;
        std::string why;
        if (!parseTemplate(tmpl, pieces, why)) return fail(why);
        for (const auto& p : pieces) {
            if (p.kind == TemplatePiece::Kind::Global) {
                if (paramsOnly) return fail("$" + p.text + " cannot be used here");
                if (!findGlobal(p.text)) return fail("unknown global '$" + p.text + "'");
            } else if (p.kind == TemplatePiece::Kind::Placeholder) {
                const bool isParam = current_.param(p.text) != nullptr;
                const bool isInput = current_.input(p.text) != nullptr;
                const int out = current_.outputIndex(p.text);
                const bool isEarlierOutput = out >= 0 && static_cast<size_t>(out) < outputsSoFar;
                if (paramsOnly ? !isParam : !(isParam || isInput || isEarlierOutput)) {
                    return fail("'{" + p.text + "}' is not " +
                                (paramsOnly ? "a param" : "an input, a param or an earlier output") +
                                " of node '" + current_.name + "'");
                }
            }
        }
        return true;
    }

    bool finishNode() {
        if (!inNode_) return true;
        inNode_ = false;
        NodeDef& n = current_;
        if (n.isOutput) {
            if (!n.outputs.empty()) return failAt(n, "an output node has no outputs");
            if (n.inputs.empty()) return failAt(n, "an output node needs inputs");
        } else if (n.outputs.empty()) {
            return failAt(n, "node '" + n.name + "' has no outputs");
        }
        const bool anyIn = std::any_of(n.inputs.begin(), n.inputs.end(),
                                       [](const PortDef& p) { return p.type == Type::Any; });
        for (const auto& o : n.outputs) {
            if (o.type == Type::Any && !anyIn) {
                return failAt(n, "output '" + o.name + "' is 'any' but no input is");
            }
        }
        nodes_[n.name] = std::move(n);
        return true;
    }

    bool failAt(const NodeDef& n, const std::string& msg) {
        if (error_.empty()) error_ = n.origin + ": " + msg;
        return false;
    }

    /// function <name> [<target>] ... end
    bool parseFunction(Line& l, std::istringstream& in) {
        const std::string name = l.word();
        const std::string target = l.word();
        if (!isIdentifier(name)) return fail("expected a function name");
        FunctionDef& f = functions_[name];
        if (f.name.empty()) {
            f.name = name;
            f.origin = where();
        }
        std::string code, raw;
        bool header = true, closed = false;
        while (std::getline(in, raw)) {
            ++line_;
            const std::string t = trim(raw);
            if (t == "end") {
                closed = true;
                break;
            }
            if (header && t.rfind("uses ", 0) == 0) {
                Line ul(t);
                ul.word();
                for (std::string u = ul.word(); !u.empty(); u = ul.word()) {
                    if (std::find(f.uses.begin(), f.uses.end(), u) == f.uses.end()) f.uses.push_back(u);
                }
                continue;
            }
            header = false;
            code += raw + "\n";
        }
        if (!closed) return fail("function '" + name + "' has no 'end'");
        if (target.empty()) f.code = code;
        else f.perTarget[target] = code;
        return true;
    }

    std::string origin_;
    std::map<std::string, NodeDef>& nodes_;
    std::map<std::string, FunctionDef>& functions_;
    NodeDef current_;
    bool inNode_ = false;
    size_t line_ = 0;
    std::string error_;
};

}  // namespace

const char* stageName(Stage s) { return s == Stage::Vertex ? "vertex" : "fragment"; }

const std::vector<GlobalDef>& globals() {
    static const std::vector<GlobalDef> g = {
        {"position", Type::Vec3, "world-space position of the surface"},
        {"normal", Type::Vec3, "world-space unit normal"},
        {"uv", Type::Vec2, "texture coordinates of the mesh"},
        {"view", Type::Vec3, "unit vector from the surface to the camera"},
        {"light", Type::Vec3, "unit vector from the surface to the light"},
        {"time", Type::Float, "seconds since the start"},
    };
    return g;
}

const GlobalDef* findGlobal(const std::string& name) {
    for (const auto& g : globals()) {
        if (name == g.name) return &g;
    }
    return nullptr;
}

bool parseTemplate(const std::string& tmpl, std::vector<TemplatePiece>& out, std::string& error) {
    out.clear();
    std::string code;
    auto flush = [&] {
        if (!code.empty()) out.push_back({TemplatePiece::Kind::Code, code});
        code.clear();
    };
    for (size_t i = 0; i < tmpl.size();) {
        const char c = tmpl[i];
        if (c == '{' || c == '$') {
            size_t j = i + 1;
            while (j < tmpl.size() && isIdentChar(tmpl[j])) ++j;
            const std::string name = tmpl.substr(i + 1, j - i - 1);
            if (!isIdentifier(name)) {
                error = std::string("'") + c + "' must be followed by a name in '" + tmpl + "'";
                return false;
            }
            if (c == '{') {
                if (j >= tmpl.size() || tmpl[j] != '}') {
                    error = "unmatched '{' in '" + tmpl + "'";
                    return false;
                }
                ++j;
            }
            flush();
            out.push_back({c == '{' ? TemplatePiece::Kind::Placeholder : TemplatePiece::Kind::Global,
                           name});
            i = j;
            continue;
        }
        if (c == '}') {
            error = "unmatched '}' in '" + tmpl + "'";
            return false;
        }
        code += c;
        ++i;
    }
    flush();
    return true;
}

const PortDef* NodeDef::input(const std::string& n) const {
    for (const auto& p : inputs) {
        if (p.name == n) return &p;
    }
    return nullptr;
}

const OutputDef* NodeDef::output(const std::string& n) const {
    for (const auto& o : outputs) {
        if (o.name == n) return &o;
    }
    return nullptr;
}

const ParamDef* NodeDef::param(const std::string& n) const {
    for (const auto& p : params) {
        if (p.name == n) return &p;
    }
    return nullptr;
}

int NodeDef::inputIndex(const std::string& n) const {
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

int NodeDef::outputIndex(const std::string& n) const {
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

NodeLibrary NodeLibrary::withBuiltins() {
    NodeLibrary lib;
    std::string error;
    if (!lib.load(builtinLibrarySource(), "builtin.pgnodes", error)) {
        // A bug in builtin.pgnodes, not a user error: the tests load it too.
        throw std::logic_error("built-in node library: " + error);
    }
    return lib;
}

bool NodeLibrary::load(const std::string& text, const std::string& origin, std::string& error) {
    // Parse into copies so a failed load leaves the library as it was.
    std::map<std::string, NodeDef> nodes;
    std::map<std::string, FunctionDef> functions;
    LibraryParser parser(origin, nodes, functions);
    if (!parser.parse(text, error)) return false;
    for (auto& [name, def] : nodes) nodes_[name] = std::move(def);
    for (auto& [name, def] : functions) {
        FunctionDef& f = functions_[name];
        if (!def.code.empty() || f.name.empty()) {
            f.name = def.name;
            f.code = def.code;
            f.uses = def.uses;
            f.origin = def.origin;
        }
        for (auto& [target, code] : def.perTarget) f.perTarget[target] = code;
    }
    return true;
}

bool NodeLibrary::loadFile(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = path + ": cannot open";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return load(ss.str(), path, error);
}

const NodeDef* NodeLibrary::find(const std::string& name) const {
    auto it = nodes_.find(name);
    return it == nodes_.end() ? nullptr : &it->second;
}

const FunctionDef* NodeLibrary::findFunction(const std::string& name) const {
    auto it = functions_.find(name);
    return it == functions_.end() ? nullptr : &it->second;
}

std::vector<const NodeDef*> NodeLibrary::nodes() const {
    std::vector<const NodeDef*> out;
    out.reserve(nodes_.size());
    for (const auto& [name, def] : nodes_) out.push_back(&def);
    std::sort(out.begin(), out.end(), [](const NodeDef* a, const NodeDef* b) {
        return a->category != b->category ? a->category < b->category : a->label < b->label;
    });
    return out;
}

std::vector<std::string> NodeLibrary::categories() const {
    std::set<std::string> cats;
    for (const auto& [name, def] : nodes_) cats.insert(def.category);
    return {cats.begin(), cats.end()};
}

}  // namespace pg::shader
