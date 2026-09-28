#include "pg/usd/Stage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <tuple>

namespace pg::usd {

namespace {

namespace fs = std::filesystem;

double timeCodesPerSecondOf(const Layer& layer) {
    if (const Value* v = layer.meta("timeCodesPerSecond")) return v->number(24.0);
    if (const Value* v = layer.meta("framesPerSecond")) return v->number(24.0);
    return 24.0;
}

/// The layers of a stack -- a layer and its sublayers, theirs in turn --
/// the strongest first, each with its time to the stack's.
struct LayerStack {
    std::string identifier;
    struct Entry {
        std::shared_ptr<const Layer> layer;
        TimeMap time;
    };
    std::vector<Entry> layers;
    const Layer& root() const { return *layers.front().layer; }
};

enum class Arc : uint8_t { Root, Inherit, Variant, Reference, Payload, Specialize };

/// A node of a prim's index: a place in a layer stack whose specs are
/// opinions about the prim, and the arcs it brings in, strongest first.
struct Node {
    const LayerStack* stack = nullptr;
    std::string path;            ///< the site, variant selections included
    TimeMap time;                ///< the stack's time to the stage's
    std::string source, target;  ///< what maps the site's namespace to the stage's
    Arc arc = Arc::Root;
    int depth = 0;               ///< the depth of the prim the arc was made on
    int order = 0;               ///< among arcs of its kind made together
    std::string variantSet;      ///< for a variant: its set
    std::vector<Node> children;
};

bool weaker(const Node& a, const Node& b) {
    if (a.arc != b.arc) return a.arc > b.arc;
    if (a.depth != b.depth) return a.depth < b.depth;  // made nearer the prim: stronger
    return a.order > b.order;
}

/// A list edit's item with where it was written.
struct Sourced {
    ListItem item;
    const Layer* layer = nullptr;
    TimeMap time;
};

bool namedBy(const ListOp& op, const ListItem& item) {
    auto in = [&](const std::vector<ListItem>& v) { return std::find(v.begin(), v.end(), item) != v.end(); };
    return in(op.explicitItems) || in(op.prepended) || in(op.appended) || in(op.added);
}

std::string mapPath(const std::string& path, const std::string& source, const std::string& target) {
    const std::string p = stripVariants(path);
    const std::string s = stripVariants(source);
    if (s == "/") return target == "/" ? p : target + p;
    if (p == s) return target;
    if (p.size() > s.size() && p.compare(0, s.size(), s) == 0 && (p[s.size()] == '/' || p[s.size()] == '.')) {
        return target + p.substr(s.size());
    }
    return p;
}

std::string stampOf(const std::string& identifier) {
    std::string file = identifier;
    if (const size_t open = file.find('['); open != std::string::npos && !file.empty() && file.back() == ']') {
        file = file.substr(0, open);
    }
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (ec) return "missing";
    const auto time = fs::last_write_time(file, ec);
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

Value sampleAt(const std::vector<double>& times, const std::vector<Value>& samples, double t) {
    if (times.empty()) return {};
    if (t <= times.front()) return samples.front().blocked() ? Value{} : samples.front();
    if (t >= times.back()) return samples.back().blocked() ? Value{} : samples.back();
    const size_t upper = static_cast<size_t>(std::lower_bound(times.begin(), times.end(), t) - times.begin());
    if (times[upper] == t) return samples[upper].blocked() ? Value{} : samples[upper];
    const size_t lower = upper - 1;
    return interpolate(samples[lower], samples[upper], times[lower], times[upper], t);
}

/// SdfPath's HasPrefix on the text of paths: `p` is `prefix` or under it.
bool hasPrefix(std::string_view p, std::string_view prefix) {
    if (prefix == "/") return !p.empty() && p[0] == '/';
    if (p.size() < prefix.size() || p.compare(0, prefix.size(), prefix) != 0) return false;
    if (p.size() == prefix.size()) return true;
    const char c = p[prefix.size()];
    return c == '/' || c == '{' || c == '.' || prefix.back() == '}';
}

/// Whether the file an identifier names is there: on disk, or in its package.
bool assetExists(const std::string& identifier) {
    std::error_code ec;
    const size_t open = identifier.find('[');
    if (open == std::string::npos || identifier.back() != ']') return fs::is_regular_file(identifier, ec);
    std::vector<std::string> names;
    std::string error;
    if (!packageFiles(identifier.substr(0, open), names, error)) return false;
    const std::string inner = identifier.substr(open + 1, identifier.size() - open - 2);
    return std::find(names.begin(), names.end(), inner) != names.end();
}

}  // namespace

Value interpolate(const Value& a, const Value& b, double ta, double tb, double t) {
    if (a.blocked()) return {};
    if (b.blocked() || !a.isNumbers() || !b.isNumbers() || a.numbers.size() != b.numbers.size() ||
        a.width != b.width || !interpolates(a.type) || tb <= ta) {
        return a;
    }
    const double u = (t - ta) / (tb - ta);
    Value out = a;
    if (a.type.rfind("quat", 0) == 0 && a.width == 4) {
        // Along the shorter arc, as GfSlerp.
        for (size_t k = 0; k + 3 < a.numbers.size(); k += 4) {
            double c = 0.0;
            for (int i = 0; i < 4; ++i) c += a.numbers[k + i] * b.numbers[k + i];
            const bool flip = c < 0.0;
            if (flip) c = -c;
            double s0 = 1.0 - u, s1 = u;
            if (1.0 - c > 1e-5) {
                const double theta = std::acos(std::min(c, 1.0)), s = std::sin(theta);
                s0 = std::sin((1.0 - u) * theta) / s;
                s1 = std::sin(u * theta) / s;
            }
            if (flip) s1 = -s1;
            for (int i = 0; i < 4; ++i) out.numbers[k + i] = s0 * a.numbers[k + i] + s1 * b.numbers[k + i];
        }
        return out;
    }
    // As GfLerp: (1 - u) a + u b.
    for (size_t k = 0; k < a.numbers.size(); ++k) out.numbers[k] = (1.0 - u) * a.numbers[k] + u * b.numbers[k];
    return out;
}

// --- Composing --------------------------------------------------------------------------------

struct Stage::Build {
    /// A variant selection from outside a nested index: from the prims
    /// already composed that a site of its stack maps to (as USD looks
    /// through its stack frames). "" if none.
    using Outer = std::function<std::string(const LayerStack* stack, const std::string& path, const std::string& set)>;

    Stage& stage;
    std::map<std::string, std::shared_ptr<const Layer>> layers;
    std::map<std::string, std::unique_ptr<LayerStack>> stacks;
    const LayerStack* rootStack = nullptr;
    int budget = 1000000;  // nodes made, against files that refer to themselves without end

    explicit Build(Stage& s) : stage(s) {}

    void warn(std::string why) {
        if (std::find(stage.warnings_.begin(), stage.warnings_.end(), why) == stage.warnings_.end()) {
            stage.warnings_.push_back(std::move(why));
        }
    }

    std::shared_ptr<const Layer> layer(const std::string& identifier, const std::string& from) {
        if (const auto it = layers.find(identifier); it != layers.end()) return it->second;
        std::string error;
        std::shared_ptr<const Layer> l = readLayer(identifier, error);
        if (!l) warn(error + (from.empty() ? "" : " (named in " + from + ")"));
        layers[identifier] = l;
        if (l) stage.layers_.push_back(l);
        return l;
    }

