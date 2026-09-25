#pragma once
//
// Shape grammar core: shapes, their scopes, and the operations that rewrite
// them. docs/shape-grammar.md explains the ideas; this header is the vocabulary.
//
// A shape grammar builds a model by repeatedly replacing a named shape -- a
// *symbol* such as Lot, Facade or Window -- with more detailed *successor*
// shapes. Every shape here is a *scope*: an oriented box given by an origin
// (a corner), three orthonormal axes and a size along each of them. Flat shapes
// have zero size along their normal: y for a footprint lying on the ground, z
// for a facade. Working in the shape's own frame is what lets a single `Floor`
// rule refine every floor of every facade, whichever way that facade faces.
//
// Invariant I3 decides the representation: a shape set is an ordinary Geometry
// whose POINTS are the shapes, described by point attributes
//
//     P      vec3    scope origin
//     xaxis  vec3    the three axes: orthonormal and right-handed,
//     yaxis  vec3    so cross(xaxis, yaxis) == zaxis
//     zaxis  vec3
//     size   vec3    extent along each axis, never negative
//     shape  string  the symbol
//     path   string  every symbol from the axiom down, e.g. "Lot/Mass/Facade"
//     asset  string  what fills a volume when it is meshed: "", recess, hip, gable
//
// so the existing nodes -- pointwrangle, merge, blast -- work on shapes as they
// are, and any attribute a user puts on a shape is inherited by every one of
// its successors without a line of code.
//
#include "pg/core/Geometry.h"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pg::grammar {

inline constexpr const char* kAttrX = "xaxis";
inline constexpr const char* kAttrY = "yaxis";
inline constexpr const char* kAttrZ = "zaxis";
inline constexpr const char* kAttrSize = "size";
inline constexpr const char* kAttrShape = "shape";
inline constexpr const char* kAttrPath = "path";
inline constexpr const char* kAttrAsset = "asset";

/// The symbol that deletes a shape instead of naming it.
inline constexpr const char* kNil = "NIL";

/// Sizes at or below this count as zero.
inline constexpr float kEpsilon = 1e-5f;

/// What fills a volume when it is meshed. Flat shapes are always one quad.
enum class Asset : uint8_t {
    Box,     ///< closed box, the default
    Recess,  ///< a hole: open towards +z, faces turned inwards
    Hip,     ///< hip roof: four slopes
    Gable,   ///< gable roof: two slopes and two triangular gables
};

const char* assetName(Asset a);
Asset assetFromName(const std::string& name);

struct Scope {
    Vec3 origin;
    Vec3 x{1, 0, 0};
    Vec3 y{0, 1, 0};
    Vec3 z{0, 0, 1};
    Vec3 size;

