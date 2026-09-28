#pragma once
//
// A USD stage: the layers a file brings in, composed into one tree of prims
// -- what usdview shows and a renderer reads -- without the library.
//
// Composed as USD's composition engine does it, for what shots and assets
// use: the root layer's sublayers (with their time offsets and scales);
// references and payloads -- to other files or within the layer stack, to
// a named prim or the default one, with layer offsets; variant sets with
// the selection the strongest opinion makes (the shot's over the asset's
// own); inherits and specializes; def, over and class; active. The
// strength of the opinions is USD's (LIVRPS): a layer stack's own layers
// strongest first, then what it inherits, its variants, its references,
// its payloads, what it specializes -- each of those in the same order
// within itself; arcs made on a prim stronger than those it gets from its
// ancestors; what is specialized anywhere in the index weaker than all the
// rest, as USD moves it. A class a prim inherits does not make it a class.
//
// An attribute's value at a time is the strongest opinion's: its samples in
// time -- interpolated as USD does, linearly for floating point values of
// the same length, slerp for quaternions, held for the rest -- or its
// default. Value clips -- a prim's values from other layers, one a frame --
// are read as USD reads them: right after the layer that names them, on the
// prim and the prims under it; a clip set's fields composed over the layers
// and arcs; templates; mappings of time that jump; the manifest, or one
// made of the clips; values interpolated across clips.
//
// Not composed: relocates, implied inherits across references, instancing
// as such (instanceable prims are expanded as ordinary ones), session
// layers, and payloads left unloaded -- every payload is loaded.
//
#include "pg/usd/Layer.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace pg::usd {

/// How time in a layer maps to the stage's: stage = offset + scale x layer.
struct TimeMap {
    double offset = 0.0, scale = 1.0;

    double toStage(double t) const { return offset + scale * t; }
    double toLayer(double t) const { return scale != 0.0 ? (t - offset) / scale : t; }
    /// Time through `inner` first, then through this.
    TimeMap after(const TimeMap& inner) const { return {offset + scale * inner.offset, scale * inner.scale}; }
};

class Stage {
public:
    /// One layer's spec of a prim, where it comes in the order of strength.
    struct Opinion {
        const Layer* layer = nullptr;
        const PrimSpec* spec = nullptr;
        TimeMap time;  ///< the layer's time to the stage's
        int node = 0;  ///< the arc it came through
    };

    /// Value clips -- a clip set of USD's: a prim's values from other
    /// layers, one a frame, as the prim sees them.
    struct Clips {
        /// Stage time to the clips' time, in order of stage time. Two
        /// entries at one stage time are a jump: the first is moved just
        /// before it. Empty: the clips' time is the stage's.
        struct Mapping {
            double stage = 0.0, clip = 0.0;
            bool jump = false;
        };
        std::string name;                 ///< the set's
        std::vector<std::string> layers;  ///< identifiers: clip k, active from starts[k]
        std::vector<double> starts;       ///< ascending
        std::string primPath;             ///< the prim in them that stands for this one
        std::vector<Mapping> times;
        std::string manifest;             ///< identifier; "" if none -- the clips' own attributes are then
        bool interpolateMissing = false;  ///< interpolateMissingClipValues
        size_t at = 0;                    ///< read just before opinion `at`: after the layer that named them
        /// Where they were named: the layer stack (its root layer), the
        /// prim and the layer in it; and the prim in the clips it names.
        std::string anchorStack, anchorPrim, clipPrim;
        size_t anchorLayer = 0;
    };

    struct Prim {
        std::string path, name, type;
        Specifier specifier = Specifier::Over;
        bool active = true;
        const Prim* parent = nullptr;
        std::vector<const Prim*> children;
        std::vector<Opinion> opinions;  ///< the strongest first
        std::vector<Clips> clips;
        /// Defined -- a def -- and not a class nor under one: what a
        /// renderer draws and USD's traversals go through.
        bool defined = false;

        bool isA(std::string_view t) const { return type == t; }
    };

    /// Opens the file at `path` and every layer it brings in. Null, with why,
    /// only when the file itself cannot be read; layers it names that cannot
    /// be read are warnings().
    static std::shared_ptr<const Stage> open(const std::string& path, std::string& error);
    /// The same, shared with earlier callers while none of the stage's files
    /// has changed.
    static std::shared_ptr<const Stage> openCached(const std::string& path, std::string& error);

    const Prim& root() const { return *prims_.front(); }
    const Prim* find(std::string_view path) const;
    /// Every prim, parents before their children, in the order of the tree.
    const std::vector<std::unique_ptr<Prim>>& prims() const { return prims_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    /// The files the stage was composed from: its layers and their clips'.
    std::vector<std::string> files() const;

    // The root layer's metadata.
    const Layer& rootLayer() const { return *rootLayer_; }
    double metersPerUnit() const;     ///< 0.01 when not said, as USD has it
    bool zUp() const;                 ///< upAxis Z; else Y
    double startTimeCode() const;
    double endTimeCode() const;
    bool hasTimeRange() const;
    double timeCodesPerSecond() const;
    std::string defaultPrim() const;

    // --- Properties ------------------------------------------------------------------------

    /// The strongest spec of property `name`: its declared type and
    /// metadata. Null if no opinion names it.
    const Property* property(const Prim& prim, std::string_view name) const;
    /// Its value at `time`; empty when it has none there (a block, no
    /// opinion, a clip without a sample).
    Value value(const Prim& prim, std::string_view name, double time) const;
    /// True when its value may change with time: its opinion has more than
    /// one sample, or it comes from clips.
    bool varies(const Prim& prim, std::string_view name) const;
    /// The stage times it has samples at, in order; empty for a default.
    std::vector<double> sampleTimes(const Prim& prim, std::string_view name) const;
    /// The names of its properties, every opinion's, in order.
    std::vector<std::string> propertyNames(const Prim& prim) const;
    /// The strongest opinion of a prim's metadata `key` ("kind"...); null if none.
    const Value* metadata(const Prim& prim, std::string_view key) const;
    /// A relationship's targets as the stage has them, its list edits
    /// composed, paths from referenced assets mapped into the stage.
    std::vector<std::string> targets(const Prim& prim, std::string_view name) const;

private:
    Stage() = default;
    struct Build;
    friend struct Build;

    /// A clip layer, read the first time it is needed.
    std::shared_ptr<const Layer> clipLayer(const std::string& identifier) const;

    struct NodeMap {
        std::string source, target;  ///< paths under source (in the arc's layers) are under target in the stage
    };

    std::vector<std::unique_ptr<Prim>> prims_;
    std::map<std::string, const Prim*, std::less<>> byPath_;
    std::vector<NodeMap> nodeMaps_;
    std::vector<std::shared_ptr<const Layer>> layers_;
    std::shared_ptr<const Layer> rootLayer_;
    std::vector<std::string> warnings_;
    mutable std::mutex clipMutex_;
    mutable std::map<std::string, std::shared_ptr<const Layer>> clipLayers_;
};

/// A value of `v` at a time between samples `a` (at ta) and `b` (at tb):
/// linear for floating point values of the same length, slerp for
/// quaternions, else `a`.
Value interpolate(const Value& a, const Value& b, double ta, double tb, double t);

}  // namespace pg::usd