    void addLayers(LayerStack& s, const std::shared_ptr<const Layer>& l, TimeMap time, std::vector<std::string>& visiting) {
        s.layers.push_back({l, time});
        visiting.push_back(l->identifier);
        for (size_t i = 0; i < l->subLayers.size(); ++i) {
            const std::string id = resolveAsset(l->subLayers[i], l->identifier);
            if (std::find(visiting.begin(), visiting.end(), id) != visiting.end()) {
                warn("a sublayer that includes itself: " + id);
                continue;
            }
            const std::shared_ptr<const Layer> sub = layer(id, l->identifier);
            if (!sub) continue;
            const auto [offset, scale] = i < l->subLayerOffsets.size() ? l->subLayerOffsets[i] : std::pair{0.0, 1.0};
            const double rate = timeCodesPerSecondOf(*l) / timeCodesPerSecondOf(*sub);
            addLayers(s, sub, time.after(TimeMap{offset, scale * rate}), visiting);
        }
        visiting.pop_back();
    }

    const LayerStack* stackFor(const std::string& identifier, const std::string& from) {
        if (const auto it = stacks.find(identifier); it != stacks.end()) return it->second.get();
        std::unique_ptr<LayerStack>& slot = stacks[identifier];
        const std::shared_ptr<const Layer> root = layer(identifier, from);
        if (!root) return nullptr;
        auto s = std::make_unique<LayerStack>();
        s->identifier = identifier;
        std::vector<std::string> visiting;
        addLayers(*s, root, {}, visiting);
        slot = std::move(s);
        return slot.get();
    }

    /// The specs at a site, the strongest first, with the time of their layers.
    std::vector<std::pair<const LayerStack::Entry*, const PrimSpec*>> specsAt(const LayerStack& s, const std::string& path) {
        std::vector<std::pair<const LayerStack::Entry*, const PrimSpec*>> out;
        for (const LayerStack::Entry& e : s.layers) {
            if (const PrimSpec* spec = e.layer->prim(path)) out.emplace_back(&e, spec);
        }
        return out;
    }

    /// A list edit of the specs at a site, composed from the weakest to the
    /// strongest: its items, each with the layer that wrote it.
    std::vector<Sourced> composed(const std::vector<std::pair<const LayerStack::Entry*, const PrimSpec*>>& specs,
                                  ListOp PrimSpec::*member) {
        std::vector<Sourced> out;
        for (auto it = specs.rbegin(); it != specs.rend(); ++it) {
            const ListOp& op = it->second->*member;
            if (op.empty()) continue;
            std::vector<ListItem> items;
            for (const Sourced& s : out) items.push_back(s.item);
            op.apply(items);
            std::vector<Sourced> next;
            for (const ListItem& item : items) {
                if (namedBy(op, item)) {
                    next.push_back({item, it->first->layer.get(), it->first->time});
                } else {
                    for (const Sourced& s : out) {
                        if (s.item == item) {
                            next.push_back(s);
                            break;
                        }
                    }
                }
            }
            out = std::move(next);
        }
        return out;
    }

    /// The arcs written at `n`'s site -- all but variants -- as its children,
    /// each expanded in turn. `chain`: the sites above, against cycles.
    void expand(Node& n, const std::string& stagePath, int depth, std::vector<std::pair<const LayerStack*, std::string>>& chain) {
        const auto specs = specsAt(*n.stack, n.path);
        if (specs.empty()) return;
        auto add = [&](Arc arc, const LayerStack* stack, std::string site, TimeMap time, int order) {
            if (!stack) return;
            if (site.empty()) {
                const std::string d = stack->root().text("defaultPrim");
                if (d.empty()) {
                    warn("a reference to " + stack->identifier + ", which names no default prim");
                    return;
                }
                site = d[0] == '/' ? d : "/" + d;
            }
            for (const auto& [s, p] : chain) {
                if (s == stack && p == site) {
                    warn("an arc that leads back to itself: " + site + " in " + stack->identifier);
                    return;
                }
            }
            if (--budget < 0) {
                warn("too many arcs: the composition was cut short");
                return;
            }
            Node c;
            std::vector<PathElement> elements;
            const bool nested = splitPath(site, elements) && elements.size() > 1 &&
                                std::all_of(elements.begin(), elements.end(),
                                            [](const PathElement& e) { return e.kind == PathElement::Kind::Prim; });
            chain.emplace_back(stack, site);
            if (nested) {
                // A prim below a root one: its whole index there, with what
                // it gets from its ancestors (their variants, references...).
                // The variants of those ancestors are chosen by the prims
                // they map to, where there are some: the shot's choice holds
                // inside the asset too.
                const std::string source = n.source, target = n.target;
                const Outer outer = [this, stack, source, target](const LayerStack* at, const std::string& path,
                                                                  const std::string& set) -> std::string {
                    if (at != stack) return {};
                    const std::string from = stripVariants(source), site = stripVariants(path);
                    const bool inside = from == "/" || site == from ||
                                        (site.size() > from.size() && site.compare(0, from.size(), from) == 0 &&
                                         site[from.size()] == '/');
                    if (!inside) return {};
                    const std::string mapped = mapPath(path, source, target);
                    const auto it = stage.byPath_.find(mapped);
                    if (it == stage.byPath_.end()) return {};
                    for (const Opinion& o : it->second->opinions) {
                        const std::string_view v = o.spec->selection(set);
                        if (!v.empty()) return std::string(v);
                    }
                    return {};
                };
                c = indexAt(*stack, elements, chain, stack == n.stack ? &outer : nullptr);
                graft(c, time, site, stagePath, depth - static_cast<int>(elements.size()));
            } else {
                c.stack = stack;
                c.path = site;
                c.time = time;
                c.source = site;
                c.target = stagePath;
                expand(c, stagePath, depth, chain);
            }
            chain.pop_back();
            c.arc = arc;
            c.depth = depth;
            c.order = order;
            n.children.push_back(std::move(c));
        };
        int order = 0;
        for (const Sourced& s : composed(specs, &PrimSpec::inherits)) {
            add(Arc::Inherit, n.stack, s.item.text, n.time, order++);
        }
        for (int kind = 0; kind < 2; ++kind) {
            order = 0;
            for (const Sourced& s : composed(specs, kind == 0 ? &PrimSpec::references : &PrimSpec::payloads)) {
                const ListItem& r = s.item;
                TimeMap time = n.time.after(s.time);
                const LayerStack* target = n.stack;
                if (!r.text.empty()) {
                    target = stackFor(resolveAsset(r.text, s.layer->identifier), s.layer->identifier);
                    if (!target) continue;
                    const double rate = timeCodesPerSecondOf(n.stack->root()) / timeCodesPerSecondOf(target->root());
                    time = time.after(TimeMap{r.offset, r.scale * rate});
                } else {
                    time = time.after(TimeMap{r.offset, r.scale});
                }
                add(kind == 0 ? Arc::Reference : Arc::Payload, target, r.path, time, order++);
            }
        }
        order = 0;
        for (const Sourced& s : composed(specs, &PrimSpec::specializes)) {
            add(Arc::Specialize, n.stack, s.item.text, n.time, order++);
        }
    }

    /// The index of `elements` (a prim path) in `stack` as a stage of its
    /// own would have it -- the variants of the prim itself left to the
    /// index it goes into, whose opinions choose them.
    Node indexAt(const LayerStack& stack, const std::vector<PathElement>& elements,
                 std::vector<std::pair<const LayerStack*, std::string>>& chain, const Outer* outer = nullptr) {
        Node n;
        n.stack = &stack;
        n.path = "/" + elements[0].name;
        n.source = n.path;
        n.target = n.path;
        n.depth = 1;
        std::string at = n.path;
        expand(n, at, 1, chain);
        if (elements.size() > 1) expandVariants(n, at, 1, outer);
        for (size_t k = 1; k < elements.size(); ++k) {
            const std::string next = childPath(at, elements[k].name);
            n = childIndex(n, elements[k].name, next, static_cast<int>(k) + 1, k + 1 < elements.size(), outer);
            at = next;
        }
        return n;
    }