    /// World position of the local coordinates (u, v, w).
    Vec3 at(float u, float v, float w) const { return origin + x * u + y * v + z * w; }
    const Vec3& axis(int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

/// A shape as the operations see it. The symbol is handled by the rewriting
/// machinery, not by the operations.
struct Shape {
    Scope scope;
    Asset asset = Asset::Box;
};

/// The axis a flat shape faces along -- the one with zero size, tested in the
/// order y, z, x -- or -1 for a volume.
int flatAxis(const Scope& s);

// --- operations ------------------------------------------------------------
//
// Pure functions of a shape. The nodes and the text grammar only decide which
// shapes they run on and what the successors are called.

/// The six faces of a volume.
enum class Face : uint8_t { Front, Right, Back, Left, Top, Bottom };
inline constexpr int kFaceCount = 6;

const char* faceName(Face f);

/// One face of a volume as a flat scope: x runs left to right as seen from
/// outside, y runs up (for the four side faces, the volume's own y), and z is
/// the outward normal. The front face is the one at the far end of the
/// volume's z axis, so for a world-aligned box it faces +Z.
Scope faceScope(const Scope& s, Face f);

/// A flat shape re-expressed so that its normal is its z axis, as if it were a
/// face. A facade is unchanged; a footprint becomes its own top face.
Scope faceFrame(const Scope& s);

/// Grows a flat shape into a volume along its normal (y for a footprint, z for
/// a facade). A negative distance digs a recess behind the shape instead: a
/// hole, meshed open towards the original shape. On a volume, sets its height.
Shape extrude(const Shape& s, float distance);

enum class RoofType : uint8_t { Hip, Gable };

/// Puts a roof over a flat shape (over the top face of a volume). The slopes
/// rise at `angleDegrees`; the ridge runs along the longer side.
Shape roof(const Shape& s, RoofType type, float angleDegrees);

// --- split -----------------------------------------------------------------

enum class SizeMode : uint8_t {
    Absolute,  ///< `4`    exactly 4 units
    Relative,  ///< `'0.5` half of the scope
    Floating,  ///< `~3`   about 3 units: floating parts share what is left over
};

struct SplitPart {
    SizeMode mode = SizeMode::Absolute;
    float value = 0.0f;
    std::string symbol;  ///< successor symbol; kNil keeps the space empty
};

/// `{ head | { repeat }* | tail }`. The repeat group is tiled as many times as
/// fits; without one, `repeat` is empty.
struct SplitPattern {
    std::vector<SplitPart> head;
    std::vector<SplitPart> repeat;
    std::vector<SplitPart> tail;
};

struct SplitPiece {
    float start = 0.0f;
    float length = 0.0f;
    const SplitPart* part = nullptr;  ///< points into the pattern
};

/// Upper bound on repetitions, so a tiny repeat size cannot exhaust memory.
inline constexpr int kMaxRepeats = 10000;

/// Lays a pattern out along `length`, in order. Pieces that end up empty are
/// omitted; pieces that do not fit are clipped at the end of the scope.
/// NIL pieces are included -- they take up space even though no shape is made.
std::vector<SplitPiece> layoutSplit(const SplitPattern& pattern, float length);
/// The pieces point into the pattern, so a temporary one would leave them dangling.
std::vector<SplitPiece> layoutSplit(const SplitPattern&& pattern, float length) = delete;

// --- rewriting -------------------------------------------------------------

/// One shape of the next shape set.
struct Successor {
    uint32_t parent = 0;                  ///< the shape it replaces; attributes come from it
    Shape shape;
    const std::string* symbol = nullptr;  ///< new symbol, or nullptr to keep the parent's
    bool derived = false;                 ///< made by a rule, rather than passed through
};

/// Read-only view of a shape set. Missing attributes read as defaults, so any
/// point cloud is a valid -- if degenerate -- shape set.
class ShapeView {
public:
    explicit ShapeView(const Geometry& geo);

    size_t size() const { return count_; }
    Shape shape(size_t i) const;
    const Geometry& geometry() const { return *geo_; }

    /// Index of shape i's symbol in the symbol table, or -1 if it has none.
    int32_t symbolId(size_t i) const;
    size_t symbolCount() const;
    const std::string& symbolName(int32_t id) const;
    const std::string& symbol(size_t i) const { return symbolName(symbolId(i)); }

    /// One flag per shape: does its symbol equal `name`? An empty name selects
    /// every shape.
    std::vector<uint8_t> select(const std::string& name) const;

private:
    const Geometry* geo_;
    size_t count_ = 0;
    std::span<const Vec3> origin_, x_, y_, z_, size_;
    const AttributeArray* symbols_ = nullptr;
    std::span<const int32_t> symbolIds_;
    std::span<const int32_t> assetIds_;
    std::vector<Asset> assetById_;
};

/// Returns true and appends successors when it rewrites `shape`; returns false
/// to let the shape pass through unchanged.
using RewriteFn = std::function<bool(size_t shape, std::vector<Successor>& out)>;

/// One rewriting pass over a shape set. Each shape either passes through or is
/// replaced, in place, by its successors -- so the order of the result is the
/// left-to-right order of the derivation tree, however many passes built it.
///
/// Shapes are processed in chunks sized by the shape count, never by the thread
/// count, and the per-chunk results are joined in chunk order. The output is
/// therefore identical on any number of threads (I5). `rewrite` must only read.
std::vector<Successor> rewrite(const ShapeView& shapes, const RewriteFn& fn);

/// Builds the next shape set. Every successor starts as a copy of its parent's
/// point -- that is the attribute inheritance -- and then gets its own scope,
/// asset, and (when renamed) symbol and path.
GeometryPtr materialize(const Geometry& in, const std::vector<Successor>& successors);

/// The split operation: one successor per non-NIL piece. A shape with no
/// extent along `axis` has nothing to split and passes through unchanged.
void emitSplit(uint32_t parent, const Shape& s, int axis, const SplitPattern& pattern,
               std::vector<Successor>& out);

/// Successor symbol per face for the component split. A specific face wins
/// over `side`, which covers the four vertical faces. Empty (or NIL) drops the
/// face.
struct CompTargets {
    std::string face[kFaceCount];
    std::string side;

    const std::string* target(Face f) const;
};

/// The component split: the faces of a volume as flat shapes, in the order
/// front, right, back, left, top, bottom. Faces without area are skipped, so a
/// footprint yields only its top and bottom.
void emitComp(uint32_t parent, const Shape& s, const CompTargets& targets,
              std::vector<Successor>& out);

// --- entering and leaving the shape world -----------------------------------

/// Scope of a planar polygon, lying flat like a building lot: x along its first
/// edge, y along its normal turned to face up, z = cross(x, y), sized to the
/// polygon's bounding rectangle in that frame. False for a degenerate polygon.
bool fitPolygon(std::span<const Vec3> points, Scope& out);

/// Turns shapes into polygons: a quad per flat shape, and a box, recess or roof
/// per volume, all facing outwards. Every polygon inherits the attributes of
/// its shape (except the scope itself) as primitive attributes -- `shape` and
/// `path` among them, so the mesh remembers where each polygon came from.
GeometryPtr mesh(const Geometry& shapes);

}  // namespace pg::grammar
