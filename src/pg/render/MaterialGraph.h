#pragma once
//
// The renderers' materials (Scene.h) out to the other programs, as MaterialX
// graphs (io/MaterialX.h) -- a standard_surface of its colour, how rough and
// how metal it is, glass and water clear and bending light -- over the
// pictures both renderers lay on it (Textures.h):
//
//   by uv           image, a picture a unit of uv (the primvar st)
//   from 3 sides    triplanarprojection of where the surface was before it
//                   moved (the primvar rest, else its position), so many
//                   metres a picture
//   tinted          the picture over its mean, times the colour: Cd -- the
//                   primvar displayColor (geompropvalue) -- else its
//                   material's
//   normal map      normalmap, as strongly as the material says; DirectX's
//                   green turned over
//   alpha           opacity, nothing where it has none: a leaf's edge
//   translucent     thin_walled, its subsurface letting light through, as
//                   much as translucency says
//
// -- and, for what reads no MaterialX, a UsdPreviewSurface of the same: its
// pictures by st, its colour, roughness, metalness, opacity and normal map.
//
#include "pg/core/Geometry.h"
#include "pg/io/MaterialX.h"
#include "pg/io/Usda.h"
#include "pg/render/Scene.h"

#include <functional>
#include <string>
#include <vector>

namespace pg::render {

/// What a material is called: its preset's name (bark), glass, water, its
/// own texture's (bricks_02), else plain.
std::string lookName(const Material& m);

/// What else a material's graph is built of.
struct GraphSource {
    /// The surfaces have a colour of their own (Cd): the primvar
    /// displayColor. Else each is its material's.
    bool colored = false;
    /// The points have the primvar rest: the pictures laid from three sides
    /// by it. Else by where they are.
    bool rest = false;
};

/// A material's graph: as MaterialX has it, and as a UsdPreviewSurface.
struct MaterialGraph {
    io::mtlx::Material mtlx;               ///< its nodes, the last its surfacematerial
    std::vector<io::mtlx::Node> preview;   ///< UsdPreviewSurface and what feeds it
};

/// `m` as a graph named `name` -- its nodes `name`_surface, `name`_picture...
/// -- its pictures named in it as `picture` says of their files.
MaterialGraph materialGraph(const Material& m, const std::string& name, const GraphSource& source,
                            const std::function<std::string(const std::string& file)>& picture);

/// The materials of geometry as a stage binds them, each once: what the
/// renderers tell apart (primitiveMaterials) -- and whether its geometry
/// colours it -- named as it is called (bark, bark_2...), a Material each at
/// `scope` on the stage; their pictures copied beside it, into a folder of
/// their own (`pictures`: "./shot_textures/"), named by their sets' and
/// their own (bark_color.jpg).
class MaterialLooks {
public:
    MaterialLooks(std::string scope, std::string pictures);

    /// The materials of `geo`'s closed primitives, the new ones taken in.
    io::usda::FaceMaterials bind(const Geometry& geo);
    /// ... of those taken in already; a primitive of another unbound.
    io::usda::FaceMaterials bound(const Geometry& geo) const;

    bool empty() const { return looks_.empty(); }
    size_t size() const { return looks_.size(); }
    const std::string& scope() const { return scope_; }
    /// Each a Material, under the Scope at `scope`.
    void addTo(io::usda::Prim& scope) const;
    /// All as a MaterialX document (.mtlx).
    std::string document() const;
    /// The pictures they show: where each is, and its name beside the stage.
    struct Picture {
        std::string from, name;
    };
    const std::vector<Picture>& pictures() const { return pictures_; }
    /// ... copied to `folder`. False, with why, when one cannot be.
    bool copyPictures(const std::string& folder, std::string& error) const;

private:
    struct Look {
        Material material;
        GraphSource source;
        std::string name;
        MaterialGraph graph;
    };
    /// What a geometry's primitives are made of.
    struct Found {
        std::vector<Material> materials;
        std::vector<uint16_t> ofPrim;
        GraphSource source;
    };
    void materialsOf(const Geometry& geo, Found& found) const;
    const Look* lookOf(const Material& m, const GraphSource& source) const;
    io::usda::FaceMaterials facesOf(const Found& found) const;
    std::string pictureName(const std::string& file);

    std::string scope_, prefix_;
    std::vector<Look> looks_;
    std::vector<Picture> pictures_;
};

}  // namespace pg::render