    /// A subtree made on its own set into another index: its times through
    /// the arc's, its namespace the arc's, the depths of the prims its arcs
    /// were made on the depths they map to.
    static void graft(Node& n, const TimeMap& time, const std::string& site, const std::string& stagePath, int shift) {
        n.time = time.after(n.time);
        n.source = site;
        n.target = stagePath;
        n.depth += shift;
        for (Node& c : n.children) graft(c, time, site, stagePath, shift);
    }

    /// Every node, the strongest first.
    static void strengthOrder(Node& n, std::vector<Node*>& out) {
        out.push_back(&n);
        std::stable_sort(n.children.begin(), n.children.end(), [](const Node& a, const Node& b) { return weaker(b, a); });
        for (Node& c : n.children) strengthOrder(c, out);
    }

    /// The variant sets of every node, their selections made by the
    /// strongest opinion anywhere in the index; what the variants bring in,
    /// in turn.
    void expandVariants(Node& root, const std::string& stagePath, int depth, const Outer* outer = nullptr) {
        for (int guard = 0; guard < 256; ++guard) {
            std::vector<Node*> order;
            strengthOrder(root, order);
            bool added = false;
            for (Node* n : order) {
                const auto specs = specsAt(*n->stack, n->path);
                if (specs.empty()) continue;
                for (const Sourced& s : composed(specs, &PrimSpec::variantSetNames)) {
                    const std::string& set = s.item.text;
                    bool done = false;
                    for (const Node& c : n->children) done = done || (c.arc == Arc::Variant && c.variantSet == set);
                    if (done) continue;
                    std::string selection = outer ? (*outer)(n->stack, n->path, set) : std::string();
                    for (const Node* m : order) {
                        if (!selection.empty()) break;
                        for (const auto& [entry, spec] : specsAt(*m->stack, m->path)) {
                            const std::string_view v = spec->selection(set);
                            if (!v.empty()) {
                                selection = std::string(v);
                                break;
                            }
                        }
                        if (!selection.empty()) break;
                    }
                    if (selection.empty()) continue;
                    Node c;
                    c.stack = n->stack;
                    c.path = std::string(primPart(n->path)) + "{" + set + "=" + selection + "}";
                    c.time = n->time;
                    c.source = n->source;
                    c.target = n->target;
                    c.arc = Arc::Variant;
                    c.depth = depth;
                    c.variantSet = set;
                    std::vector<std::pair<const LayerStack*, std::string>> chain;
                    expand(c, stagePath, depth, chain);
                    n->children.push_back(std::move(c));
                    added = true;
                    break;
                }
                if (added) break;
            }
            if (!added) return;
        }
        warn("variants nested too deep under " + stagePath);
    }

    /// Whether any node of the subtree has a spec; those without are dropped.
    bool prune(Node& n) {
        bool any = !specsAt(*n.stack, n.path).empty();
        std::vector<Node> kept;
        for (Node& c : n.children) {
            if (prune(c)) kept.push_back(std::move(c));
        }
        n.children = std::move(kept);
        return any || !n.children.empty();
    }

    /// `parent`'s index made the index of its child `name`: every node's
    /// site one prim further (arcs from ancestors), then the arcs written
    /// on the child's own sites.
    Node childIndex(const Node& parent, const std::string& name, const std::string& stagePath, int depth,
                    bool variants = true, const Outer* outer = nullptr) {
        std::function<Node(const Node&)> clone = [&](const Node& n) {
            Node c;
            c.stack = n.stack;
            c.path = childPath(n.path, name);
            c.time = n.time;
            c.source = n.source;
            c.target = n.target;
            c.arc = n.arc;
            c.depth = n.depth;
            c.order = n.order;
            c.variantSet = n.variantSet;
            for (const Node& k : n.children) c.children.push_back(clone(k));
            return c;
        };
        Node index = clone(parent);
        std::function<void(Node&)> direct = [&](Node& n) {
            const size_t existing = n.children.size();
            for (size_t i = 0; i < existing; ++i) direct(n.children[i]);
            std::vector<std::pair<const LayerStack*, std::string>> chain{{n.stack, n.path}};
            expand(n, stagePath, depth, chain);
        };
        direct(index);
        if (variants) expandVariants(index, stagePath, depth, outer);
        prune(index);
        return index;
    }

    /// A node as its prim's opinions have it: where its own start, the
    /// layer of each in its stack.
    struct Placed {
        const LayerStack* stack = nullptr;
        std::string path;
        size_t first = 0;
        std::vector<size_t> layers;
        bool viaInherit = false;  ///< it, or a node above it, is an inherit or specialize made on the prim
    };

    /// The opinions of an index, the strongest first: each node's specs in
    /// the order of its stack, the nodes in the order of strength -- what
    /// is specialized after all the rest, as USD moves it, in the order it
    /// was found.
    void flatten(Node& root, int depth, std::vector<Opinion>& out, std::vector<Placed>& placed) {
        struct Visit {
            Node* node;
            int group;  ///< -1; the pre-order place of the specialize it is under
            bool viaInherit;
        };
        std::vector<Visit> order;
        std::function<void(Node&, int, bool)> walk = [&](Node& n, int group, bool viaInherit) {
            if (n.arc == Arc::Specialize) group = static_cast<int>(order.size());
            viaInherit = viaInherit || ((n.arc == Arc::Inherit || n.arc == Arc::Specialize) && n.depth == depth);
            order.push_back({&n, group, viaInherit});
            std::stable_sort(n.children.begin(), n.children.end(), [](const Node& a, const Node& b) { return weaker(b, a); });
            for (Node& c : n.children) walk(c, group, viaInherit);
        };
        walk(root, -1, false);
        std::stable_sort(order.begin(), order.end(), [](const Visit& a, const Visit& b) { return a.group < b.group; });
        for (const Visit& v : order) {
            const Node& n = *v.node;
            const int id = static_cast<int>(stage.nodeMaps_.size());
            stage.nodeMaps_.push_back({n.source, n.target});
            Placed p{n.stack, n.path, out.size(), {}, v.viaInherit};
            for (size_t i = 0; i < n.stack->layers.size(); ++i) {
                const LayerStack::Entry& e = n.stack->layers[i];
                if (const PrimSpec* spec = e.layer->prim(n.path)) {
                    out.push_back({e.layer.get(), spec, n.time.after(e.time), id});
                    p.layers.push_back(i);
                }
            }
            placed.push_back(std::move(p));
        }
    }

    // --- Value clips ---------------------------------------------------------------------------

    /// A clip set's fields as the opinions compose them -- each from the
    /// strongest that has it -- and where it is anchored: the strongest
    /// layer that names its clips.
    struct ClipSetSpec {
        std::string name;
        const Value *assets = nullptr, *primPath = nullptr, *manifest = nullptr, *pattern = nullptr,
                    *stride = nullptr, *startTime = nullptr, *endTime = nullptr, *activeOffset = nullptr,
                    *interpolateMissing = nullptr;
        std::optional<std::vector<std::pair<double, double>>> active, times;  ///< in stage time
        const Placed* node = nullptr;  ///< the anchor
        size_t layer = 0;
        const Opinion* opinion = nullptr;
        size_t rank = 0, order = 0;  ///< the anchor node's strength; the set's place in its stack
    };

    static ListItem named(const std::string& name) {
        ListItem item;
        item.text = name;
        return item;
    }

