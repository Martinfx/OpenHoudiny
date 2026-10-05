#pragma once
//
// OpenColorIO configs, read without the library: the config.ocio a studio
// or Blender keeps its colours in -- its colour spaces, displays and views,
// roles and looks -- and light through the transforms it names, as
// OpenColorIO's processors do on the CPU:
//
//   matrices; exponents, with a straight line near black or not; logs,
//   affine and of cameras; ASC CDLs; ranges; allocations; look-up tables
//   in files (.spi1d, .spi3d, .spimtx, .cube); colour space, look and
//   display-view transforms; groups of them; and the built-in transforms
//   of OpenColorIO 2's ACES configs -- the ACES 1.x and 2.0 outputs for SDR
//   screens (Aces.h), the displays' encodings (sRGB, Rec. 1886, Display P3,
//   gamma 2.2 and 2.6, P3 of DCI, D60 and D65, Rec. 2020), ACEScc and
//   ACEScct, the matrices between ACES's spaces, the reference gamut
//   compression.
//
// A view is a scene's colour space to a display's: in a config of version
// 1 a colour space; in one of version 2 a view transform and a display
// colour space, a scene's space and a display's bridged by the default
// view transform. Looks go in their process spaces before it. A data space
// -- Raw, Non-Color -- passes the light as it is.
//
// Each transform goes forward or inverse; back, each step by the
// transform the config has for that way, else by its inverse -- a 1D
// table's as OpenColorIO finds it, a 3D table's tabled from its exact
// inverse as OpenColorIO's processors do by default. What it cannot do --
// grading transforms, HDR displays and outputs, cameras' built-in curves
// -- it says by name.
//
#include "pg/core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pg::render {

/// Light through a chain of colour transforms: what OpenColorIO calls a
/// processor. Copies share their steps.
class OcioProcessor {
public:
    struct Op;

    Vec3 apply(const Vec3& rgb) const;
    /// The chain backwards; false, with the step that has no inverse, where
    /// one has none.
    bool inverse(OcioProcessor& out, std::string& error) const;
    bool empty() const { return ops_.empty(); }
    size_t size() const { return ops_.size(); }

    /// A step after those there are: two matrices one after the other made one.
    void add(std::shared_ptr<const Op> op);
    void append(const OcioProcessor& other);

private:
    std::vector<std::shared_ptr<const Op>> ops_;
};

class OcioConfig {
public:
    struct Data;

    /// The config in the file; null, with why, where it cannot be read.
    static std::shared_ptr<const OcioConfig> load(const std::string& path, std::string& error);
    /// A config's text; its tables' files are looked for from `folder`.
    static std::shared_ptr<const OcioConfig> parse(std::string_view text, const std::string& folder, std::string& error);
    OcioConfig();
    ~OcioConfig();

    /// Its colour spaces, scenes' and displays', but those it lists as
    /// inactive (still there by name).
    std::vector<std::string> colorSpaces() const;
    /// The displays: the active ones as the config lists them, else all.
    std::vector<std::string> displays() const;
    std::vector<std::string> views(const std::string& display) const;
    /// The display `name` is (any case) as the config writes it; "".
    std::string display(const std::string& name) const;
    std::string defaultDisplay() const;
    std::string defaultView(const std::string& display) const;
    /// The colour space `name` is -- its name, an alias, a role; any case --
    /// as the config writes it; "" for none.
    std::string colorSpace(const std::string& name) const;
    /// The colour space the config has for linear Rec. 709 light, by the
    /// names configs give it -- "Linear Rec.709 (sRGB)", lin_rec709 --; "".
    std::string linearRec709() const;
    /// The colour space a role names; "".
    std::string role(const std::string& name) const;

    /// Light from one colour space into another.
    bool processor(const std::string& from, const std::string& to, OcioProcessor& out, std::string& error) const;
    /// Light from a colour space as `view` of `display` shows it, with its
    /// looks -- or `looks` ("A, -B") in their place, as OpenColorIO's viewing
    /// pipeline and Blender put them. `inverse`: what it shows
    /// back to the light, each step by the transform the config has for that
    /// way (a colour space's to_scene_reference, say), else its inverse.
    bool viewProcessor(const std::string& from, const std::string& display, const std::string& view,
                       const std::string& looks, OcioProcessor& out, std::string& error, bool inverse = false) const;

private:
    std::unique_ptr<Data> d_;
};

/// A display's view of a config, as the render shows its light through it:
/// linear Rec. 709 light -- what the renderers make -- taken into the
/// config as its own colour space for it, or through ACES2065-1 (its
/// aces_interchange role), or as its scene_linear role; then the view.
class OcioView {
public:
    /// `view` of `display` -- "" each for the config's defaults --, `looks`
    /// in place of the view's own, the light taken as colour space `space`
    /// ("": as above). The same arguments and file: the one made before.
    /// Null, with why.
    static std::shared_ptr<const OcioView> make(const std::string& config, const std::string& display,
                                                const std::string& view, const std::string& looks,
                                                const std::string& space, std::string& error);

    /// Linear Rec. 709 light as the view shows it: 0 to 1, as it goes to
    /// the screen.
    Vec3 shown(const Vec3& linear) const;
    /// What `shown` shows as `display`, back: as the config takes the view
    /// back -- else the picture taken as sRGB --, then made exact by
    /// Newton's method where it can be.
    Vec3 unshown(const Vec3& display) const;
    bool invertible() const { return invertible_; }
    const std::string& display() const { return display_; }
    const std::string& view() const { return view_; }
    /// The colour space the light went in as.
    const std::string& space() const { return space_; }

private:
    OcioProcessor forward_, back_;
    bool invertible_ = false;
    std::string display_, view_, space_;
};

}  // namespace pg::render