    /// The pairs of a double2[] in stage time through `time`.
    static std::vector<std::pair<double, double>> pairs(const Value& v, const TimeMap& time) {
        std::vector<std::pair<double, double>> out;
        for (size_t k = 0; k + 1 < v.numbers.size(); k += 2) {
            if (!std::isnan(v.numbers[k]) && !std::isnan(v.numbers[k + 1])) {
                out.emplace_back(time.toStage(v.numbers[k]), v.numbers[k + 1]);
            }
        }
        return out;
    }

    /// The clip sets a prim's opinions name (its `clips` metadata), as USD
    /// composes them: in each node, its layers from the weakest, a stronger
    /// one's fields over a weaker one's, the sets its `clipSets` leaves;
    /// then the nodes, a stronger one's fields over a weaker one's.
    std::vector<ClipSetSpec> clipSets(const std::vector<Opinion>& opinions, const std::vector<Placed>& placed) {
        std::vector<ClipSetSpec> composed;
        for (size_t r = 0; r < placed.size(); ++r) {
            const Placed& n = placed[r];
            std::vector<ClipSetSpec> inNode;
            std::vector<ListItem> names;
            for (size_t j = n.layers.size(); j-- > 0;) {
                const Opinion& o = opinions[n.first + j];
                if (const Value* clips = o.spec->meta("clips"); clips && clips->dictionary) {
                    std::vector<std::string> here;
                    for (const auto& [name, set] : *clips->dictionary) {
                        if (name.empty() || !set.dictionary) continue;
                        auto it = std::find_if(inNode.begin(), inNode.end(), [&](const ClipSetSpec& s) { return s.name == name; });
                        if (it == inNode.end()) {
                            inNode.push_back({});
                            it = inNode.end() - 1;
                            it->name = name;
                        }
                        ClipSetSpec& s = *it;
                        if (set.find("assetPaths") || set.find("templateAssetPath")) {
                            s.node = &n;
                            s.layer = n.layers[j];
                            s.opinion = &o;
                            s.rank = r;
                        }
                        const auto take = [&](const char* key, const Value*& field) {
                            if (const Value* v = set.find(key)) field = v;
                        };
                        take("assetPaths", s.assets);
                        take("primPath", s.primPath);
                        take("manifestAssetPath", s.manifest);
                        take("templateAssetPath", s.pattern);
                        take("templateStride", s.stride);
                        take("templateStartTime", s.startTime);
                        take("templateEndTime", s.endTime);
                        take("templateActiveOffset", s.activeOffset);
                        take("interpolateMissingClipValues", s.interpolateMissing);
                        if (const Value* v = set.find("active")) s.active = pairs(*v, o.time);
                        if (const Value* v = set.find("times")) s.times = pairs(*v, o.time);
                        here.push_back(name);
                    }
                    // What the dictionary names is added to the list, in order of name.
                    std::sort(here.begin(), here.end());
                    ListOp add;
                    for (const std::string& h : here) add.added.push_back(named(h));
                    add.apply(names);
                }
                if (const Value* list = o.spec->meta("clipSets")) {
                    if (list->list) {
                        list->list->apply(names);
                    } else if (list->isStrings()) {
                        names.clear();
                        for (const std::string& h : list->strings) names.push_back(named(h));
                    }
                }
            }
            for (ClipSetSpec& s : inNode) {
                const auto it = std::find(names.begin(), names.end(), named(s.name));
                if (it == names.end()) continue;
                s.order = static_cast<size_t>(it - names.begin());
                auto c = std::find_if(composed.begin(), composed.end(), [&](const ClipSetSpec& x) { return x.name == s.name; });
                if (c == composed.end()) {
                    composed.push_back(s);
                    continue;
                }
                if (!c->node) {
                    c->node = s.node;
                    c->layer = s.layer;
                    c->opinion = s.opinion;
                    c->rank = s.rank;
                    c->order = s.order;
                }
                for (auto field : {&ClipSetSpec::assets, &ClipSetSpec::primPath, &ClipSetSpec::manifest,
                                   &ClipSetSpec::pattern, &ClipSetSpec::stride, &ClipSetSpec::startTime,
                                   &ClipSetSpec::endTime, &ClipSetSpec::activeOffset,
                                   &ClipSetSpec::interpolateMissing}) {
                    if (!((*c).*field)) (*c).*field = s.*field;
                }
                if (!c->active) c->active = s.active;
                if (!c->times) c->times = s.times;
            }
        }
        std::erase_if(composed, [](const ClipSetSpec& s) { return !s.node; });
        std::stable_sort(composed.begin(), composed.end(), [](const ClipSetSpec& a, const ClipSetSpec& b) {
            return std::tie(a.rank, a.node->path, a.order) < std::tie(b.rank, b.node->path, b.order);
        });
        return composed;
    }

    /// The clips a template names ("sim/fx.###.usd", "fx.###.###.usd" for
    /// subframes): the files of the pattern there are, from the start time
    /// to the end by the stride, each active from its time (less the offset)
    /// and holding its own time -- as USD derives them.
    void fromTemplate(const ClipSetSpec& s, const std::string& prim, std::vector<std::string>& assets,
                      std::vector<std::pair<double, double>>& active, std::vector<std::pair<double, double>>& times) {
        const std::string pattern = s.pattern->text();
        const double stride = s.stride->number(), start = s.startTime->number(), end = s.endTime->number();
        const bool offset = s.activeOffset && s.activeOffset->isNumbers();
        const double activeOffset = offset ? s.activeOffset->number() : 0.0;
        const std::string where = " in the clips of " + prim + " (" + s.name + ")";
        if (!(stride > 0.0)) return warn("a template stride that is not above 0" + where);
        if (offset && std::abs(activeOffset) > stride) return warn("a template active offset larger than the stride" + where);
        if (!(std::abs(start) < 1e9 && std::abs(end) < 1e9)) return warn("a template's times out of reach" + where);
        if (start > end) return warn("a template that starts after it ends" + where);
        if ((end - start) / stride > 1e6) return warn("a template of more than a million clips" + where);
        const size_t slash = pattern.find_last_of('/');
        const std::string folder = slash == std::string::npos ? std::string() : pattern.substr(0, slash + 1);
        std::vector<std::string> parts;
        for (size_t i = slash == std::string::npos ? 0 : slash + 1; i <= pattern.size();) {
            size_t dot = pattern.find('.', i);
            if (dot == std::string::npos) dot = pattern.size();
            if (dot > i) parts.push_back(pattern.substr(i, dot - i));
            i = dot + 1;
        }
        size_t whole = std::string::npos, fraction = std::string::npos, groups = 0;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].find_first_not_of('#') != std::string::npos) continue;
            (whole == std::string::npos ? whole : fraction) = i;
            ++groups;
        }
        if ((groups != 1 && groups != 2) || (groups == 2 && whole + 1 != fraction)) {
            return warn("a template asset path that is not name.###.usd or name.###.###.usd" + where);
        }
        const int wholeDigits = static_cast<int>(parts[whole].size());
        const int fractionDigits = groups == 2 ? static_cast<int>(parts[fraction].size()) : 0;
        const std::string& anchor = s.opinion->layer->identifier;
        // As USD counts: in ten-thousandths, so a fractional stride adds up.
        constexpr double promotion = 10000.0;
        const double margin = std::abs(activeOffset) * promotion;
        if (offset) times.emplace_back((start * promotion - margin) / promotion, (start * promotion - margin) / promotion);
        int index = 0;
        size_t steps = 0;
        for (double t = start * promotion; t <= end * promotion; t += stride * promotion) {
            if (++steps > 1000001) break;  // a stride too small to move t on
            const double clipTime = t / promotion;
            char text[128];
            std::snprintf(text, sizeof text, "%0*d", wholeDigits, static_cast<int>(clipTime));
            parts[whole] = text;
            if (fractionDigits > 0) {
                std::snprintf(text, sizeof text, "%.*f", fractionDigits, clipTime);
                const std::string decimal = text;
                parts[fraction] = decimal.substr(decimal.find('.') + 1);
            }
            std::string name = folder;
            for (size_t i = 0; i < parts.size(); ++i) name += (i ? "." : "") + parts[i];
            const std::string id = resolveAsset(name, anchor);
            if (!assetExists(id)) continue;
            assets.push_back(id);
            times.emplace_back(clipTime, clipTime);
            active.emplace_back(offset ? (t + activeOffset * promotion) / promotion : clipTime, index++);
        }
        if (offset) times.emplace_back((end * promotion + margin) / promotion, (end * promotion + margin) / promotion);
        // The anchor layer's offset, applied after: it moves when each clip
        // is used, not which there are.
        for (auto& [stageTime, clip] : times) stageTime = s.opinion->time.toStage(stageTime);
        for (auto& [stageTime, clip] : active) stageTime = s.opinion->time.toStage(stageTime);
    }

    /// A clip set ready to read -- or nothing, with a warning, where USD
    /// would not read it either.
    std::optional<Clips> makeClips(const ClipSetSpec& s, const std::string& prim) {
        const std::string where = " in the clips of " + prim + " (" + s.name + ")";
        std::vector<std::string> assets;
        std::vector<std::pair<double, double>> active, times;
        if (s.assets) {
            for (const std::string& a : s.assets->strings) {
                if (a.empty()) {
                    warn("an empty clip asset path" + where);
                    return std::nullopt;
                }
                assets.push_back(resolveAsset(a, s.opinion->layer->identifier));
            }
            if (s.active) active = *s.active;
            if (s.times) times = *s.times;
        } else if (s.pattern && s.stride && s.startTime && s.endTime) {
            fromTemplate(s, prim, assets, active, times);
        }
        if (!s.primPath || assets.empty() || active.empty()) return std::nullopt;
        Clips c;
        c.name = s.name;
        c.clipPrim = s.primPath->text();
        std::vector<PathElement> elements;
        if (!splitPath(c.clipPrim, elements) || elements.empty() ||
            !std::all_of(elements.begin(), elements.end(), [](const PathElement& e) { return e.kind == PathElement::Kind::Prim; })) {
            warn("a clip prim path that is not a prim's: \"" + c.clipPrim + "\"" + where);
            return std::nullopt;
        }
        std::stable_sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < active.size(); ++k) {
            const double index = active[k].second;
            if (!(index >= 0.0 && index < static_cast<double>(assets.size()))) {
                char text[32];
                std::snprintf(text, sizeof text, "%g", index);
                warn(std::string("a clip index ") + text + " with no clip" + where);
                return std::nullopt;
            }
            if (k > 0 && active[k].first == active[k - 1].first) {
                warn("two clips active from the same time" + where);
                return std::nullopt;
            }
            c.starts.push_back(active[k].first);
            c.layers.push_back(assets[static_cast<size_t>(index)]);
        }
        // Stage time to clip time: two entries at one time are a jump, the
        // first moved just before it (USD's SafeStep); one more at each end.
        std::stable_sort(times.begin(), times.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < times.size(); ++k) {
            if (k >= 2 && times[k].first == times[k - 2].first) {
                warn("more than two clip times at one stage time" + where);
                return std::nullopt;
            }
            c.times.push_back({times[k].first, times[k].second, false});
        }
        constexpr double kSafeStep = std::numeric_limits<double>::epsilon() * 1e6 * 10.0 * 2.0;
        for (size_t k = 0; k + 1 < c.times.size(); ++k) {
            if (c.times[k].stage == c.times[k + 1].stage) {
                c.times[k].stage -= kSafeStep;
                c.times[k].jump = true;
            }
        }
        if (!c.times.empty()) {
            c.times.insert(c.times.begin(), c.times.front());
            c.times.push_back(c.times.back());
        }
        if (s.manifest && !s.manifest->text().empty()) c.manifest = resolveAsset(s.manifest->text(), s.opinion->layer->identifier);
        c.interpolateMissing = s.interpolateMissing && s.interpolateMissing->number() != 0.0;
        c.anchorStack = s.node->stack->identifier;
        c.anchorPrim = s.node->path;
        c.anchorLayer = s.layer;
        return c;
    }

    /// Where a prim reads a clip set: right after the layer that named it,
    /// in the first node of that layer stack at or under the prim that
    /// named it -- and which prim in the clips stands for this one. Never,
    /// when there is no such node.
    static void place(Clips& c, const std::vector<Placed>& placed) {
        c.at = std::numeric_limits<size_t>::max();
        for (const Placed& n : placed) {
            if (n.stack->identifier != c.anchorStack || !hasPrefix(n.path, c.anchorPrim)) continue;
            c.at = n.first;
            for (const size_t layer : n.layers) c.at += layer <= c.anchorLayer ? 1 : 0;
            const std::string from = stripVariants(c.anchorPrim), site = stripVariants(n.path);
            c.primPath = site.size() > from.size() ? c.clipPrim + site.substr(from.size()) : c.clipPrim;
            return;
        }
    }

    void build(Prim& prim, Node& index, int depth) {
        std::vector<Placed> placed;
        flatten(index, depth, prim.opinions, placed);
        // The specifier: the strongest def or class -- but a class that
        // comes through an inherit made on the prim does not make it one;
        // a def weaker than that still counts.
        std::vector<bool> viaInherit(prim.opinions.size(), false);
        for (const Placed& n : placed) {
            for (size_t j = 0; j < n.layers.size(); ++j) viaInherit[n.first + j] = n.viaInherit;
        }
        for (size_t i = 0; i < prim.opinions.size(); ++i) {
            const Specifier s = prim.opinions[i].spec->specifier;
            if (s == Specifier::Over) continue;
            prim.specifier = s;
            if (s == Specifier::Def || !viaInherit[i]) break;
        }
        for (const Opinion& o : prim.opinions) {
            if (!o.spec->typeName.empty()) {
                prim.type = o.spec->typeName;
                break;
            }
        }
        for (const Opinion& o : prim.opinions) {
            if (const Value* a = o.spec->meta("active")) {
                prim.active = a->number(1.0) != 0.0;
                break;
            }
        }
        const bool parentDefined = !prim.parent || prim.parent == stage.prims_.front().get() || prim.parent->defined;
        prim.defined = parentDefined && prim.specifier == Specifier::Def && prim.active &&
                       !(prim.parent && prim.parent->specifier == Specifier::Class);
        // Value clips: its own sets, then those of the prims above it.
        for (const ClipSetSpec& s : clipSets(prim.opinions, placed)) {
            if (std::optional<Clips> c = makeClips(s, prim.path)) {
                place(*c, placed);
                prim.clips.push_back(std::move(*c));
            }
        }
        if (prim.parent) {
            for (Clips c : prim.parent->clips) {
                place(c, placed);
                prim.clips.push_back(std::move(c));
            }
        }
        if (!prim.active) return;
        // The children: every opinion's, the weakest first, as USD orders them.
        std::vector<std::string> names;
        for (auto it = prim.opinions.rbegin(); it != prim.opinions.rend(); ++it) {
            for (const auto& c : it->spec->children) {
                if (std::find(names.begin(), names.end(), c->name) == names.end()) names.push_back(c->name);
            }
        }
        for (const std::string& name : names) {
            auto child = std::make_unique<Prim>();
            child->name = name;
            child->path = childPath(prim.path, name);
            child->parent = &prim;
            Prim* c = child.get();
            stage.prims_.push_back(std::move(child));
            stage.byPath_[c->path] = c;
            prim.children.push_back(c);
            Node childIdx = childIndex(index, name, c->path, depth + 1);
            build(*c, childIdx, depth + 1);
        }
    }

    bool run(const std::string& path, std::string& error) {
        std::string why;
        const std::shared_ptr<const Layer> root = readLayer(path, why);
        if (!root) {
            error = why;
            return false;
        }
        layers[path] = root;
        stage.layers_.push_back(root);
        stage.rootLayer_ = root;
        rootStack = stackFor(path, "");
        auto pseudo = std::make_unique<Prim>();
        pseudo->path = "/";
        pseudo->specifier = Specifier::Def;
        pseudo->defined = true;
        Prim* p = pseudo.get();
        stage.prims_.push_back(std::move(pseudo));
        stage.byPath_["/"] = p;
        Node index;
        index.stack = rootStack;
        index.path = "/";
        index.source = "/";
        index.target = "/";
        stage.nodeMaps_.push_back({"/", "/"});
        for (const LayerStack::Entry& e : rootStack->layers) p->opinions.push_back({e.layer.get(), &e.layer->root, e.time, 0});
        std::vector<std::string> names;
        for (auto it = rootStack->layers.rbegin(); it != rootStack->layers.rend(); ++it) {
            for (const auto& c : it->layer->root.children) {
                if (std::find(names.begin(), names.end(), c->name) == names.end()) names.push_back(c->name);
            }
        }
        for (const std::string& name : names) {
            auto child = std::make_unique<Prim>();
            child->name = name;
            child->path = "/" + name;
            child->parent = p;
            Prim* c = child.get();
            stage.prims_.push_back(std::move(child));
            stage.byPath_[c->path] = c;
            p->children.push_back(c);
            Node childIdx = childIndex(index, name, c->path, 1);
            build(*c, childIdx, 1);
        }
        return true;
    }
};

std::shared_ptr<const Stage> Stage::open(const std::string& path, std::string& error) {
    std::shared_ptr<Stage> stage(new Stage());
    Build build(*stage);
    if (!build.run(path, error)) return nullptr;
    return stage;
}

std::shared_ptr<const Stage> Stage::openCached(const std::string& path, std::string& error) {
    struct Entry {
        std::shared_ptr<const Stage> stage;
        std::vector<std::pair<std::string, std::string>> stamps;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(path); it != cache.end()) {
            bool fresh = true;
            for (const auto& [file, stamp] : it->second.stamps) fresh = fresh && stampOf(file) == stamp;
            if (fresh) return it->second.stage;
            cache.erase(it);
        }
    }
    std::shared_ptr<const Stage> stage = open(path, error);
    if (!stage) return nullptr;
    Entry e;
    e.stage = stage;
    for (const std::string& f : stage->files()) e.stamps.emplace_back(f, stampOf(f));
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() > 16) cache.erase(cache.begin());
    cache[path] = std::move(e);
    return stage;
}

std::vector<std::string> Stage::files() const {
    std::vector<std::string> out;
    for (const auto& l : layers_) out.push_back(l->identifier);
    for (const auto& p : prims_) {
        for (const Clips& c : p->clips) {
            for (const std::string& l : c.layers) {
                if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
            }
            if (!c.manifest.empty() && std::find(out.begin(), out.end(), c.manifest) == out.end()) out.push_back(c.manifest);
        }
    }
    return out;
}

const Stage::Prim* Stage::find(std::string_view path) const {
    const auto it = byPath_.find(path);
    return it == byPath_.end() ? nullptr : it->second;
}

double Stage::metersPerUnit() const { return rootLayer_->number("metersPerUnit", 0.01); }
bool Stage::zUp() const {
    const std::string up = rootLayer_->text("upAxis");
    return up == "Z" || up == "z";
}
double Stage::startTimeCode() const { return rootLayer_->number("startTimeCode", 0.0); }
double Stage::endTimeCode() const { return rootLayer_->number("endTimeCode", 0.0); }
bool Stage::hasTimeRange() const { return rootLayer_->meta("startTimeCode") && rootLayer_->meta("endTimeCode"); }
double Stage::timeCodesPerSecond() const { return timeCodesPerSecondOf(*rootLayer_); }
std::string Stage::defaultPrim() const { return rootLayer_->text("defaultPrim"); }

// --- Properties -----------------------------------------------------------------------------

// --- Value clips ------------------------------------------------------------------------------

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

/// Sdf's samples either side of `t` among ascending `times`: the same one
/// twice at or outside the ends and on a sample.
bool bracket(const std::vector<double>& times, double t, double& lo, double& hi) {
    if (times.empty()) return false;
    if (t <= times.front()) {
        lo = hi = times.front();
    } else if (t >= times.back()) {
        lo = hi = times.back();
    } else {
        const auto it = std::lower_bound(times.begin(), times.end(), t);
        if (*it == t) {
            lo = hi = t;
        } else {
            hi = *it;
            lo = *(it - 1);
        }
    }
    return true;
}

/// A clip set asked about one attribute, answered as USD answers
/// (Usd_ClipSet, Usd_Clip): which clip is active when, how stage time maps
/// to its time, the samples either side of a time -- the neighbouring
/// clips' too -- and the value between them.
class ClipQuery {
public:
    using Mapping = Stage::Clips::Mapping;
    using Load = std::function<std::shared_ptr<const Layer>(const std::string&)>;

    ClipQuery(const Stage::Clips& c, std::string_view name, const Load& load) : c_(c), name_(name), load_(load) {
        if (!c.manifest.empty()) {
            if (const auto m = load_(c.manifest)) {
                if (const PrimSpec* s = m->prim(c.primPath)) manifest_ = s->property(name_);
            }
            if (manifest_ && manifest_->relationship) manifest_ = nullptr;
            declared_ = manifest_ && !manifest_->uniform;
            return;
        }
        // No manifest: USD makes one of the attributes the clips have samples of.
        for (size_t k = 0; k < c.layers.size(); ++k) {
            if (const Property* p = attribute(k); sampled(p)) {
                declared_ = !p->uniform;
                first_ = p;
                break;
            }
        }
    }

    /// Whether the set gives the attribute's values: its manifest declares it, varying.
    bool declared() const { return declared_ && !c_.layers.empty(); }
    /// Its spec: the manifest's, else the first clip's that has samples.
    const Property* spec() const { return manifest_ ? manifest_ : first_; }

    /// Its value at stage time `t`: interpolated between the samples either
    /// side, which may be in two clips; empty where blocked.
    Value value(double t) const {
        double lo = 0.0, hi = 0.0;
        if (!setBracket(t, lo, hi)) return {};
        Value v0;
        if (!setQuery(lo, v0) || v0.blocked()) return {};
        if (std::abs(lo - hi) < 1e-6) return v0;
        Value v1;
        if (!setQuery(hi, v1) || v1.blocked()) return v0;
        return interpolate(v0, v1, lo, hi, t);
    }

    /// Its time samples in stage time: every clip's, where it is active,
    /// with the times the mapping names and the clips start at.
    std::vector<double> times() const {
        std::set<double> out;
        for (size_t k = 0; k < c_.layers.size(); ++k) {
            if (contributes(k)) clipTimes(k, out);
        }
        if (out.empty() && !c_.starts.empty()) out.insert(c_.starts.front());
        return {out.begin(), out.end()};
    }

    /// As USD says it: more than one clip may vary; one varies with more than one sample.
    bool mightVary() const {
        if (c_.layers.size() != 1) return c_.layers.size() > 1;
        std::set<double> out;
        clipTimes(0, out);
        return out.size() > 1;
    }

private:
    const Stage::Clips& c_;
    std::string name_;
    const Load& load_;
    const Property* manifest_ = nullptr;
    const Property* first_ = nullptr;
    bool declared_ = false;

    double start(size_t k) const { return k == 0 ? -kInf : c_.starts[k]; }
    double end(size_t k) const { return k + 1 < c_.starts.size() ? c_.starts[k + 1] : kInf; }

    const Property* attribute(size_t k) const {
        const auto l = load_(c_.layers[k]);
        const PrimSpec* s = l ? l->prim(c_.primPath) : nullptr;
        const Property* p = s ? s->property(name_) : nullptr;
        return p && !p->relationship ? p : nullptr;
    }
    static bool sampled(const Property* p) { return p && p->hasSamples && !p->times.empty(); }

    /// The clip active at `t`: each from its start until the next one's.
    size_t active(double t) const {
        if (c_.starts.size() <= 1) return 0;
        const auto it = std::upper_bound(c_.starts.begin() + 1, c_.starts.end(), t);
        return static_cast<size_t>(it - c_.starts.begin()) - 1;
    }

    /// Whether clip `k` counts for the attribute: every clip does, unless
    /// missing values are interpolated -- then those with samples or a
    /// default in the manifest.
    bool contributes(size_t k) const {
        if (!c_.interpolateMissing) return true;
        if (sampled(attribute(k)) && !manifestBlocked(c_.starts[k])) return true;
        return manifest_ && manifest_->hasDefault;
    }
    bool manifestBlocked(double t) const {
        if (!manifest_ || !manifest_->hasSamples) return false;
        const auto& ts = manifest_->times;
        const auto it = std::lower_bound(ts.begin(), ts.end(), t);
        return it != ts.end() && *it == t && manifest_->samples[static_cast<size_t>(it - ts.begin())].blocked();
    }

    // --- Time ----------------------------------------------------------------------------------

    /// The two mapping entries around stage time `t`.
    bool segment(double t, size_t& i1, size_t& i2) const {
        const auto& m = c_.times;
        if (m.size() < 2) return false;
        if (t <= m.front().stage) {
            i1 = 0;
            i2 = 1;
        } else if (t >= m.back().stage) {
            i1 = m.size() - 2;
            i2 = m.size() - 1;
        } else {
            i2 = static_cast<size_t>(std::lower_bound(m.begin(), m.end(), t,
                                                      [](const Mapping& a, double x) { return a.stage < x; }) -
                                     m.begin());
            i1 = i2 - 1;
        }
        return true;
    }
    /// Entry `i2`, or where a jump that starts at it lands.
    Mapping upperOf(size_t i2) const {
        Mapping b = c_.times[i2];
        if (b.jump && i2 + 1 < c_.times.size()) b.stage = c_.times[i2 + 1].stage;
        return b;
    }
    double toClip(double t) const {
        size_t i1 = 0, i2 = 0;
        if (!segment(t, i1, i2)) return t;
        const Mapping a = c_.times[i1], b = upperOf(i2);
        if (a.stage == b.stage || t == a.stage) return a.clip;
        if (t == b.stage) return b.clip;
        return (b.clip - a.clip) / (b.stage - a.stage) * (t - a.stage) + a.clip;
    }
    double toStage(double x, size_t i1, size_t i2) const {
        const Mapping a = c_.times[i1], b = upperOf(i2);
        if (a.clip == b.clip || x == a.clip) return a.stage;
        if (x == b.clip) return b.stage;
        return (b.stage - a.stage) / (b.clip - a.clip) * (x - a.clip) + a.stage;
    }

    // --- Samples -------------------------------------------------------------------------------

    /// The clip's own samples either side of `t`, in stage time: mapped back
    /// through the segment nearest `t` that holds each.
    bool layerBracket(size_t k, double t, double& lo, double& hi) const {
        const Property* p = attribute(k);
        if (!sampled(p)) return false;
        double loIn = 0.0, hiIn = 0.0;
        bracket(p->times, toClip(t), loIn, hiIn);
        size_t m1 = 0, m2 = 0;
        if (!segment(t, m1, m2)) {
            lo = loIn;
            hi = hiIn;
            return true;
        }
        const auto& m = c_.times;
        std::optional<double> tl, tu;
        const auto translate = [&](size_t i1, size_t i2, bool lower) {
            const Mapping& a = m[i1];
            const Mapping& b = m[i2];
            if (a.jump) return false;
            const double x = lower ? loIn : hiIn;
            std::optional<double>& out = lower ? tl : tu;
            if (std::min(a.clip, b.clip) <= x && x <= std::max(a.clip, b.clip)) {
                if (a.clip != b.clip) out = toStage(x, i1, i2);
                else if (loIn == hiIn && t == a.stage) out = a.stage;
                else if (loIn == hiIn && t == b.stage) out = b.stage;
                else out = lower ? a.stage : b.stage;
            }
            return out.has_value();
        };
        for (size_t i1 = m1 + 1; i1-- > 0;) {
            if (translate(i1, i1 + 1, true)) break;
        }
        for (size_t i1 = m1; i1 + 1 < m.size(); ++i1) {
            if (translate(i1, i1 + 1, false)) break;
        }
        if (tl && !tu) {
            tu = tl;
        } else if (!tl && tu) {
            tl = tu;
        } else if (!tl && !tu) {
            // Outside what the mapping reaches: its nearest end.
            if (loIn < m.front().clip) tl = m.front().stage;
            else if (loIn > m.back().clip) tl = m.back().stage;
            if (hiIn < m.front().clip) tu = m.front().stage;
            else if (hiIn > m.back().clip) tu = m.back().stage;
        }
        // Where one does not map back USD reads an empty std::optional --
        // undefined; this reader keeps the other, and with neither, none.
        if (!tl && !tu) return false;
        lo = tl ? *tl : *tu;
        hi = tu ? *tu : *tl;
        return true;
    }

    /// Clip `k`'s samples either side of `t`: its own, the mapping's
    /// entries and its start -- each clip has one there -- where it is active.
    bool clipBracket(size_t k, double t, double& lo, double& hi) const {
        std::vector<double> times;
        double a = 0.0, b = 0.0;
        if (layerBracket(k, t, a, b)) times.insert(times.end(), {a, b});
        if (!c_.times.empty()) {
            std::vector<double> knots;
            knots.reserve(c_.times.size());
            for (const Mapping& m : c_.times) knots.push_back(m.stage);
            bracket(knots, t, a, b);
            times.insert(times.end(), {a, b});
        }
        times.push_back(c_.starts[k]);
        const double s = start(k), e = end(k);
        times.erase(std::remove_if(times.begin(), times.end(), [&](double x) { return x < s || x >= e; }), times.end());
        if (times.empty()) return false;
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        return bracket(times, t, lo, hi);
    }

    /// The set's samples either side of `t`: the active clip's, else the
    /// nearest ones of the clips before and after it.
    bool setBracket(double t, double& lo, double& hi) const {
        bool foundLower = false, foundUpper = false;
        const size_t k = active(t);
        if (contributes(k)) {
            if (!clipBracket(k, t, lo, hi)) return false;
            foundLower = true;
            foundUpper = !(lo == hi && t > hi);
        }
        for (size_t i = k; !foundLower && i-- > 0;) {
            if (!contributes(i)) continue;
            double a = 0.0, b = 0.0;
            if (!clipBracket(i, t, a, b)) return false;
            lo = b;
            foundLower = true;
        }
        for (size_t i = k + 1; !foundUpper && i < c_.starts.size(); ++i) {
            if (!contributes(i)) continue;
            hi = c_.starts[i];
            foundUpper = true;
        }
        if (foundLower && !foundUpper) hi = lo;
        else if (!foundLower && foundUpper) lo = hi;
        else if (!foundLower && !foundUpper) lo = hi = c_.starts.front();
        return true;
    }

    /// The value at `t` of the clip active then, interpolated between its
    /// samples; else the manifest's default. False: none.
    bool setQuery(double t, Value& out) const {
        if (clipQuery(active(t), t, out)) return true;
        if (manifest_ && manifest_->hasDefault && !manifest_->value.blocked()) {
            out = manifest_->value;
            return true;
        }
        return false;
    }
    bool clipQuery(size_t k, double t, Value& out) const {
        const Property* p = attribute(k);
        if (!sampled(p)) return false;
        const double x = toClip(t);
        const auto& ts = p->times;
        const auto at = [&](double time) -> const Value& {
            return p->samples[static_cast<size_t>(std::lower_bound(ts.begin(), ts.end(), time) - ts.begin())];
        };
        double lo = 0.0, hi = 0.0;
        bracket(ts, x, lo, hi);
        const Value& v0 = at(lo);
        if (lo == hi || std::abs(lo - hi) < 1e-6 || v0.blocked()) {
            out = v0;
            return true;
        }
        const Value& v1 = at(hi);
        out = v1.blocked() ? v0 : interpolate(v0, v1, lo, hi, x);
        return true;
    }

    /// Clip `k`'s time samples in stage time, where it is active.
    void clipTimes(size_t k, std::set<double>& out) const {
        const double s = start(k), e = end(k);
        const auto inside = [&](double x) { return x >= s && x < e; };
        const auto& m = c_.times;
        if (const Property* p = attribute(k); sampled(p)) {
            for (const double x : p->times) {
                if (m.empty()) {
                    if (inside(x)) out.insert(x);
                    continue;
                }
                // Every segment that holds it: the mapping may go back and forth.
                for (size_t i = 0; i + 1 < m.size(); ++i) {
                    const Mapping& a = m[i];
                    const Mapping& b = m[i + 1];
                    if (std::max(a.stage, b.stage) < s || std::min(a.stage, b.stage) >= e || a.jump) continue;
                    if (x < std::min(a.clip, b.clip) || x > std::max(a.clip, b.clip)) continue;
                    if (a.clip == b.clip) {
                        if (inside(a.stage)) out.insert(a.stage);
                        if (inside(b.stage)) out.insert(b.stage);
                    } else if (const double y = toStage(x, i, i + 1); inside(y)) {
                        out.insert(y);
                    }
                }
            }
        }
        for (const Mapping& a : m) {
            if (inside(a.stage)) out.insert(a.stage);
        }
        out.insert(c_.starts[k]);
    }
};

}  // namespace

const Property* Stage::property(const Prim& prim, std::string_view name) const {
    for (const Opinion& o : prim.opinions) {
        if (const Property* p = o.spec->property(name)) return p;
    }
    const ClipQuery::Load load = [this](const std::string& id) { return clipLayer(id); };
    for (const Clips& c : prim.clips) {
        if (c.at > prim.opinions.size()) continue;  // they do not reach this prim
        const ClipQuery q(c, name, load);
        if (q.declared()) return q.spec();
    }
    return nullptr;
}

std::shared_ptr<const Layer> Stage::clipLayer(const std::string& identifier) const {
    std::lock_guard<std::mutex> lock(clipMutex_);
    if (const auto it = clipLayers_.find(identifier); it != clipLayers_.end()) return it->second;
    std::string error;
    std::shared_ptr<const Layer> l = readLayer(identifier, error);
    clipLayers_[identifier] = l;
    return l;
}

Value Stage::value(const Prim& prim, std::string_view name, double time) const {
    const ClipQuery::Load load = [this](const std::string& id) { return clipLayer(id); };
    for (size_t i = 0; i <= prim.opinions.size(); ++i) {
        // Clips come right after the layer that named them.
        for (const Clips& c : prim.clips) {
            if (c.at != i) continue;
            const ClipQuery q(c, name, load);
            if (q.declared()) return q.value(time);
        }
        if (i == prim.opinions.size()) break;
        const Opinion& o = prim.opinions[i];
        const Property* p = o.spec->property(name);
        if (!p) continue;
        if (p->hasSamples && !p->times.empty()) return sampleAt(p->times, p->samples, o.time.toLayer(time));
        if (p->hasDefault) return p->value.blocked() ? Value{} : p->value;
    }
    return {};
}

bool Stage::varies(const Prim& prim, std::string_view name) const {
    const ClipQuery::Load load = [this](const std::string& id) { return clipLayer(id); };
    for (size_t i = 0; i <= prim.opinions.size(); ++i) {
        for (const Clips& c : prim.clips) {
            if (c.at != i) continue;
            const ClipQuery q(c, name, load);
            if (q.declared()) return q.mightVary();
        }
        if (i == prim.opinions.size()) break;
        const Property* p = prim.opinions[i].spec->property(name);
        if (!p) continue;
        if (p->hasSamples && !p->times.empty()) return p->times.size() > 1;
        if (p->hasDefault) return false;
    }
    return false;
}

std::vector<double> Stage::sampleTimes(const Prim& prim, std::string_view name) const {
    const ClipQuery::Load load = [this](const std::string& id) { return clipLayer(id); };
    for (size_t i = 0; i <= prim.opinions.size(); ++i) {
        for (const Clips& c : prim.clips) {
            if (c.at != i) continue;
            const ClipQuery q(c, name, load);
            if (q.declared()) return q.times();
        }
        if (i == prim.opinions.size()) break;
        const Opinion& o = prim.opinions[i];
        const Property* p = o.spec->property(name);
        if (!p) continue;
        if (p->hasSamples && !p->times.empty()) {
            std::vector<double> out;
            for (const double t : p->times) out.push_back(o.time.toStage(t));
            return out;
        }
        if (p->hasDefault) return {};
    }
    return {};
}

std::vector<std::string> Stage::propertyNames(const Prim& prim) const {
    std::vector<std::string> out;
    for (auto it = prim.opinions.rbegin(); it != prim.opinions.rend(); ++it) {
        for (const Property& p : it->spec->properties) {
            if (std::find(out.begin(), out.end(), p.name) == out.end()) out.push_back(p.name);
        }
    }
    return out;
}

const Value* Stage::metadata(const Prim& prim, std::string_view key) const {
    for (const Opinion& o : prim.opinions) {
        if (const Value* v = o.spec->meta(key)) return v;
    }
    return nullptr;
}

std::vector<std::string> Stage::targets(const Prim& prim, std::string_view name) const {
    struct Mapped {
        ListItem item;
        int node;
    };
    std::vector<Mapped> out;
    for (auto it = prim.opinions.rbegin(); it != prim.opinions.rend(); ++it) {
        const Property* p = it->spec->property(name);
        if (!p || p->targets.empty()) continue;
        std::vector<ListItem> items;
        for (const Mapped& m : out) items.push_back(m.item);
        p->targets.apply(items);
        std::vector<Mapped> next;
        for (const ListItem& i : items) {
            int node = it->node;
            for (const Mapped& m : out) {
                if (m.item == i && !namedBy(p->targets, i)) node = m.node;
            }
            next.push_back({i, node});
        }
        out = std::move(next);
    }
    std::vector<std::string> paths;
    for (const Mapped& m : out) {
        const NodeMap& map = nodeMaps_[static_cast<size_t>(m.node)];
        paths.push_back(mapPath(m.item.text, map.source, map.target));
    }
    return paths;
}

}  // namespace pg::usd
