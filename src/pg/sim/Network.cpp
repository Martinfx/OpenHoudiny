#include "pg/sim/Network.h"

#include "pg/sim/Asset.h"
#include "pg/sim/GeometryGraph.h"

#include "pg/lang/Lang.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <locale>
#include <mutex>
#include <set>
#include <sstream>

namespace pg::sim {

// The examples in examples/sim, compiled in by CMake (generated/SimExamples.cpp).
struct EmbeddedExample {
    const char* name;
    const char* text;
};
const std::vector<EmbeddedExample>& embeddedExamples();

namespace {

constexpr float kBig = 1e6f;

using K = ParamKind;

// --- the parameters several node types share -----------------------------------------

ParamDef mask(int byDefault) {
    return {"mask",
            "Acts On",
            "Force",
            K::Choice,
            {static_cast<float>(byDefault), 0.0f, 0.0f},
            0.0f,
            2.0f,
            0.0f,
            2.0f,
            "",
            "Where it acts: everywhere, only where the gas is hot (or has fuel), or only where there is smoke.",
            {"everywhere", "heat", "smoke"},
            {"Everywhere", "Heat", "Smoke"}};
}

ParamDef seed(const char* section, const char* help) {
    return {"seed", "Seed", section, K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "", help};
}

ParamDef center(const char* section, Vec3 at, const char* help) {
    return {"center", "Center", section, K::Vector, {at.x, at.y, at.z}, -1.0f, 1.0f, -kBig, kBig, "m", help};
}

// --- shapes placed in the world: objects and sources (Shape.h) ----------------------

ParamDef shape(const char* help, Shape byDefault = Shape::Sphere) {
    return {"shape",
            "Shape",
            "Shape",
            K::Choice,
            {static_cast<float>(byDefault), 0.0f, 0.0f},
            0.0f,
            5.0f,
            0.0f,
            5.0f,
            "",
            help,
            {"sphere", "box", "cylinder", "cone", "torus", "mesh"},
            {"Sphere", "Box", "Cylinder", "Cone", "Torus", "Mesh"}};
}

ParamDef file(const char* help) {
    return {"file", "File", "Shape", K::File, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "", help, {".obj"}, {}};
}

ParamDef usdFile(const char* help) {
    return {"file", "File", "File", K::File, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "", help,
            {".usd", ".usda", ".usdc", ".usdz"}, {}};
}

ParamDef plateFile(const char* help) {
    return {"plate", "Plate", "Plate", K::File, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "", help,
            {".exr", ".png", ".jpg", ".jpeg"}, {}};
}

ParamDef plateFrame(float byDefault, const char* help) {
    return {"plate_frame", "Plate Frame", "Plate", K::Int, {byDefault, 0.0f, 0.0f}, 0.0f, 1100.0f, -1e6f, 1e6f, "", help};
}

ParamDef frameOffset() {
    return {"offset", "Frame Offset", "Time", K::Float, {0.0f, 0.0f, 0.0f}, -100.0f, 100.0f, -1e6f, 1e6f, "",
            "Frame 1 reads the stage's first time code (its startTimeCode, 1 when it says none), each frame after "
            "it as far on as the stage's time codes per second and the Output's frame rate make it. This moves "
            "it by as many frames."};
}

ParamDef position(Vec3 at, const char* help) {
    return {"center", "Position", "Transform", K::Vector, {at.x, at.y, at.z}, -1.0f, 1.0f, -kBig, kBig, "m", help};
}

ParamDef rotation() {
    return {"rotation", "Rotation", "Transform", K::Vector, {0.0f, 0.0f, 0.0f}, -180.0f, 180.0f, -kBig, kBig,
            "\xc2\xb0", "Degrees about x, then y, then z: how it is turned. The W / E / R gizmo in the viewport "
            "moves, turns and sizes it."};
}

ParamDef size(Vec3 extent, const char* help) {
    return {"size", "Size", "Transform", K::Vector, {extent.x, extent.y, extent.z}, 0.01f, 2.0f, 0.005f, kBig, "m", help};
}

std::vector<ParamDef> pyroSourceParams() {
    std::vector<ParamDef> p;
    p.push_back(shape("Its shape. Gas comes out of all of it, most away from its surface."));
    p.push_back(file("An OBJ file, when the shape is mesh: a burning car, a smoking chimney. A relative path is "
                     "read from the network's folder."));
    p.push_back(position(Vec3(0.0f, 0.12f, 0.0f), "Where the source is; y is its height above the floor."));
    p.push_back(rotation());
    p.push_back(size(Vec3(0.2f, 0.2f, 0.2f),
                     "Width, height and depth, along its own axes: a ball's diameter, a box's edges."));
    p.push_back({"fuel", "Fuel", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 40.0f, 0.0f, kBig, "1/s",
                 "Fuel added each second. Fuel burns -- into flame, heat and soot, and the gas swells: fire."});
    p.push_back({"smoke", "Smoke", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                 "Smoke added each second."});
    p.push_back({"heat", "Heat", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                 "Heat added each second. Hot gas rises, and cools as it goes."});
    p.push_back({"velocity", "Velocity", "Emission", K::Vector, {0.0f, 0.5f, 0.0f}, -2.0f, 2.0f, -kBig, kBig, "m/s",
                 "The gas leaves the source at least this fast, along the source's own axes: turn the source "
                 "and the jet turns with it."});
    p.push_back({"expansion", "Expansion", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "1/s",
                 "How fast the gas in the source swells: it is pushed out on all sides -- a blast, or the air a "
                 "collapse squeezes out. 0: it does not."});
    p.push_back({"flicker", "Flicker", "Noise", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                 "How much the output flickers: 0 steady, 1 strongly -- what makes flames lick."});
    p.push_back({"flicker_size", "Flicker Size", "Noise", K::Float, {0.07f, 0.0f, 0.0f}, 0.01f, 0.3f, 0.005f, kBig,
                 "m", "Size of the patches that flicker together."});
    p.push_back(seed("Noise", "Another number, another flicker."));
    p.push_back({"start", "Start", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When it starts."});
    p.push_back({"end", "End", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When it stops. At or before the start, it never does."});
    p.push_back({"motion", "Motion", "Motion", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                 "How it moves: it stays, goes round in a circle about its position, or sways from side to side.",
                 {"static", "circle", "sway"}, {"Static", "Circle", "Sway"}});
    p.push_back({"motion_size", "Reach", "Motion", K::Float, {0.25f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, kBig, "m",
                 "Radius of the circle; how far it sways."});
    p.push_back({"motion_period", "Period", "Motion", K::Float, {4.0f, 0.0f, 0.0f}, 0.2f, 10.0f, 0.05f, kBig, "s",
                 "Seconds for one round."});
    return p;
}

std::vector<ParamDef> waterSourceParams() {
    std::vector<ParamDef> p;
    p.push_back(shape("Its shape: the water fills it, or pours out of it.", Shape::Box));
    p.push_back(file("An OBJ file, when the shape is mesh: water in the shape of a model. A relative path is read "
                     "from the network's folder."));
    p.push_back(position(Vec3(0.0f, 0.3f, 0.0f), "Where the water is; y is its height above the floor."));
    p.push_back(rotation());
    p.push_back(size(Vec3(0.4f, 0.6f, 0.4f), "Width, height and depth, along its own axes."));
    p.push_back({"mode", "Mode", "Emission", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                 "Fill: the shape is filled with water once, when it starts -- a block of water let go, a pool. "
                 "Flow: water keeps coming out of it at its velocity -- a hose, a fountain, a waterfall.",
                 {"fill", "flow"}, {"Fill", "Flow"}});
    p.push_back({"velocity", "Velocity", "Emission", K::Vector, {0.0f, 0.0f, 0.0f}, -5.0f, 5.0f, -kBig, kBig, "m/s",
                 "How fast the water leaves, along the source's own axes: turn the source and the jet turns "
                 "with it."});
    p.push_back(seed("Emission", "Another number: the drops of water at other places."));
    p.push_back({"start", "Start", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When it fills, or starts to flow."});
    p.push_back({"end", "End", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When a flow stops. At or before the start, it never does."});
    return p;
}

std::vector<ParamDef> objectParams() {
    return {shape("Its shape: a ball, a box, a column, a cone, a ring -- or a mesh from an OBJ file."),
            file("An OBJ file, when the shape is mesh: a rock, a statue, a car. Its box is stretched onto the "
                 "size. A relative path is read from the network's folder."),
            position(Vec3(0.0f, 0.15f, 0.0f), "Where it is: its middle. y is its height above the floor."),
            rotation(),
            size(Vec3(0.3f, 0.3f, 0.3f),
                 "Width, height and depth, along its own axes: a ball's diameter, a box's edges, a ring's width "
                 "and thickness."),
            {"color", "Color", "Look", K::Color, {0.45f, 0.45f, 0.46f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "Its colour. Painting it simulates nothing again."},
            {"matte", "Over the Plate", "Look", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
             "What it is when the camera has a plate. Solid: itself, drawn over the plate. Holdout: the real "
             "thing the plate shows -- a wall, the ground -- it hides the CG behind it and the plate shows there. "
             "Shadow Catcher: a holdout that the CG relights -- darker where it takes the sun away, brighter "
             "where the fire lights it. With no plate, it is drawn as itself.",
             {"solid", "holdout", "catcher"},
             {"Solid", "Holdout", "Shadow Catcher"}}};
}

// --- the nodes of earlier versions, as their files hold them (Legacy below) ------

std::vector<ParamDef> legacySourceParams(bool sphere) {
    std::vector<ParamDef> p;
    p.push_back(center("Shape", Vec3(0.0f, 0.12f, 0.0f), "Where the source is; y is its height above the floor."));
    if (sphere) {
        p.push_back({"radius", "Radius", "Shape", K::Float, {0.1f, 0.0f, 0.0f}, 0.01f, 0.5f, 0.005f, kBig, "m",
                     "How big it is. Gas comes out of it all, most at the middle."});
    } else {
        p.push_back({"size", "Size", "Shape", K::Vector, {0.2f, 0.1f, 0.2f}, 0.01f, 1.0f, 0.005f, kBig, "m",
                     "Width, height and depth. Gas comes out of it all, most away from its sides."});
    }
    p.push_back({"fuel", "Fuel", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 40.0f, 0.0f, kBig, "1/s",
                 "Fuel added each second. Fuel burns -- into flame, heat and soot, and the gas swells: fire."});
    p.push_back({"smoke", "Smoke", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                 "Smoke added each second."});
    p.push_back({"heat", "Heat", "Emission", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                 "Heat added each second. Hot gas rises, and cools as it goes."});
    p.push_back({"velocity", "Velocity", "Emission", K::Vector, {0.0f, 0.5f, 0.0f}, -2.0f, 2.0f, -kBig, kBig, "m/s",
                 "The gas leaves the source at least this fast, this way."});
    p.push_back({"flicker", "Flicker", "Noise", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                 "How much the output flickers: 0 steady, 1 strongly -- what makes flames lick."});
    p.push_back({"flicker_size", "Flicker Size", "Noise", K::Float, {0.07f, 0.0f, 0.0f}, 0.01f, 0.3f, 0.005f, kBig,
                 "m", "Size of the patches that flicker together."});
    p.push_back(seed("Noise", "Another number, another flicker."));
    p.push_back({"start", "Start", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When it starts."});
    p.push_back({"end", "End", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                 "When it stops. At or before the start, it never does."});
    p.push_back({"motion", "Motion", "Motion", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                 "How it moves: it stays, goes round in a circle about its centre, or sways from side to side.",
                 {"static", "circle", "sway"}, {"Static", "Circle", "Sway"}});
    p.push_back({"motion_size", "Reach", "Motion", K::Float, {0.25f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, kBig, "m",
                 "Radius of the circle; how far it sways."});
    p.push_back({"motion_period", "Period", "Motion", K::Float, {4.0f, 0.0f, 0.0f}, 0.2f, 10.0f, 0.05f, kBig, "s",
                 "Seconds for one round."});
    return p;
}

std::vector<ParamDef> legacyColliderParams(bool sphere) {
    std::vector<ParamDef> p;
    p.push_back(center("Shape", Vec3(0.0f, 0.6f, 0.0f), "Where the solid is."));
    if (sphere) {
        p.push_back({"radius", "Radius", "Shape", K::Float, {0.15f, 0.0f, 0.0f}, 0.01f, 1.0f, 0.005f, kBig, "m",
                     "How big it is."});
    } else {
        p.push_back({"size", "Size", "Shape", K::Vector, {0.3f, 0.3f, 0.3f}, 0.01f, 2.0f, 0.005f, kBig, "m",
                     "Width, height and depth."});
    }
    return p;
}

// --- the solver, the looks, the output -------------------------------------------------

/// The Pyro Solver's parameters; version 1 had the frame rate, which is the
/// Output's now.
std::vector<ParamDef> pyroSolverParams(bool withFrameRate) {
    std::vector<ParamDef> p = {{"size", "Size", "Domain", K::Vector, {1.0f, 1.5f, 1.0f}, 0.1f, 5.0f, 0.1f, 1000.0f, "m",
           "Width, height and depth of the box the gas lives in. It stands on the floor, centred."},
          {"resolution", "Resolution", "Domain", K::Int, {96.0f, 0.0f, 0.0f}, 16.0f, 256.0f, 16.0f, 1024.0f, "",
           "Cells along the longest side. Twice as many: finer detail, and eight times the work."},
          {"closed_floor", "Closed Floor", "Domain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A floor the gas cannot pass. Off, the bottom is open like the sides and the top."},
          {"sparse", "Sparse", "Domain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "Work only where the gas is -- in tiles of 8 cells a side, and those round them. The rest of the "
           "domain is still, empty air: a big domain costs what its gas does."},
          {"cutoff", "Cutoff", "Domain", K::Float, {0.001f, 0.0f, 0.0f}, 0.0f, 0.05f, 0.0f, 1.0f, "",
           "Sparse: a tile whose smoke, heat, fuel and flame all stay below this is let go."},
          {"fps", "Frame Rate", "Time", K::Float, {30.0f, 0.0f, 0.0f}, 10.0f, 120.0f, 1.0f, 10000.0f, "fps",
           "Frames a second. Each frame moves the gas on by 1/fps seconds."},
          {"substeps", "Substeps", "Time", K::Int, {1.0f, 0.0f, 0.0f}, 1.0f, 8.0f, 1.0f, 16.0f, "",
           "Steps a frame is split into: for fast gas, explosions."},
          {"pressure_cycles", "Pressure Cycles", "Time", K::Int, {2.0f, 0.0f, 0.0f}, 1.0f, 8.0f, 1.0f, 16.0f, "",
           "Rounds of the pressure solver each step. More keeps the gas from squeezing together."},
          seed("Time", "Another number, other turbulence and flicker."),
          {"buoyancy", "Buoyancy", "Motion", K::Float, {1.0f, 0.0f, 0.0f}, -2.0f, 5.0f, -kBig, kBig, "",
           "How strongly heat lifts the gas."},
          {"weight", "Smoke Weight", "Motion", K::Float, {0.05f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "",
           "How strongly smoke weighs the gas down."},
          {"vorticity", "Swirl", "Motion", K::Float, {0.6f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, kBig, "",
           "Brings back the small swirls a coarse grid smooths away (vorticity confinement)."},
          {"burn_rate", "Burn Rate", "Combustion", K::Float, {10.0f, 0.0f, 0.0f}, 0.0f, 40.0f, 0.0f, kBig, "1/s",
           "How fast fuel burns."},
          {"heat_release", "Heat", "Combustion", K::Float, {2.5f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "",
           "Heat from each unit of fuel burnt."},
          {"soot_release", "Soot", "Combustion", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "",
           "Smoke from each unit of fuel burnt."},
          {"expansion", "Expansion", "Combustion", K::Float, {0.8f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "",
           "How much burning gas swells: the push of an explosion."},
          {"flame_life", "Flame Life", "Combustion", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, kBig, "s",
           "How long a flame shows once its fuel has burnt."},
          {"cooling", "Cooling", "Dissipation", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "1/s",
           "How fast heat fades."},
          {"smoke_decay", "Smoke Decay", "Dissipation", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "1/s",
           "How fast smoke thins out."}};
    if (!withFrameRate) {
        p.erase(std::remove_if(p.begin(), p.end(), [](const ParamDef& d) { return std::string(d.name) == "fps"; }), p.end());
    }
    return p;
}

/// Version 1 of the Volume Look had the light as well; the look of the gas
/// is what is left.
const char* const kEnvironment[] = {"light_azimuth", "light_elevation", "light_color", "light_intensity",
                                    "sky_color",     "sky_intensity",   "exposure",    "floor"};

std::vector<ParamDef> legacyVolumeLookParams() {
    return {{"smoke_color", "Color", "Smoke", K::Color, {0.75f, 0.75f, 0.77f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of smoke in light: pale for steam, dark for soot."},
          {"smoke_density", "Density", "Smoke", K::Float, {20.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, kBig, "",
           "How much light it stops: thin haze to thick soot."},
          {"occlusion", "Occlusion", "Smoke", K::Float, {3.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "",
           "How much thick smoke around darkens the light of the sky."},
          {"flame_intensity", "Intensity", "Fire", K::Float, {30.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, kBig, "",
           "How brightly the fire glows."},
          {"flame_start", "Glow From", "Fire", K::Float, {0.3f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "",
           "Temperature where the gas starts to glow red."},
          {"flame_range", "White At", "Fire", K::Float, {4.0f, 0.0f, 0.0f}, 0.1f, 20.0f, 0.1f, kBig, "",
           "How much hotter than that it glows yellow-white."},
          {"fire_light", "Fire Light", "Fire", K::Float, {2.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "",
           "How much the fire lights the smoke around it."},
          {"light_azimuth", "Sun Around", "Light", K::Float, {169.0f, 0.0f, 0.0f}, 0.0f, 360.0f, -kBig, kBig,
           "\xc2\xb0", "Where the sun is, round the vertical."},
          {"light_elevation", "Sun Height", "Light", K::Float, {38.0f, 0.0f, 0.0f}, -10.0f, 90.0f, -90.0f, 90.0f,
           "\xc2\xb0", "How high the sun is above the horizon."},
          {"light_color", "Sun Color", "Light", K::Color, {1.0f, 0.95f, 0.88f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of sunlight."},
          {"light_intensity", "Sun", "Light", K::Float, {2.2f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "",
           "How bright the sun is."},
          {"sky_color", "Sky Color", "Light", K::Color, {0.55f, 0.65f, 0.8f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of the light from the sky."},
          {"sky_intensity", "Sky", "Light", K::Float, {0.25f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "",
           "How bright the sky is."},
          {"exposure", "Exposure", "Image", K::Float, {1.0f, 0.0f, 0.0f}, 0.05f, 8.0f, 0.01f, 100.0f, "",
           "How bright the whole image is."},
          {"floor", "Floor", "Image", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "Draw the floor, with the shadow of the smoke and the glow of the fire on it."}};
}

bool isEnvironment(const char* name) {
    return std::any_of(std::begin(kEnvironment), std::end(kEnvironment), [&](const char* e) { return std::string(e) == name; });
}

std::vector<ParamDef> volumeLookParams() {
    std::vector<ParamDef> p = legacyVolumeLookParams();
    p.erase(std::remove_if(p.begin(), p.end(), [](const ParamDef& d) { return isEnvironment(d.name); }), p.end());
    return p;
}

/// How the path tracer renders the shot (render/PathTracer.h): the Output's
/// Render section, what the editor's Render tab and `prototype sim
/// --renderer path` take.
std::vector<ParamDef> renderParams() {
    return {{"render_samples", "Samples", "Render", K::Int, {128.0f, 0.0f, 0.0f}, 1.0f, 1024.0f, 1.0f, 65536.0f, "",
             "How many samples a pixel the render takes: more, less noise. 16 a quick look, 128 a picture, 512 "
             "one to print."},
            {"render_bounces", "Bounces", "Render", K::Int, {4.0f, 0.0f, 0.0f}, 0.0f, 12.0f, 0.0f, 64.0f, "",
             "How many times light may bounce from surface to surface on its way to the camera: 0 the sun and the "
             "sky alone, 4 a shadow lit by what is round it."},
            {"render_denoise", "Denoise", "Render", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "Take out the noise that is left, keeping edges: a clean picture from fewer samples."},
            {"render_fstop", "F-Stop", "Render", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 22.0f, 0.0f, 1000.0f, "",
             "The lens's f-number: 1.4 a shallow focus, the background blurred; 16 nearly all sharp. 0: "
             "everything sharp."},
            {"render_focus", "Focus", "Render", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 50.0f, 0.0f, kBig, "m",
             "How far from the camera what is sharp is. 0: what the middle of the picture sees."},
            {"render_clamp", "Clamp", "Render", K::Float, {20.0f, 0.0f, 0.0f}, 1.0f, 100.0f, 0.1f, kBig, "",
             "The most light a bounce may add to a pixel: no bright specks (fireflies) where light found a rare "
             "way, a little less of what is lit only that way."},
            {"render_motion_blur", "Motion Blur", "Render", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f,
             "frames",
             "How much of a frame the shutter is open, about the frame: in Cycles what moves -- the pieces, the "
             "grit, the cloth, the water, a moving camera -- blurs along its way over that time, as on film. 0.5 "
             "half a frame (a 180\xc2\xb0 shutter), 0 all sharp. The path tracer renders the frame's moment."},
            {"render_sun_angle", "Sun Size", "Render", K::Float, {0.53f, 0.0f, 0.0f}, 0.1f, 5.0f, 0.01f, 30.0f,
             "\xc2\xb0",
             "How wide the sun is, degrees: 0.53 the real sun, sharp shadows near what casts them and soft far "
             "from it; larger, softer -- a hazy day."},
            {"render_sky", "Sky", "Render", K::Choice, {1.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
             "What lights the render in Cycles. Physical: a real day's sky, as Blender's Sky Texture -- this "
             "node's sun, the sky's blue from the air it shines through, clouds as Clouds says, the ground out to "
             "the horizon. Image: a picture all round (an HDRI, Sky Image). Look: the light and sky of this node "
             "as the viewport has them (the path tracer always).",
             {"look", "physical", "image"},
             {"Look", "Physical", "Image"}},
            {"render_sky_image", "Sky Image", "Render", K::File, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
             "With Sky Image: the picture all round -- equirectangular (2 : 1), .hdr or .exr for its light as it "
             "is (Poly Haven's HDRIs, say), .png or .jpg too. It lights the scene and the camera sees it with Sky "
             "Behind. A relative path is read from the network's folder.",
             {".hdr", ".exr", ".png", ".jpg", ".jpeg"},
             {}},
            {"render_sky_rotation", "Sky Rotation", "Render", K::Float, {0.0f, 0.0f, 0.0f}, -180.0f, 180.0f, -360.0f,
             360.0f, "\xc2\xb0", "The sky picture turned about the vertical: its sun where the shot wants it."},
            {"render_sky_strength", "Sky Strength", "Render", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 1000.0f,
             "", "The sky picture's light times this."},
            {"render_sky_sun", "Sky Sun", "Render", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "With Sky Image: this node's sun too -- sharp shadows under a sky picture without a sun of its own."},
            {"render_clouds", "Clouds", "Render", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "With a physical sky: how much of it clouds cover -- 0 clear, 0.3 a few, 0.6 broken, 1 overcast, the "
             "sun mostly hidden."},
            {"render_cloud_size", "Cloud Size", "Render", K::Float, {1.5f, 0.0f, 0.0f}, 0.1f, 5.0f, 0.01f, 100.0f,
             "km", "How big the clouds are: some kilometre and a half across at 1.5."},
            {"render_cloud_wind", "Cloud Wind", "Render", K::Float, {5.0f, 0.0f, 0.0f}, 0.0f, 50.0f, 0.0f, 1000.0f,
             "m/s", "How fast the wind takes the clouds over the sky, frame after frame."},
            {"render_cloud_direction", "Cloud Direction", "Render", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 360.0f, -360.0f,
             720.0f, "\xc2\xb0", "Which way the wind takes the clouds, degrees round from +x."},
            {"render_view", "View", "Render", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
             "How the light becomes the picture. AgX: as Blender shows it, bright colours going towards white as "
             "on film -- Punchy with Blender's look of more contrast and colour. ACES: as the viewport.",
             {"agx_punchy", "agx", "aces"},
             {"AgX Punchy", "AgX", "ACES"}},
            {"render_detail", "Surface Detail", "Render", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "What Cycles adds to surfaces the scene has flat: colour and roughness that vary, small bumps that "
             "catch the light -- as stone, plaster and the ground are. 0: as flat as the viewport draws them."},
            {"render_textures", "Textures", "Render", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "The photographs of the materials -- concrete, plaster, a brick wall, wood, bark, soil -- and the "
             "textures the Material nodes give. Off: their patterns and colours alone."},
            {"render_texture_folder", "Texture Folder", "Render", K::Text, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f,
             0.0f, "",
             "Where the materials' photographs are: a folder for each, named as the material (concrete, plaster, "
             "brick_wall...), each with color.jpg, height.jpg and texture.txt. Empty: the ones that come with the "
             "program (examples/textures). A relative path is read from the network's folder."}};
}

std::vector<ParamDef> outputParams() {
    std::vector<ParamDef> p = {
        {"frames", "Frames", "Output", K::Int, {150.0f, 0.0f, 0.0f}, 1.0f, 1000.0f, 1.0f, 100000.0f, "",
         "How many frames to simulate: the length of the timeline."},
        {"fps", "Frame Rate", "Output", K::Float, {30.0f, 0.0f, 0.0f}, 10.0f, 120.0f, 1.0f, 10000.0f, "fps",
         "Frames a second. Each frame moves every simulation on by 1/fps seconds."}};
    // The sun, the sky and the image, as the Volume Look had them.
    for (ParamDef d : legacyVolumeLookParams()) {
        if (!isEnvironment(d.name)) continue;
        const std::string name = d.name;
        d.section = name == "exposure" || name == "floor" ? "Image" : name.rfind("sky", 0) == 0 ? "Sky" : "Sun";
        if (name == "floor") d.help = "Draw the floor, with the shadows and the glow of the fire on it.";
        p.push_back(d);
    }
    p.push_back({"ground_color", "Ground Color", "Image", K::Color, {0.075f, 0.075f, 0.075f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                 "The colour of the floor: dark asphalt, pale concrete, dusty earth."});
    p.push_back({"grid", "Grid", "Image", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                 "Lines on the floor every 10 cm and every metre, to judge sizes by. Off for a shot."});
    p.push_back({"sky_behind", "Sky Behind", "Image", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                 "The sky behind everything, where the floor ends: hazy towards the horizon, glowing round the "
                 "sun -- outdoors, and smoke against the light. Off, the dark backdrop of a studio."});
    p.push_back({"floor_matte", "Floor over the Plate", "Image", K::Choice, {2.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f,
                 "",
                 "What the floor is when the camera has a plate. Shadow Catcher: the ground the plate was filmed "
                 "on -- the plate shows there, darker in the shadows of the CG, brighter where the fire lights it. "
                 "Holdout: the plate as it is. Solid: the floor drawn over the plate.",
                 {"solid", "holdout", "catcher"},
                 {"Solid", "Holdout", "Shadow Catcher"}});
    for (ParamDef& d : renderParams()) p.push_back(d);
    return p;
}

std::vector<NodeType> buildTypes() {
    std::vector<NodeType> t;

    // --- geometry -----------------------------------------------------------------------
    // Nodes that make and change geometry, as the SOPs of Houdini do: each is
    // a node of the core's cook engine (pg/nodes, `core` names it), cooked
    // lazily -- only what changed cooks again. Their parameters carry the
    // names the core's nodes read.
    const std::vector<PinDef> in = {{"geometry", "Geometry", PinType::Geometry}};
    const std::vector<PinDef> out = {{"geometry", "Geometry", PinType::Geometry}};
    auto vec = [](const char* name, const char* label, const char* section, Vec3 v, float lo, float hi,
                  const char* unit, const char* help) {
        return ParamDef{name, label, section, K::Vector, {v.x, v.y, v.z}, lo, hi, -kBig, kBig, unit, help};
    };
    auto text = [](const char* name, const char* label, const char* section, const char* byDefault, const char* help) {
        ParamDef d{name, label, section, K::Text, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "", help};
        d.text = byDefault;
        return d;
    };
    auto geometry = [&](const char* name, const char* label, const char* core, const char* help,
                        std::vector<PinDef> inputs, std::vector<ParamDef> params, Handles handles = {}) {
        t.push_back({name, label, "Geometry", help, std::move(inputs), out, std::move(params), 1});
        t.back().core = core;
        t.back().handles = handles;
    };

    geometry("box", "Box", "box",
             "A box of six faces turned outward, each cut into quads that share their points.",
             {},
             {vec("size", "Size", "Box", Vec3(1.0f, 1.0f, 1.0f), 0.01f, 5.0f, "m", "Width, height and depth."),
              vec("center", "Center", "Box", Vec3(0.0f, 0.5f, 0.0f), -5.0f, 5.0f, "m",
                  "Its middle. Half its height up stands it on the floor."),
              {"divisions", "Divisions", "Box", K::Int, {1.0f, 0.0f, 0.0f}, 1.0f, 20.0f, 1.0f, 100.0f, "",
               "How many quads each face is cut into along each side."}},
             {"center", nullptr, nullptr, "size", nullptr, nullptr});
    geometry("sphere", "Sphere", "sphere",
             "A sphere of quads, and triangles at the poles: rows from pole to pole, columns round it.",
             {},
             {{"radius", "Radius", "Sphere", K::Float, {0.5f, 0.0f, 0.0f}, 0.01f, 5.0f, 0.0f, kBig, "m", "How big it is."},
              vec("center", "Center", "Sphere", Vec3(0.0f, 0.5f, 0.0f), -5.0f, 5.0f, "m", "Its middle."),
              {"rows", "Rows", "Sphere", K::Int, {12.0f, 0.0f, 0.0f}, 3.0f, 64.0f, 3.0f, 1000.0f, "",
               "Bands from pole to pole."},
              {"columns", "Columns", "Sphere", K::Int, {24.0f, 0.0f, 0.0f}, 3.0f, 128.0f, 3.0f, 1000.0f, "",
               "Faces round it."}},
             {"center", nullptr, nullptr, nullptr, "radius", nullptr});
    geometry("tube", "Tube", "tube",
             "A tube standing along y: quads round it and up it, closed at the ends by a polygon each.",
             {},
             {{"radius", "Radius", "Tube", K::Float, {0.3f, 0.0f, 0.0f}, 0.01f, 5.0f, 0.0f, kBig, "m", "How thick."},
              {"height", "Height", "Tube", K::Float, {1.0f, 0.0f, 0.0f}, 0.01f, 5.0f, 0.0f, kBig, "m", "How tall."},
              vec("center", "Center", "Tube", Vec3(0.0f, 0.5f, 0.0f), -5.0f, 5.0f, "m", "Its middle."),
              {"columns", "Columns", "Tube", K::Int, {24.0f, 0.0f, 0.0f}, 3.0f, 128.0f, 3.0f, 1000.0f, "",
               "Faces round it."},
              {"rows", "Rows", "Tube", K::Int, {1.0f, 0.0f, 0.0f}, 1.0f, 64.0f, 1.0f, 1000.0f, "", "Faces up it."},
              {"caps", "Caps", "Tube", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "A polygon over each end: closed, it has an inside."}},
             {"center", nullptr, nullptr, nullptr, "radius", "height"});
    geometry("grid", "Grid", "grid",
             "A flat grid of quads on the floor: rows along z, columns along x. Displaced by a wrangle it is "
             "a terrain.",
             {},
             {{"sizex", "Size X", "Grid", K::Float, {2.0f, 0.0f, 0.0f}, 0.1f, 20.0f, 0.0f, kBig, "m", "Along x."},
              {"sizez", "Size Z", "Grid", K::Float, {2.0f, 0.0f, 0.0f}, 0.1f, 20.0f, 0.0f, kBig, "m", "Along z."},
              {"rows", "Rows", "Grid", K::Int, {10.0f, 0.0f, 0.0f}, 2.0f, 200.0f, 2.0f, 10000.0f, "",
               "Points along z."},
              {"cols", "Columns", "Grid", K::Int, {10.0f, 0.0f, 0.0f}, 2.0f, 200.0f, 2.0f, 10000.0f, "",
               "Points along x."},
              vec("center", "Center", "Grid", Vec3(), -5.0f, 5.0f, "m", "Its middle.")},
             {"center", nullptr, nullptr, nullptr, nullptr, nullptr});
    geometry("line", "Line", "line", "An open polyline: points from the origin along a direction.",
             {},
             {vec("origin", "Origin", "Line", Vec3(), -5.0f, 5.0f, "m", "Where it starts."),
              vec("direction", "Direction", "Line", Vec3(1.0f, 0.0f, 0.0f), -1.0f, 1.0f, "", "Which way it goes."),
              {"length", "Length", "Line", K::Float, {1.0f, 0.0f, 0.0f}, 0.01f, 10.0f, 0.0f, kBig, "m", "How long."},
              {"points", "Points", "Line", K::Int, {10.0f, 0.0f, 0.0f}, 2.0f, 100.0f, 2.0f, 100000.0f, "",
               "How many points along it."}},
             {"origin", nullptr, "direction", nullptr, nullptr, nullptr});
    geometry("point_cloud", "Point Cloud", "pointcloud",
             "Loose points scattered in a cube, the same for the same seed.",
             {},
             {{"count", "Count", "Points", K::Int, {1000.0f, 0.0f, 0.0f}, 0.0f, 100000.0f, 0.0f, 1e7f, "",
               "How many points."},
              seed("Points", "Another number, other places."),
              {"size", "Size", "Points", K::Float, {1.0f, 0.0f, 0.0f}, 0.01f, 10.0f, 0.0f, kBig, "m",
               "The side of the cube they are in."},
              vec("center", "Center", "Points", Vec3(0.0f, 0.5f, 0.0f), -5.0f, 5.0f, "m", "The cube's middle.")},
             {"center", nullptr, nullptr, nullptr, nullptr, nullptr});
    {
        auto level = [](int n, const char* section, int branches, float angle, float length) {
            static const char* names[3][3] = {{"branches1", "angle1", "length1"},
                                              {"branches2", "angle2", "length2"},
                                              {"branches3", "angle3", "length3"}};
            const char* const* name = names[n - 1];
            return std::vector<ParamDef>{
                {name[0], "Branches", section, K::Int, {static_cast<float>(branches), 0.0f, 0.0f}, 0.0f, 60.0f, 0.0f, 200.0f,
                 "", n == 1 ? "How many branches grow from the trunk, from the crown's foot up."
                            : "How many branches grow from each branch of the level before."},
                {name[1], "Angle", section, K::Float, {angle, 0.0f, 0.0f}, 0.0f, 120.0f, 0.0f, 180.0f, "\xc2\xb0",
                 "How far off their parent they grow: 0 along it, 90 square to it."},
                {name[2], "Length", section, K::Float, {length, 0.0f, 0.0f}, 0.0f, 1.2f, 0.0f, 5.0f, "",
                 n == 1 ? "How long the longest of them is, a share of the trunk's length; the crown's shape says "
                          "how long the others are."
                        : "How long they are at their parent's base, a share of its length -- shorter towards its "
                          "tip."}};
        };
        std::vector<ParamDef> p = {
            {"shape", "Shape", "Tree", K::Choice, {1.0f, 0.0f, 0.0f}, 0.0f, 6.0f, 0.0f, 6.0f, "",
             "The crown's outline -- how long the first branches are from its foot up. Conical: longest at the "
             "foot, a spruce, a fir. Spherical: longest halfway, an oak, a lime. Hemispherical: long at the foot, "
             "round over the top. Cylindrical: all as long, a poplar. Flame: longest two thirds down, a birch. "
             "Umbrella: longest at the top, an acacia, a stone pine. Weeping: round, the twigs hanging, a "
             "willow.",
             {"conical", "spherical", "hemispherical", "cylindrical", "flame", "umbrella", "weeping"},
             {"Conical", "Spherical", "Hemispherical", "Cylindrical", "Flame", "Umbrella", "Weeping"}},
            {"height", "Height", "Tree", K::Float, {6.0f, 0.0f, 0.0f}, 0.5f, 30.0f, 0.01f, kBig, "m",
             "How long the trunk is; the crown reaches a little higher."},
            {"radius", "Radius", "Tree", K::Float, {0.16f, 0.0f, 0.0f}, 0.01f, 1.0f, 1e-4f, kBig, "m",
             "How thick the trunk is above its foot."},
            seed("Tree", "Another number: another tree of the same kind."),
            vec("center", "Center", "Tree", Vec3(), -5.0f, 5.0f, "m", "Where it stands, without points in."),
            {"sizevariation", "Size Variation", "Tree", K::Float, {0.2f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "How much the trees on the points differ in size, one from the next."},
            {"tip", "Tip", "Trunk", K::Float, {0.08f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "How thick the trunk is at its top, a share of Radius."},
            {"flare", "Flare", "Trunk", K::Float, {0.35f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "",
             "How much wider it is at the ground, where the roots go in."},
            {"lean", "Lean", "Trunk", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "How far it leans one way and bows back."},
            {"crown", "Crown", "Trunk", K::Float, {0.35f, 0.0f, 0.0f}, 0.0f, 0.95f, 0.0f, 0.95f, "",
             "Where the branches begin, a share of the trunk's length: below it the trunk is bare."},
            {"forks", "Forks", "Trunk", K::Int, {1.0f, 0.0f, 0.0f}, 1.0f, 5.0f, 1.0f, 5.0f, "",
             "How many leaders the trunk parts into -- an oak's, a maple's, an acacia's crown on two or three. "
             "1: it goes up to its top, a spruce's, a poplar's."},
            {"forkheight", "Fork Height", "Trunk", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "Where it forks, a share of its length."},
            {"forkangle", "Fork Angle", "Trunk", K::Float, {25.0f, 0.0f, 0.0f}, 0.0f, 60.0f, 0.0f, 90.0f, "\xc2\xb0",
             "How far the leaders diverge; they turn back up as they grow."},
            {"levels", "Levels", "Branches", K::Int, {3.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 3.0f, "",
             "Branches off the trunk, off those, off those: how many levels. 0: a bare trunk."},
            {"thickness", "Thickness", "Branches", K::Float, {0.55f, 0.0f, 0.0f}, 0.05f, 0.95f, 0.05f, 0.95f, "",
             "How thick a branch is at its base, a share of its parent's thickness where it grows."},
            {"gravity", "Gravity", "Branches", K::Float, {0.25f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 4.0f, "",
             "How much the branches droop under their weight -- the thinner, the more, towards their tips."},
            {"up", "Up", "Branches", K::Float, {0.25f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 4.0f, "",
             "How much they turn up to the light as they grow."},
            {"wobble", "Wobble", "Branches", K::Float, {0.3f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 4.0f, "",
             "How much they wander this way and that as they grow."}};
        for (auto&& d : level(1, "Level 1", 28, 55.0f, 0.5f)) p.push_back(d);
        for (auto&& d : level(2, "Level 2", 7, 45.0f, 0.45f)) p.push_back(d);
        for (auto&& d : level(3, "Level 3", 5, 40.0f, 0.4f)) p.push_back(d);
        p.push_back({"leaves", "Leaves", "Leaves", K::Int, {10.0f, 0.0f, 0.0f}, 0.0f, 60.0f, 0.0f, 500.0f, "",
                     "How many leaves grow on each twig -- each branch nothing grows from."});
        p.push_back({"leafsize", "Leaf Size", "Leaves", K::Float, {0.12f, 0.0f, 0.0f}, 0.01f, 0.5f, 1e-4f, kBig, "m",
                     "How long a leaf is."});
        p.push_back({"leafshape", "Leaf Shape", "Leaves", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                     "Broad: an oval blade, an oak's, a lime's. Narrow: a long thin one, a willow's. Needles: a "
                     "conifer's -- many to a twig.",
                     {"broad", "narrow", "needles"}, {"Broad", "Narrow", "Needles"}});
        p.push_back({"barkcolor", "Bark Color", "Look", K::Color, {0.14f, 0.11f, 0.085f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                     "The colour of the bark, as Cd -- the young wood a little lighter."});
        p.push_back({"leafcolor", "Leaf Color", "Look", K::Color, {0.1f, 0.23f, 0.05f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                     "The colour of the leaves, as Cd."});
        p.push_back({"variation", "Variation", "Look", K::Float, {0.3f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                     "How much the leaves differ in shade -- lighter, darker, yellower -- and the trees' bark."});
        p.push_back({"sides", "Sides", "Detail", K::Int, {10.0f, 0.0f, 0.0f}, 3.0f, 32.0f, 3.0f, 64.0f, "",
                     "Faces round the trunk; two fewer round each level of branches, three at the least -- "
                     "never six: their edges would be as sharp as the viewport's crease, 60 degrees, smooth one "
                     "frame and sharp the next as the tree sways; seven instead."});
        p.push_back({"segment", "Segment", "Detail", K::Float, {0.25f, 0.0f, 0.0f}, 0.05f, 1.0f, 0.01f, kBig, "m",
                     "How long a piece of the trunk is; the branches in finer pieces, level by level. Longer: "
                     "fewer faces, for a forest far away."});
        p.push_back({"output", "Output", "Detail", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
                     "Mesh: the stems as tubes of bark, the leaves as polygons -- the groups bark and leaves. "
                     "Skeleton: each stem an open polyline, pscale its radius, and each leaf a point -- N the way "
                     "it faces, pscale its size, orient, in the group leaves -- for Copy to Points to put a leaf "
                     "of your own on: modelled lying flat, facing +y, its stalk at the origin, pointing along +z. "
                     "Instances: Variants trees grown once and a point for each tree standing for one of them, "
                     "turned about +y as it happens, a shade of its own -- a forest of thousands, drawn "
                     "instanced.",
                     {"mesh", "skeleton", "instances"}, {"Mesh", "Skeleton", "Instances"}});
        p.push_back({"variants", "Variants", "Detail", K::Int, {8.0f, 0.0f, 0.0f}, 1.0f, 32.0f, 1.0f, 64.0f, "",
                     "Instances: how many trees are grown -- the ones the first points would grow -- for all the "
                     "points to stand for."});
        geometry("tree", "Tree", "tree",
                 "A tree grown as a plant grows: a trunk thick at its foot, branches off it round it a golden "
                 "angle on from the one before, branches off those, each turned up to the light, bent under its "
                 "weight and wandering a little, and leaves on the twigs. The crown's shape says how long the "
                 "first branches are from its foot up (after Weber and Penn). With points in, a tree on each -- a "
                 "forest, each tree its own: its id, else its number, grows it; pscale sizes it. Point Cd and "
                 "flex -- how far along the wood from the tree's foot, a share of its height: 0 at the ground, "
                 "about 1 at the crown's top, what a wrangle bends it in the wind by; primitive level (-1 a "
                 "leaf), stem, tree.",
                 {{"points", "Points", PinType::Geometry}}, std::move(p),
                 {"center", nullptr, nullptr, nullptr, "radius", "height"});
    }
    geometry("grass", "Grass", "grass",
             "Grass as it grows: clumps of blades from one root, each blade narrowing to its tip, leaning out "
             "and bowing over under its weight, dark green at the root, light at the tip, a blade here and there "
             "dry. With a surface in, clumps over it, Density to a square metre -- as instances: Variants clumps "
             "held once and a point for each clump in the meadow (instance, orient, pscale, tint), millions of "
             "blades for what their points cost. Only points in: a clump on each. Nothing in: one clump at "
             "Center. Point Cd and flex -- how far along the blade, 0 at the root, 1 at the tip.",
             {{"surface", "Surface", PinType::Geometry}},
             {{"density", "Density", "Grass", K::Float, {50.0f, 0.0f, 0.0f}, 0.0f, 200.0f, 0.0f, kBig, "1/m\xc2\xb2",
               "How many clumps to a square metre of the surface."},
              seed("Grass", "Another number: other places, other clumps."),
              vec("center", "Center", "Grass", Vec3(), -5.0f, 5.0f, "m", "Where the clump stands, with nothing in."),
              {"sizevariation", "Size Variation", "Grass", K::Float, {0.3f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How much the clumps differ in size, one from the next -- on top of the points' pscale."},
              {"alongnormal", "Along Normal", "Grass", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Grow out of the surface along its normal -- moss on a wall. Off: straight up, as grass grows "
               "on a slope."},
              text("densityattribute", "Density Attribute", "Rules", "",
                   "A point attribute of the surface, 0 to 1 -- painted, or a wrangle's -- that says what share of "
                   "the clumps grow there: 0 none, a path, 1 all. Empty: all."),
              {"maxslope", "Max Slope", "Rules", K::Float, {45.0f, 0.0f, 0.0f}, 0.0f, 180.0f, 0.0f, 180.0f, "\xc2\xb0",
               "No grass where the face leans more than this from level: off rocks and cliffs."},
              {"blades", "Blades", "Blades", K::Int, {16.0f, 0.0f, 0.0f}, 1.0f, 60.0f, 0.0f, 1000.0f, "",
               "How many blades in a clump."},
              {"height", "Height", "Blades", K::Float, {0.4f, 0.0f, 0.0f}, 0.02f, 1.5f, 0.0f, kBig, "m",
               "How long a blade is: a lawn 0.08, a meadow 0.4, tall grass 1."},
              {"heightvariation", "Height Variation", "Blades", K::Float, {0.4f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How much the blades differ in length."},
              {"width", "Width", "Blades", K::Float, {0.006f, 0.0f, 0.0f}, 0.001f, 0.05f, 0.0f, kBig, "m",
               "How wide a blade is at its root; it narrows to its tip."},
              {"bend", "Bend", "Blades", K::Float, {0.55f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How far the blades bow over under their weight: 0 upright, 1 their tips level."},
              {"lean", "Lean", "Blades", K::Float, {30.0f, 0.0f, 0.0f}, 0.0f, 60.0f, 0.0f, 90.0f, "\xc2\xb0",
               "How far they lean out from the clump's middle, at the most."},
              {"spread", "Spread", "Blades", K::Float, {0.08f, 0.0f, 0.0f}, 0.0f, 0.3f, 0.0f, kBig, "m",
               "How far from the clump's middle their roots are, at the most."},
              {"segments", "Segments", "Blades", K::Int, {4.0f, 0.0f, 0.0f}, 1.0f, 12.0f, 1.0f, 16.0f, "",
               "Pieces along a blade: more, a smoother bow; fewer, lighter for a field far away."},
              {"rootcolor", "Root Color", "Look", K::Color, {0.08f, 0.14f, 0.03f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour of the blades at the root, as Cd."},
              {"tipcolor", "Tip Color", "Look", K::Color, {0.25f, 0.4f, 0.08f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour at the tip."},
              {"dry", "Dry", "Look", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The share of blades that are dry: 0 spring, 0.5 late summer."},
              {"drycolor", "Dry Color", "Look", K::Color, {0.45f, 0.38f, 0.15f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour of a dry blade."},
              {"variation", "Variation", "Look", K::Float, {0.2f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How much the blades, and the clumps, differ in shade -- lighter, darker, yellower."},
              {"variants", "Variants", "Output", K::Int, {8.0f, 0.0f, 0.0f}, 1.0f, 32.0f, 1.0f, 64.0f, "",
               "How many clumps are grown, for the points to stand for."},
              {"instances", "Instances", "Output", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The points, each standing for its clump, drawn instanced. Off: the clumps made copies of -- "
               "geometry every node can change, as heavy as its blades."}},
             {"center", nullptr, nullptr, nullptr, nullptr, "height"});
    {
        ParamDef f = file("An OBJ file: its points, polygons and lines. A relative path is read from the "
                          "network's folder; a file that changes is read again.");
        f.section = "File";
        geometry("file", "File", "file", "Geometry from an OBJ file -- from Blender, Houdini, Maya, anywhere.",
                 {}, {f});
    }
    {
        auto toggle = [](const char* name, const char* label, bool on, const char* help) {
            return ParamDef{name, label, "Import", K::Toggle, {on ? 1.0f : 0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "", help};
        };
        geometry("usd_import", "USD Import", "usdimport",
                 "Geometry from a USD file -- a set, a scan, props, a cache from Houdini, Maya or Blender -- as its "
                 "stage composes it: sublayers, references, payloads, variants. Meshes with their normals, uv, "
                 "colours and primvars; curves; points; the implicit shapes as polygons; each primitive's prim in "
                 "`path`, a mesh's subsets as groups. Animated, it is read at each frame.",
                 {},
                 {usdFile("A USD file: .usd, .usda, .usdc or .usdz. A relative path is read from the network's "
                          "folder; a file that changes is read again."),
                  text("prims", "Prims", "Import", "",
                       "Which prims to read, with all that is under them: paths separated by spaces "
                       "(/World/set /World/props). Empty: the whole stage."),
                  frameOffset(),
                  toggle("render", "Render", true, "Read what is only for renders (purpose render)."),
                  toggle("proxy", "Proxy", false, "Read the light stand-ins of heavy geometry (purpose proxy)."),
                  toggle("guide", "Guide", false, "Read what only guides the eye (purpose guide)."),
                  toggle("metres", "Metres, Y Up", true,
                         "The stage's units and up axis made the program's: metres -- metersPerUnit; centimetres "
                         "when the stage says none -- and Y up. Off: as the file has them."),
                  toggle("subsets", "Subsets as Groups", true, "A mesh's subsets of faces as primitive groups of their names."),
                  toggle("path", "Path Attribute", true, "Each primitive's prim, as the text attribute path.")});
    }
    geometry("transform", "Transform", "transform",
             "Moves, turns and sizes what comes in: scale, then rotate -- both about the pivot -- then "
             "translate. Only the positions (and normals) are written; every other attribute is shared, not "
             "copied.",
             in,
             {vec("t", "Translate", "Transform", Vec3(), -5.0f, 5.0f, "m", "How far it moves."),
              vec("r", "Rotate", "Transform", Vec3(), -180.0f, 180.0f, "\xc2\xb0",
                  "Degrees about x, then y, then z, about the pivot."),
              vec("s", "Scale", "Transform", Vec3(1.0f, 1.0f, 1.0f), 0.0f, 5.0f, "", "How much larger along x, y, z."),
              {"scale", "Uniform Scale", "Transform", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, -kBig, kBig, "",
               "How much larger, all ways."},
              vec("p", "Pivot", "Transform", Vec3(), -5.0f, 5.0f, "m",
                  "The point it turns and sizes about -- the foot of a tower that is to topple.")},
             {"t", "r", nullptr, "s", nullptr, nullptr, "p"});
    t.push_back({"merge", "Merge", "Geometry",
                 "Everything linked into it, one after the other: points, primitives, attributes (missing ones "
                 "filled with zeros), groups and volumes.",
                 {{"geometry", "Geometry", PinType::Geometry, true}}, out, {}, 1});
    t.back().core = "merge";
    t.push_back({"switch", "Switch", "Geometry", "One of the geometries linked into it: the one at Index, counting from 0.",
                 {{"geometry", "Geometry", PinType::Geometry, true}}, out,
                 {{"index", "Index", "Switch", K::Int, {0.0f, 0.0f, 0.0f}, 0.0f, 9.0f, 0.0f, 1000.0f, "",
                   "Which input, from 0."}},
                 1});
    t.back().core = "switch";
    geometry("attribute_create", "Attribute Create", "attribcreate",
             "An attribute of one value everywhere: a number or a vector, on the points, the corners, the "
             "primitives or the whole geometry.",
             in,
             {text("name", "Name", "Attribute", "mass", "What it is called: @name in a wrangle."),
              {"class", "Class", "Attribute", K::Choice, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 3.0f, "",
               "What it belongs to.", {"detail", "point", "vertex", "primitive"},
               {"Detail", "Point", "Vertex", "Primitive"}},
              {"vector", "Vector", "Attribute", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Three numbers rather than one."},
              {"value", "Value", "Attribute", K::Float, {1.0f, 0.0f, 0.0f}, -10.0f, 10.0f, -kBig, kBig, "",
               "Its value, for a number."},
              vec("vvalue", "Vector Value", "Attribute", Vec3(), -10.0f, 10.0f, "", "Its value, for a vector.")});
    geometry("color", "Color", "color", "A colour, Cd, on every point or every primitive: how the viewport draws it.",
             in,
             {{"color", "Color", "Color", K::Color, {0.9f, 0.45f, 0.2f}, 0.0f, 1.0f, 0.0f, 1.0f, "", "The colour."},
              {"class", "Class", "Color", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "On the points, or on the primitives.", {"point", "primitive"}, {"Point", "Primitive"}}});
    // The names in the order of kMaterialNames (core/Material.h), "none" for "".
    geometry("material", "Material", "material",
             "What the faces of a group are made of -- or all of them -- for the renderers: the string attribute "
             "material, and a texture of your own if you give one (texture, texture_size, texture_tint). Brick "
             "Wall, Concrete Fracture, Tree, Grass and Glass Fracture set theirs themselves.",
             in,
             {text("group", "Group", "Material", "",
                   "Which faces: a group's name, numbers and ranges -- 0-9 12 -- * or empty for all."),
              {"material", "Material", "Material", K::Choice, {1.0f, 0.0f, 0.0f}, 0.0f, 22.0f, 0.0f, 22.0f, "",
               "What the faces are. Cycles draws most with the photographs that come with the program "
               "(examples/textures) -- concrete and its break, plaster, a brick wall, mortar, metal, asphalt, wood, "
               "roofs, paving, bark, soil, a lawn, sand -- the others with a pattern of their own: brick, a window "
               "with a room behind it, rusty steel, stone, leaves, grass. All on their colour Cd, or the "
               "material's own where the geometry has none; the path tracer the photographs too, without their "
               "bumps. None takes it away.",
               {"none", "concrete", "broken_concrete", "brick", "brick_wall", "mortar", "plaster", "window", "glass",
                "steel", "metal", "asphalt", "wood", "stone", "roof", "bark", "leaf", "grass", "soil", "paving",
                "roof_tiles", "lawn", "sand"},
               {"None", "Concrete", "Broken Concrete", "Brick", "Brick Wall", "Mortar", "Plaster", "Window", "Glass",
                "Steel", "Metal", "Asphalt", "Wood", "Stone", "Roof", "Bark", "Leaf", "Grass", "Soil", "Paving",
                "Roof Tiles", "Lawn", "Sand"}},
              {"texture", "Texture", "Texture", K::File, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
               "A texture of your own instead of the material's: a picture of its colour -- the other pictures of "
               "the set beside it are found by their names (Poly Haven's _diff_, _rough_, _disp_; ambientCG's "
               "_Color, _Roughness, _Displacement), or a folder's texture.txt. Laid on from three sides by where "
               "the faces were before they moved: it goes with a piece that flies.",
               {".jpg", ".jpeg", ".png", ".exr"},
               {}},
              {"texture_size", "Texture Size", "Texture", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, 1000.0f, "m",
               "How many metres one picture covers. 0: as its texture.txt says, else 2 m."},
              {"texture_tint", "Tint by Color", "Texture", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour Cd in place of the picture's own: its pattern lighter and darker round the faces' "
               "colour. Off: the picture as it is."}});
    geometry("group_box", "Group by Box", "groupbox",
             "A group of the points inside a box: what Blast deletes, or keeps.",
             in,
             {text("name", "Group", "Group", "selected", "The group's name."),
              vec("min", "Min", "Group", Vec3(-0.5f, 0.0f, -0.5f), -5.0f, 5.0f, "m", "The box's lowest corner."),
              vec("max", "Max", "Group", Vec3(0.5f, 1.0f, 0.5f), -5.0f, 5.0f, "m", "The box's highest corner.")});
    // Which of the points or the primitives: the class of a Group, an Edit,
    // a Blast.
    auto elements = [](const char* section, const char* help) {
        return ParamDef{"class", "Class", section, K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "", help,
                        {"point", "primitive"}, {"Points", "Primitives"}};
    };
    geometry("blast", "Blast", "blast",
             "Deletes the points of a group, and the primitives they were part of -- or of class Primitives the "
             "primitives, and the points only they used. Inverted, it keeps only them. Delete in the viewport "
             "makes one of what is picked there.",
             in,
             {text("group", "Group", "Blast", "selected",
                   "Which: a group's name, numbers and ranges -- 0-9 12 -- edges by their points -- p3-4, "
                   "p0-1-2 -- * for all; ^ before one takes it away."),
              elements("Blast", "What Group names: points, or primitives."),
              {"invert", "Keep", "Blast", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Keep the group and delete the rest."}});
    geometry("group", "Group", "groupcreate",
             "A group of the points or primitives picked -- in the viewport, or by numbers and ranges: what "
             "Blast deletes, a wrangle runs over, the cloth pins by. Ctrl+G in the viewport makes one of what "
             "is picked there.",
             in,
             {text("name", "Name", "Group", "group1", "What it is called."),
              elements("Group", "Points, or primitives."),
              text("pattern", "Elements", "Group", "",
                   "Which: numbers and ranges -- 0-9 12 20-30 -- edges by their points -- p3-4 -- other groups "
                   "by name, * for all; ^ before one takes it away.")});
    geometry("edit", "Edit", "edit",
             "Moves, turns and sizes the points picked in the viewport -- or those of the primitives picked -- "
             "about the pivot, as the handle does: what dragging what is picked in the viewport makes -- W, "
             "E, R. Soft Radius takes the points round them along, less the further they are -- O in the "
             "viewport shows how much each takes.",
             in,
             {text("group", "Elements", "Edit", "",
                   "Which: numbers and ranges -- 0-9 12 -- edges -- p3-4 -- groups by name, * for all; ^ before "
                   "one takes it away."),
              elements("Edit", "Points, or the points of primitives."),
              vec("t", "Translate", "Edit", Vec3(), -5.0f, 5.0f, "m", "How far they move."),
              vec("r", "Rotate", "Edit", Vec3(), -180.0f, 180.0f, "\xc2\xb0", "Degrees about x, then y, then z, about the pivot."),
              vec("s", "Scale", "Edit", Vec3(1.0f, 1.0f, 1.0f), 0.0f, 5.0f, "", "How much larger along x, y, z, about the pivot."),
              vec("p", "Pivot", "Edit", Vec3(), -5.0f, 5.0f, "m", "What they turn and size about: the middle of the selection."),
              {"soft", "Soft Radius", "Edit", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "m",
               "How far round the selection points go along: all the way at it, not at all this far away. 0: "
               "only those picked. [ ] in the viewport with O on, or the wheel while dragging."},
              {"metric", "Distance", "Edit", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How far a point is from the selection: straight through space, or along the surface -- through "
               "its edges, so that a sheet lying over another, or a piece near but not joined, stays where it is.",
               {"space", "surface"}, {"Space", "Along the Surface"}},
              {"falloff", "Falloff", "Edit", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 4.0f, "",
               "How the share of the move goes from all of it at the selection to none at Soft Radius: Smooth, a "
               "hill; Linear, a cone; Sharp, a spike; Sphere, a dome; Constant, all of it as far as the radius.",
               {"smooth", "linear", "sharp", "sphere", "constant"}, {"Smooth", "Linear", "Sharp", "Sphere", "Constant"}}},
             {"t", "r", nullptr, "s", nullptr, nullptr, "p"});
    geometry("attribute_paint", "Attribute Paint", "attribpaint",
             "A number painted onto the points with the viewport's brush -- where the cloth is pinned (pin), "
             "how soon it tears (tear), how heavy it is (mass). The strokes are places, not point numbers: "
             "made finer, the geometry keeps its paint. P in the viewport paints on what is shown.",
             in,
             {text("name", "Attribute", "Paint", "pin", "What it paints: @name in a wrangle."),
              {"value", "Value", "Paint", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "",
               "What the brush lays on. Ctrl held, it lays on Erase Value."},
              {"erase", "Erase Value", "Paint", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "",
               "What the brush lays on with Ctrl held."},
              {"radius", "Radius", "Paint", K::Float, {0.15f, 0.0f, 0.0f}, 0.01f, 1.0f, 1e-4f, kBig, "m",
               "How big the brush is: [ and ] in the viewport, or Shift+wheel."},
              {"strength", "Strength", "Paint", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How much of the value a dab lays on at its middle."},
              {"default", "Default", "Paint", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "",
               "Where the points start when they have no such attribute."},
              {"strokes", "Strokes", "Paint", K::Data, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
               "The dabs painted, in order."}});
    geometry("sculpt", "Sculpt", "sculpt",
             "The surface shaped with the viewport's brush: pushed out and pulled in, smoothed, grabbed and "
             "moved, flattened -- a hill in a terrain, a dent in a car, a fold in a sheet. The dabs are places, "
             "each on the surface as those before it left it: made finer, the geometry keeps its shape. U in "
             "the viewport sculpts what is shown.",
             in,
             {{"tool", "Tool", "Sculpt", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 3.0f, "",
               "What the brush does next: Push / Pull out along the surface's normal (Ctrl held: in); Smooth "
               "(also Shift held); Grab, what is under it moved with the mouse; Flatten onto the plane where "
               "the brush is.",
               {"push", "smooth", "grab", "flatten"}, {"Push / Pull", "Smooth", "Grab", "Flatten"}},
              {"radius", "Radius", "Sculpt", K::Float, {0.2f, 0.0f, 0.0f}, 0.01f, 2.0f, 1e-4f, kBig, "m",
               "How big the brush is: [ and ] in the viewport, or Shift+wheel."},
              {"strength", "Strength", "Sculpt", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 4.0f, "",
               "How much a dab does at its middle: Push moves out a fifth of the radius for 1; Smooth and "
               "Flatten, the share of the way."},
              {"falloff", "Falloff", "Sculpt", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 4.0f, "",
               "How a dab does less towards its edge: Smooth, Linear, Sharp, Sphere, Constant. Of all the dabs.",
               {"smooth", "linear", "sharp", "sphere", "constant"}, {"Smooth", "Linear", "Sharp", "Sphere", "Constant"}},
              {"strokes", "Strokes", "Sculpt", K::Data, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
               "The dabs, in order."}});
    {
        // One node of the core, three ways in: over the points, the
        // primitives, or once over the whole geometry.
        auto wrangle = [&](const char* name, const char* label, int runOver, const char* help) {
            ParamDef snippet{"snippet", "Snippet", "Wrangle", K::Code, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
                             "What runs for every element, in the wrangle language (like VEX): variables, if, for, "
                             "functions; @P, @Cd, i@id, s@name; point(1, \"P\", @ptnum), nearpoints(0, @P, 0.5), "
                             "addpoint(0, @P), removepoint(0, @ptnum); ch(\"amount\") makes a parameter of it. An "
                             "attribute written that is not there is made. See docs/wrangle.md."};
            ParamDef over{"runover", "Run Over", "Wrangle", K::Choice, {static_cast<float>(runOver), 0.0f, 0.0f}, 0.0f, 3.0f,
                          0.0f, 3.0f, "", "What the snippet runs for: every point, primitive or vertex, or once for the "
                                          "whole geometry (Detail) -- where making geometry is at home.",
                          {"points", "primitives", "vertices", "detail"}, {"Points", "Primitives", "Vertices", "Detail"}};
            geometry(name, label, "attribwrangle", help,
                     {{"geometry", "Geometry", PinType::Geometry}, {"input1", "Input 1", PinType::Geometry},
                      {"input2", "Input 2", PinType::Geometry}, {"input3", "Input 3", PinType::Geometry}},
                     {snippet, over,
                      text("group", "Group", "Wrangle", "",
                           "Only these elements run: a group's name, or numbers and ranges -- 0-9 12, edges p3-4 "
                           "-- as Tab in the viewport writes them for what is picked; empty: all of them.")});
        };
        wrangle("point_wrangle", "Point Wrangle", 0,
                "Runs a snippet for every point: move them, colour them, read their neighbours, make and delete "
                "geometry. Reading @Time or $F, it changes every frame.");
        wrangle("primitive_wrangle", "Primitive Wrangle", 1,
                "Runs a snippet for every primitive: @P is its middle, primpoints() its corners.");
        wrangle("detail_wrangle", "Detail Wrangle", 3,
                "Runs a snippet once for the whole geometry: build points and polygons with addpoint() and "
                "addprim(), sum up, set detail attributes.");
    }
    geometry("normal", "Normal", "normal",
             "Point normals, N: the faces round each point, the larger ones counting more. The viewport "
             "shades by them; Copy to Points turns copies up along them.",
             in, {});
    geometry("scatter", "Scatter", "scatter",
             "Points over the surface, as many to a square metre everywhere; each with the normal of its face "
             "and the attributes of the corners round it, blended. Rules keep them off where they do not "
             "belong -- too steep, painted out, too near one another: grass on a meadow, trees in a wood.",
             in,
             {{"mode", "Mode", "Scatter", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Count: that many are tried, all over. Density: as many to a square metre -- more area, more "
               "points.",
               {"count", "density"}, {"Count", "Density"}},
              {"count", "Count", "Scatter", K::Int, {1000.0f, 0.0f, 0.0f}, 1.0f, 20000.0f, 0.0f, 1e7f, "",
               "How many points."},
              {"density", "Density", "Scatter", K::Float, {10.0f, 0.0f, 0.0f}, 0.0f, 200.0f, 0.0f, kBig, "1/m\xc2\xb2",
               "How many to a square metre, in Density mode."},
              seed("Scatter", "Another number, other places."),
              text("densityattribute", "Density Attribute", "Rules", "",
                   "A point attribute of the input, 0 to 1 -- painted, or a wrangle's -- that says what share of "
                   "the points to keep there: 0 none, 1 all. Empty: all."),
              {"maxslope", "Max Slope", "Rules", K::Float, {180.0f, 0.0f, 0.0f}, 0.0f, 180.0f, 0.0f, 180.0f, "\xc2\xb0",
               "None where the face leans more than this from level: 30 keeps grass off a cliff, 90 off "
               "overhangs, 180 anywhere."},
              {"mindistance", "Min Distance", "Rules", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "m",
               "None nearer than this to one before it: trees that keep their distance. 0: as they fall."}});
    t.push_back({"copy_to_points", "Copy to Points", "Geometry",
                 "A copy of Geometry on every point of Points: moved there, sized by the point's pscale, turned "
                 "by its orient -- a quaternion, as RBD Pieces gives the grit -- else its +y turned to the point's "
                 "N. The points' other attributes go onto their copy -- their Cd in place of its colours, their "
                 "tint multiplying them. Instance: the points themselves, each standing for its copy, the "
                 "geometry held once -- a forest of a few trees, a meadow of a few clumps.",
                 {{"geometry", "Geometry", PinType::Geometry}, {"points", "Points", PinType::Geometry}}, out,
                 {{"scale", "Scale", "Copy", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "",
                   "Every copy this much larger, on top of pscale."},
                  {"align", "Align to N", "Copy", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "Turn each copy's +y to its point's normal."},
                  {"instance", "Instance", "Copy", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "Not copies: the points, each standing for what would be copied onto it (its instance), "
                   "that held once -- the renderer draws it instanced, Unpack and the writers make the copies. "
                   "Millions of blades of grass cost what their points do."},
                  text("pieceattribute", "Piece Attribute", "Copy", "",
                       "A primitive attribute of Geometry -- an integer or a text: each of its values is a piece, "
                       "and each point gets the piece of its own value of it (a point without it, one in turn). "
                       "Variants: eight clumps of grass, one on each point. Empty: all of it on every point.")},
                 1});
    t.back().core = "copytopoints";
    geometry("unpack", "Unpack", "unpack",
             "What the points stand for (instances: Copy to Points with Instance, Grass, Tree) made copies of "
             "-- geometry every node can change. What is no instance stays as it is.",
             in, {});
    geometry("null", "Null", "null", "What comes in, unchanged: a name to point at, an end to display.", in, {});
    // What changes the mesh itself.
    geometry("connectivity", "Connectivity", "connectivity",
             "Which piece each primitive -- or point -- is in: those that share points are one. An integer "
             "attribute, the pieces numbered from 0: what a For-Each goes over, piece by piece.",
             in,
             {text("attribute", "Attribute", "Connectivity", "class", "What the number is called."),
              {"class", "Class", "Connectivity", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "On the primitives, or on the points.", {"primitive", "point"}, {"Primitive", "Point"}}});
    geometry("fuse", "Fuse", "fuse",
             "Points nearer each other than Distance made one, at their middle -- the seams of pieces merged "
             "closed. What folds to nothing goes.",
             in,
             {{"distance", "Distance", "Fuse", K::Float, {0.001f, 0.0f, 0.0f}, 0.0f, 0.1f, 0.0f, kBig, "m",
               "How near two points must be to become one."}});
    geometry("polyextrude", "PolyExtrude", "polyextrude",
             "Each face pushed out along its normal, a wall along each of its edges -- inward for a window, "
             "outward for a ledge; Inset shrinks it first. The faces moved are in the group Front Group, the "
             "walls in Side Group.",
             in,
             {{"distance", "Distance", "Extrude", K::Float, {0.2f, 0.0f, 0.0f}, -2.0f, 2.0f, -kBig, kBig, "m",
               "How far out along the face's normal; less than 0, in."},
              {"inset", "Inset", "Extrude", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "m",
               "How far each edge moves in across the face first."},
              text("group", "Group", "Extrude", "",
                   "Only these faces: a group's name, or numbers and ranges -- 0-9 12 -- as Tab in the viewport "
                   "writes them for the faces picked; empty: every face."),
              {"outputback", "Output Back", "Extrude", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Keep the face where it was, turned round: a closed solid."},
              text("frontgroup", "Front Group", "Groups", "extrudeFront", "The faces moved; empty: no group."),
              text("sidegroup", "Side Group", "Groups", "extrudeSide", "The walls; empty: no group.")});
    geometry("subdivide", "Subdivide", "subdivide",
             "Smoother: each face cut into quads, the points moved to round the surface off (Catmull-Clark). "
             "Open edges keep their line; the attributes of the points go with them.",
             in,
             {{"iterations", "Depth", "Subdivide", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 6.0f, "",
               "How many times: each makes four times the faces."}});
    geometry("clip", "Clip", "clip",
             "What is on one side of a plane: the faces it cuts are cut along it and, with Cap, closed with "
             "new faces there -- in the group Cap Group.",
             in,
             {vec("origin", "Origin", "Plane", Vec3(0.0f, 0.5f, 0.0f), -5.0f, 5.0f, "m", "A point of the plane."),
              vec("dir", "Direction", "Plane", Vec3(0.0f, 1.0f, 0.0f), -1.0f, 1.0f, "",
                  "Square to the plane: the side kept is the side it points to."),
              {"keep", "Keep", "Plane", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The side Direction points to, or the other.", {"above", "below"}, {"Above", "Below"}},
              {"cap", "Cap", "Plane", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "Close a closed mesh again where it was cut."},
              text("capgroup", "Cap Group", "Plane", "cut", "The faces that close the cut; empty: no group.")},
             {"origin", nullptr, "dir", nullptr, nullptr, nullptr});
    geometry("attribute_transfer", "Attribute Transfer", "attribtransfer",
             "Point attributes of Source onto the points near them: within Distance, the weighted mean of the "
             "points there; further, fading out over Blend Width.",
             {{"geometry", "Geometry", PinType::Geometry}, {"source", "Source", PinType::Geometry}},
             {text("attributes", "Attributes", "Transfer", "Cd", "Which: names with spaces between; * for all."),
              {"distance", "Distance", "Transfer", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "m",
               "Within this, a point takes the mean of the points of Source near it."},
              {"blend", "Blend Width", "Transfer", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "m",
               "Beyond Distance, over this much further, the nearest one's fading out."}});
    geometry("voronoi_fracture", "Voronoi Fracture", "voronoifracture",
             "A closed mesh broken into pieces: the cells of points -- those linked into Points, or Count of "
             "them at random inside it -- each the part of it nearer its point than any other, closed where it "
             "was cut. The pieces carry piece, their number; the cut faces are in the group Inside Group.",
             {{"geometry", "Geometry", PinType::Geometry}, {"points", "Points", PinType::Geometry}},
             {{"count", "Count", "Fracture", K::Int, {20.0f, 0.0f, 0.0f}, 1.0f, 200.0f, 0.0f, 10000.0f, "",
               "How many pieces, when no points come in: points at random inside the mesh."},
              {"seed", "Seed", "Fracture", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "",
               "Another number: the points elsewhere."},
              text("attribute", "Piece Attribute", "Fracture", "piece", "What each piece's number is called."),
              text("insidegroup", "Inside Group", "Fracture", "inside", "The faces made where it was cut.")});
    geometry("concrete_fracture", "Concrete Fracture", "concretefracture",
             "A closed mesh broken as concrete breaks: chunks of every size -- the smallest round Impact -- "
             "corners chipped off as flat spalls, the faces of the cracks rough, the same on both sides of each so "
             "the pieces still fit. The plain cut stays in the point attribute proxy: the RBD Solver simulates "
             "the pieces as the proxy has them and draws them rough. The pieces carry piece, the cut faces are "
             "in the group Inside Group, the spalls have chip 1.",
             {{"geometry", "Geometry", PinType::Geometry}, {"points", "Points", PinType::Geometry}},
             {{"count", "Count", "Fracture", K::Int, {60.0f, 0.0f, 0.0f}, 1.0f, 300.0f, 0.0f, 10000.0f, "",
               "How many pieces -- before the spalls -- when no points come in: at random inside the mesh."},
              {"seed", "Seed", "Fracture", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "",
               "Another number: the pieces elsewhere."},
              {"uneven", "Uneven", "Fracture", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How unlike the pieces are: 0 all about as big, 1 big chunks beside crumbs."},
              {"impact", "Impact", "Fracture", K::Vector, {0.0f, 1.0f, 0.0f}, -10.0f, 10.0f, -kBig, kBig, "m",
               "Where it is struck: with Focus, the pieces are smallest there."},
              {"focus", "Focus", "Fracture", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How many more pieces round Impact: 0 none more."},
              {"reach", "Reach", "Fracture", K::Float, {1.0f, 0.0f, 0.0f}, 0.05f, 5.0f, 0.001f, kBig, "m",
               "How far from Impact the pieces are smaller."},
              {"chips", "Chips", "Spalls", K::Float, {0.2f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The share of the pieces' corners chipped off: flat spalls, small pieces of their own glued on by "
               "their face -- the first to fly off in a knock."},
              {"chipsize", "Chip Size", "Spalls", K::Float, {0.06f, 0.0f, 0.0f}, 0.0f, 0.3f, 0.0f, kBig, "m",
               "How deep a spall is at most."},
              {"rough", "Rough", "Cracks", K::Float, {0.02f, 0.0f, 0.0f}, 0.0f, 0.1f, 0.0f, kBig, "m",
               "How far the faces of the cracks go in and out, each way: 0 flat cuts."},
              {"roughscale", "Rough Scale", "Cracks", K::Float, {0.3f, 0.0f, 0.0f}, 0.02f, 2.0f, 0.001f, kBig, "m",
               "How far apart their bumps are; finer ones sit on them."},
              {"detail", "Detail", "Cracks", K::Float, {0.03f, 0.0f, 0.0f}, 0.005f, 0.2f, 0.001f, kBig, "m",
               "How long the triangles of a rough face are at most: smaller is finer, and heavier to draw."},
              text("attribute", "Piece Attribute", "Fracture", "piece", "What each piece's number is called."),
              text("insidegroup", "Inside Group", "Fracture", "inside", "The faces made where it was cut.")});
    geometry("rbd_cluster", "RBD Cluster", "rbdcluster",
             "The pieces of a fracture grouped into Count chunks of about one size -- each piece in the chunk "
             "whose middle is nearest -- the glue between the pieces of one chunk Strength times as strong as "
             "between chunks. The RBD Solver breaks a thing into chunks first, and a chunk breaks up only where a "
             "hard knock lands on it -- when it lands. The pieces carry cluster (1 and up) and clusterglue.",
             in,
             {{"count", "Count", "Cluster", K::Int, {8.0f, 0.0f, 0.0f}, 1.0f, 50.0f, 1.0f, 100000.0f, "",
               "How many chunks."},
              {"seed", "Seed", "Cluster", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "",
               "Another number: the chunks elsewhere."},
              {"strength", "Strength", "Cluster", K::Float, {5.0f, 0.0f, 0.0f}, 1.0f, 20.0f, 0.0f, 1e6f, "",
               "How many times as strong the glue inside a chunk is as the glue between chunks: 1 as strong -- "
               "no chunks; the higher, the harder a knock must be to break a chunk up."},
              text("attribute", "Piece Attribute", "Cluster", "piece", "What says which piece a primitive is of.")});
    geometry("rbd_constraints", "RBD Constraints", "rbd_constraints",
             "The glue between the pieces as geometry -- Houdini's constraint network: a point at the middle of "
             "each piece, with the piece it is; a line for each joint, where two pieces touch face to face, with "
             "strength -- a share of the RBD Solver's Glue: 1 as it holds, 0.1 a tenth, 0 nothing -- area (m\xc2\xb2 "
             "of the faces) and Cd: green as the Glue holds, yellow weaker, blue stronger. Weaken it, delete lines, "
             "draw new ones with the geometry nodes -- a wrangle over where the lines are -- and link it into the "
             "solver's Constraints: its lines are then the joints. The pieces' glue, cluster and clusterglue make "
             "the strength here.",
             in,
             {text("attribute", "Piece Attribute", "Pieces", "piece",
                   "What says which piece a primitive is of -- as the RBD Solver's Piece Attribute.")});
    geometry("glass_fracture", "Glass Fracture", "glassfracture",
             "A pane of glass broken as glass breaks where it is struck: cracks straight out from Impact and "
             "cracks round it from one to the next -- a spider's web, slivers at the middle, shards growing wider "
             "further out, a crack branching off where a shard grows too wide. The input is the pane -- any "
             "outline, lying any way -- the shards are cut square through it, closed, with piece, glass (1 the "
             "pane's faces, 2 the cracks') and Cd the tint: link them into an RBD Solver; they are drawn as glass.",
             in,
             {vec("impact", "Impact", "Glass", Vec3(0.0f, 1.0f, 0.0f), -5.0f, 5.0f, "m",
                  "Where it is struck: the middle of the web, on the pane."),
              {"radials", "Radials", "Glass", K::Int, {14.0f, 0.0f, 0.0f}, 3.0f, 40.0f, 3.0f, 256.0f, "",
               "How many cracks run out from Impact."},
              {"first", "First Ring", "Glass", K::Float, {0.04f, 0.0f, 0.0f}, 0.005f, 0.3f, 0.0001f, kBig, "m",
               "How far from Impact the first crack round it runs: the slivers at the middle."},
              {"growth", "Growth", "Glass", K::Float, {1.45f, 0.0f, 0.0f}, 1.1f, 3.0f, 1.05f, 100.0f, "",
               "How many times further out each next crack round it runs: the shards grow as they go out."},
              {"rings", "Rings", "Glass", K::Int, {10.0f, 0.0f, 0.0f}, 1.0f, 20.0f, 1.0f, 200.0f, "",
               "How many cracks round it at most; past them the shards run to the edge of the pane."},
              {"jitter", "Jitter", "Glass", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How unevenly the cracks run: 0 a perfect web; the rings then do not line up across a crack."},
              {"split", "Split", "Glass", K::Float, {1.1f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 100.0f, "",
               "A crack branches off where a shard is this many times as wide as it is deep. 0: never."},
              {"seed", "Seed", "Glass", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "",
               "Another number: other cracks."},
              {"tint", "Tint", "Glass", K::Color, {0.82f, 0.9f, 0.88f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour the glass gives what is seen through it -- the pale green of window glass -- as Cd."},
              text("attribute", "Piece Attribute", "Glass", "piece", "What each shard's number is called."),
              text("insidegroup", "Inside Group", "Glass", "inside", "The faces of the cracks.")},
             {"impact", nullptr, nullptr, nullptr, nullptr, nullptr});
    geometry("brick_wall", "Brick Wall", "brickwall",
             "A wall laid of bricks as a bricklayer lays it: course on course in a bond, each brick on its bed of "
             "mortar with a joint at its end, filling the input -- a solid of any outline, openings and all, "
             "standing as it stands -- with plaster on its faces where there is some. Each brick is a piece with "
             "its mortar -- piece, Cd -- Broken of them cut in two, the halves one cluster held Strength times "
             "as hard as the mortar. Link them into an RBD Solver: its Glue is the mortar's.",
             in,
             {{"bond", "Bond", "Bricks", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 4.0f, "",
               "Where the bricks of each course break joint with those below. Stretcher: each course half a "
               "brick on. English: a course of headers -- bricks across the wall -- then one of stretchers. "
               "Flemish: header and stretcher in turn in every course. Stack: joint over joint. Auto: Stretcher "
               "for a wall one brick's width thick, else English.",
               {"auto", "stretcher", "english", "flemish", "stack"}, {"Auto", "Stretcher", "English", "Flemish", "Stack"}},
              {"length", "Length", "Bricks", K::Float, {0.25f, 0.0f, 0.0f}, 0.1f, 0.6f, 0.01f, kBig, "m",
               "How long a brick is: 250 mm (290 the Czech full brick). A header is half of it and its joint."},
              {"width", "Width", "Bricks", K::Float, {0.12f, 0.0f, 0.0f}, 0.05f, 0.4f, 0.005f, kBig, "m",
               "How wide a brick is: the wall has as many leaves of them across as fit its thickness."},
              {"height", "Height", "Bricks", K::Float, {0.065f, 0.0f, 0.0f}, 0.04f, 0.3f, 0.005f, kBig, "m",
               "How high a brick is -- a course with its bed; the beds give a little so that the courses fill "
               "the wall's height."},
              {"joint", "Joint", "Bricks", K::Float, {0.01f, 0.0f, 0.0f}, 0.0f, 0.03f, 0.0f, 0.1f, "m",
               "How thick the mortar is between bricks."},
              vec("front", "Front", "Bricks", Vec3(0.0f, 0.0f, 1.0f), -1.0f, 1.0f, "",
                  "Which way the wall's front faces: the face that way is the front."),
              {"plaster", "Plaster", "Plaster", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 0.05f, 0.0f, 0.2f, "m",
               "How thick the plaster on the wall's faces is, part of the wall's thickness. 0: bare brick."},
              {"plastersides", "Plaster Sides", "Plaster", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
               "Which faces are plastered.", {"both", "front", "back"}, {"Both", "Front", "Back"}},
              {"color", "Brick Color", "Look", K::Color, {0.46f, 0.18f, 0.11f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour of the bricks, as Cd."},
              {"variation", "Variation", "Look", K::Float, {0.35f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "How much the bricks differ in shade -- lighter, darker, warmer -- one from the next."},
              {"mortar", "Mortar Color", "Look", K::Color, {0.52f, 0.5f, 0.47f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour of the mortar."},
              {"plastercolor", "Plaster Color", "Look", K::Color, {0.86f, 0.83f, 0.77f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The colour of the plaster."},
              {"broken", "Broken", "Break", K::Float, {0.3f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The share of the bricks -- of those half as long again as they are wide -- that can break: cut "
               "in two across, the halves held together Strength times as hard as the mortar holds a brick -- a "
               "hard knock breaks them. 0: none breaks."},
              {"strength", "Strength", "Break", K::Float, {8.0f, 0.0f, 0.0f}, 1.0f, 50.0f, 0.0f, kBig, "",
               "How many times harder a brick holds than the mortar round it (the halves' clusterglue)."},
              {"seed", "Seed", "Break", K::Int, {1.0f, 0.0f, 0.0f}, 0.0f, 100.0f, 0.0f, 1e6f, "",
               "Another number: other shades, other bricks broken."},
              text("attribute", "Piece Attribute", "Bricks", "piece", "What each brick's number is called.")});
    geometry("rebar", "Rebar", "rebar",
             "Steel bars inside a block of concrete, as they are laid before it is poured: a mesh both ways near "
             "each face of a wall or a slab, or bars along a beam or a column with stirrups round them. The block "
             "is the input's box, turned as it lies -- the block itself, or the pieces of its fracture. Link it "
             "into the RBD Solver's Rebar: the bars hold the pieces once the concrete cracks, bend, pull out of "
             "small pieces and tear. Open polylines, width their diameter.",
             in,
             {{"layout", "Layout", "Rebar", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 2.0f, "",
               "Auto: a mesh when the block's thinnest side is under half the next -- a wall, a slab -- else a "
               "cage -- a beam, a column.",
               {"auto", "mesh", "cage"}, {"Auto", "Mesh", "Cage"}},
              {"spacing", "Spacing", "Rebar", K::Float, {0.2f, 0.0f, 0.0f}, 0.05f, 1.0f, 0.01f, kBig, "m",
               "How far apart the bars are at most -- and the stirrups along a cage."},
              {"cover", "Cover", "Rebar", K::Float, {0.035f, 0.0f, 0.0f}, 0.0f, 0.1f, 0.0f, kBig, "m",
               "The concrete over the bars, and past their ends."},
              {"diameter", "Diameter", "Rebar", K::Float, {0.012f, 0.0f, 0.0f}, 0.006f, 0.04f, 0.0001f, kBig, "m",
               "How thick a bar is: 8 to 32 mm."},
              {"layers", "Layers", "Rebar", K::Int, {2.0f, 0.0f, 0.0f}, 1.0f, 2.0f, 1.0f, 2.0f, "",
               "For a mesh: 2 a layer near each face, 1 one in the middle."},
              {"stirrup", "Stirrup Diameter", "Rebar", K::Float, {0.008f, 0.0f, 0.0f}, 0.0f, 0.02f, 0.0f, kBig, "m",
               "How thick the stirrups round a cage are. 0: none."}});
    geometry("convert_volume", "Convert Volume", "convertvolume",
             "The surface of a volume as polygons: where its values cross Iso, a closed mesh of quads turned "
             "outward, with normals N -- closed where the volume ends. Smoke from a Gas Volume, a distance "
             "field, anything that is a volume.",
             in,
             {text("volume", "Volume", "Convert", "", "Which volume, by name; empty: the first."),
              {"iso", "Iso", "Convert", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "",
               "The value where the surface is."},
              {"inside", "Inside", "Convert", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "What is inside: values above Iso -- a density, smoke -- or below it -- a distance, below 0 "
               "inside.",
               {"above", "below"}, {"Above Iso", "Below Iso"}}});
    // Loops.
    geometry("foreach_begin", "For-Each Begin", "foreachbegin",
             "Where a loop begins: the nodes after it, up to a For-Each End, run once for each piece of what "
             "comes in -- by an attribute (the class a Connectivity gives), each primitive, each point -- or "
             "Count times; Feedback runs them Count times, each on what the time before made. Cooked alone it "
             "gives the first piece. A piece carries detail attributes: iteration, numiterations, value.",
             in,
             {{"method", "Method", "Loop", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 4.0f, 0.0f, 4.0f, "",
               "What each time gets.", {"pieces", "primitives", "points", "count", "feedback"},
               {"Pieces", "Primitives", "Points", "Count", "Feedback"}},
              text("attribute", "Piece Attribute", "Loop", "class",
                   "For Pieces: the primitives (or points) of one value of it are one piece."),
              {"count", "Count", "Loop", K::Int, {4.0f, 0.0f, 0.0f}, 1.0f, 20.0f, 0.0f, 100000.0f, "",
               "For Count and Feedback: how many times."}});
    geometry("foreach_end", "For-Each End", "foreachend",
             "Where a loop ends: what the nodes from its For-Each Begin made of each piece, put together -- "
             "for Feedback, what the last time made.",
             in,
             {text("begin", "Begin", "Loop", "", "The For-Each Begin it closes, by name; empty: the nearest upstream.")});
    geometry("asset_input", "Asset Input", "asset_input",
             "What comes into a digital asset: inside its network, the geometry linked into the asset's input "
             "Index. Outside an asset, nothing.",
             {},
             {{"index", "Index", "Input", K::Int, {0.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 3.0f, "",
               "Which input of the asset, from 0."}});
    // What the simulations make, as geometry: at the frame shown.
    geometry("liquid_points", "Liquid Points", "liquid_points",
             "The particles of a Liquid Solver at the frame: points with their velocity v, foam and id -- the "
             "same number from frame to frame -- to colour, to copy drops onto, to export.",
             {{"liquid", "Liquid", PinType::Liquid}}, {});
    geometry("liquid_surface", "Liquid Surface", "liquid_surface",
             "The water of a Liquid Solver at the frame as a surface: a closed mesh round it, turned outward, "
             "with normals N, its velocity v -- what a renderer blurs it by -- and foam. What a renderer renders "
             "the water from; closed against the floor and the walls too, to bend light through it.",
             {{"liquid", "Liquid", PinType::Liquid}},
             {{"ripples", "Ripples", "Water", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The rain's ripples on it: its top raised and tilted as they are."}});
    geometry("rain_points", "Rain Points", "rain_points",
             "The drops of a Rain at the frame: points with their velocity v and id -- the same number from "
             "frame to frame.",
             {{"rain", "Rain", PinType::Rain}},
             {{"droplets", "Droplets", "Rain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The droplets of the splashes too, with droplet 1 and ids from 2^30."}});
    geometry("gas_volume", "Gas Volume", "gas_volume",
             "The gas of a Pyro Solver at the frame, as volumes: density (smoke), temperature and flame.",
             {{"gas", "Gas", PinType::Gas}}, {});
    geometry("rbd_pieces", "RBD Pieces", "rbd_pieces",
             "The pieces of an RBD Solver at the frame, where they have fallen: moved and turned, with the "
             "velocity v of each point -- to process further, to export, to show otherwise. The solver's look "
             "draws them already.",
             {{"rigid", "Rigid", PinType::Rigid}},
             {{"grit", "Grit", "Rigid", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The grit too: points as wide as a bit is (pscale), with its velocity v and id -- the same number "
               "from frame to frame."},
              {"rebar", "Rebar", "Rigid", K::Toggle, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "The bars in the pieces too, where the pieces have taken them: an open polyline for each stretch "
               "of a bar in one piece, torn apart at a tear, with width -- its diameter -- and v."},
              {"output", "Output", "Rigid", K::Choice, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
               "What it gives. Pieces: the pieces. Constraints: the glue between them as a network, as RBD "
               "Constraints makes it, where the pieces are -- a point at each one's middle, with v; a line for "
               "each joint that held, broken 1 where it broke, at time (seconds; -1 where it holds), at where "
               "its faces touched, red where broken.",
               {"pieces", "constraints"}, {"Pieces", "Constraints"}}});

    geometry("cloth_geometry", "Cloth Geometry", "cloth_geometry",
             "The cloth of a Cloth Solver at the frame, where it has gone: its points moved, with the velocity v "
             "and smooth normals N of each -- to process further, to export, to show otherwise. The solver's "
             "look draws it already.",
             {{"cloth", "Cloth", PinType::Cloth}}, {});

    // --- objects ----------------------------------------------------------------------
    t.push_back({"object", "Object", "Objects",
                 "A solid in the scene: a ball, a box, a column, a cone, a ring. It is drawn and casts shadows; "
                 "linked into a solver's Colliders, what the solver simulates goes round it. Move, turn and size "
                 "it in the viewport: W, E, R. Geometry linked into Shape is its shape instead, where it is.",
                 {{"shape", "Shape", PinType::Geometry}},
                 {{"collider", "Collider", PinType::Collider}},
                 objectParams(),
                 1});
    t.back().handles = {"center", "rotation", nullptr, "size", nullptr, nullptr};

    // --- sources --------------------------------------------------------------------
    t.push_back({"pyro_source", "Pyro Source", "Sources",
                 "A shape that gives off fuel, smoke and heat, and pushes the gas its way. Fuel makes fire; "
                 "smoke and heat without fuel make a column of smoke. A ball for a campfire, a box for a burning "
                 "log or a vent, a ring for a gas burner. Geometry linked into Shape is its shape instead: "
                 "polygons, or points -- a ball round each.",
                 {{"shape", "Shape", PinType::Geometry}},
                 {{"source", "Source", PinType::Source}},
                 pyroSourceParams(),
                 2});
    t.back().handles = {"center", "rotation", nullptr, "size", nullptr, nullptr};
    t.push_back({"water_source", "Water Source", "Sources",
                 "A shape the water comes from: filled once -- a block of water, a pool -- or pouring water out "
                 "at its velocity -- a hose, a fountain, a waterfall. Geometry linked into Shape is its shape "
                 "instead.",
                 {{"shape", "Shape", PinType::Geometry}},
                 {{"water", "Water", PinType::Water}},
                 waterSourceParams(),
                 1});
    t.back().handles = {"center", "rotation", nullptr, "size", nullptr, nullptr};

    // --- forces ---------------------------------------------------------------------
    t.push_back({"turbulence", "Turbulence", "Forces",
                 "Random whirls that change over time: what breaks a smooth plume into curls.",
                 {},
                 {{"force", "Force", PinType::Force}},
                 {{"strength", "Strength", "Force", K::Float, {3.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "m/s\xc2\xb2",
                   "How hard the whirls push."},
                  {"scale", "Scale", "Force", K::Float, {0.05f, 0.0f, 0.0f}, 0.01f, 0.5f, 0.005f, kBig, "m",
                   "Size of the whirls."},
                  {"speed", "Change", "Force", K::Float, {4.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                   "How many times a second the whirls change."},
                  mask(1),
                  seed("Force", "Another number, other whirls.")}});
    t.push_back({"wind", "Wind", "Forces",
                 "Air moving one way, steadily or in gusts: it bends a plume and carries the smoke off.",
                 {},
                 {{"force", "Force", PinType::Force}},
                 {{"direction", "Direction", "Wind", K::Vector, {1.0f, 0.0f, 0.0f}, -1.0f, 1.0f, -kBig, kBig, "",
                   "Where it blows."},
                  {"speed", "Speed", "Wind", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, kBig, "m/s",
                   "How fast it blows."},
                  {"strength", "Grip", "Wind", K::Float, {1.5f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "1/s",
                   "How quickly the gas takes on the wind's speed."},
                  {"gusts", "Gusts", "Wind", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "0 steady, 1 strongly gusting."},
                  mask(0),
                  seed("Wind", "Another number, other gusts.")}});
    t.back().handles = {nullptr, nullptr, "direction", nullptr, nullptr, nullptr};
    t.push_back({"vortex", "Vortex", "Forces",
                 "Turns the gas round an axis, and can carry it along the axis and draw it in: a whirl, a "
                 "fire tornado. It acts inside a cylinder about the axis.",
                 {},
                 {{"force", "Force", PinType::Force}},
                 {center("Shape", Vec3(0.0f, 0.75f, 0.0f), "A point on its axis: the middle of the cylinder."),
                  {"axis", "Axis", "Shape", K::Vector, {0.0f, 1.0f, 0.0f}, -1.0f, 1.0f, -kBig, kBig, "",
                   "The direction of its axis."},
                  {"radius", "Radius", "Shape", K::Float, {0.3f, 0.0f, 0.0f}, 0.02f, 1.0f, 0.005f, kBig, "m",
                   "How far from the axis it reaches."},
                  {"height", "Height", "Shape", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "m",
                   "Its length along the axis, about the center. 0: all the way through."},
                  {"speed", "Speed", "Motion", K::Float, {1.5f, 0.0f, 0.0f}, -5.0f, 5.0f, -kBig, kBig, "m/s",
                   "Speed round the axis, fastest halfway out. Below 0 it turns the other way."},
                  {"lift", "Lift", "Motion", K::Float, {0.0f, 0.0f, 0.0f}, -3.0f, 3.0f, -kBig, kBig, "m/s",
                   "Speed along the axis."},
                  {"suction", "Suction", "Motion", K::Float, {0.0f, 0.0f, 0.0f}, -3.0f, 3.0f, -kBig, kBig, "m/s",
                   "Speed in towards the axis, at the edge. Below 0 it throws the gas out."},
                  {"strength", "Grip", "Motion", K::Float, {3.0f, 0.0f, 0.0f}, 0.0f, 20.0f, 0.0f, kBig, "1/s",
                   "How quickly the gas takes on these speeds."},
                  mask(0)}});
    t.back().handles = {"center", nullptr, "axis", nullptr, "radius", "height"};
    t.push_back({"attractor", "Attractor", "Forces",
                 "Pulls the gas towards a point -- or, below 0, pushes it away -- harder the closer it is.",
                 {},
                 {{"force", "Force", PinType::Force}},
                 {center("Shape", Vec3(0.0f, 0.75f, 0.0f), "The point it pulls to."),
                  {"radius", "Radius", "Shape", K::Float, {0.5f, 0.0f, 0.0f}, 0.02f, 2.0f, 0.005f, kBig, "m",
                   "How far it reaches."},
                  {"strength", "Strength", "Force", K::Float, {2.0f, 0.0f, 0.0f}, -20.0f, 20.0f, -kBig, kBig,
                   "m/s\xc2\xb2", "How hard it pulls at the center. Below 0 it pushes away."},
                  mask(0)}});
    t.back().handles = {"center", nullptr, nullptr, nullptr, "radius", nullptr};
    t.push_back({"drag", "Drag", "Forces", "Slows the gas down: thick, calm air.",
                 {},
                 {{"force", "Force", PinType::Force}},
                 {{"strength", "Strength", "Force", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "1/s",
                   "How quickly the gas slows down."},
                  mask(0)}});

    // --- the solver -------------------------------------------------------------------
    t.push_back(
        {"pyro_solver", "Pyro Solver", "Simulation",
         "Simulates the gas in a box standing on the floor: the sources add to it, the forces move it, "
         "the colliders stand in its way; heat lifts it, fuel burns.",
         {{"sources", "Sources", PinType::Source, true},
          {"forces", "Forces", PinType::Force, true},
          {"colliders", "Colliders", PinType::Collider, true}},
         {{"gas", "Gas", PinType::Gas}},
         pyroSolverParams(false),
         2});
    t.push_back(
        {"liquid_solver", "Liquid Solver", "Simulation",
         "Simulates water in a box standing on the floor (FLIP): particles carry it, a grid keeps its volume. "
         "It falls, splashes, piles up and flows round the colliders; the forces push it about.",
         {{"sources", "Sources", PinType::Water, true},
          {"forces", "Forces", PinType::Force, true},
          {"colliders", "Colliders", PinType::Collider, true}},
         {{"liquid", "Liquid", PinType::Liquid}},
         {{"size", "Size", "Domain", K::Vector, {2.0f, 1.0f, 1.2f}, 0.1f, 5.0f, 0.1f, 1000.0f, "m",
           "Width, height and depth of the box the water lives in. It stands on the floor, centred."},
          {"resolution", "Resolution", "Domain", K::Int, {64.0f, 0.0f, 0.0f}, 16.0f, 192.0f, 16.0f, 256.0f, "",
           "Cells along the longest side; eight particles fill a cell. Twice as many: finer splashes, and "
           "eight times the work."},
          {"closed_sides", "Closed Sides", "Domain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "Walls round the four sides: a tank. Off, the water runs off the edges and is gone."},
          {"sparse", "Sparse", "Domain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "Work only where the water is -- in tiles of 8 cells a side, and those round them: the air above "
           "it and the empty part of the box cost nothing. Off: every tile, the same water to the bit, in "
           "more memory and time."},
          {"gravity", "Gravity", "Motion", K::Float, {9.81f, 0.0f, 0.0f}, 0.0f, 20.0f, -100.0f, 100.0f,
           "m/s\xc2\xb2", "How hard the water is pulled down."},
          {"flip", "Splash", "Motion", K::Float, {0.95f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "1: lively and splashy, every drop keeps its own speed (FLIP); 0: smooth and thick, slowing down "
           "(PIC). Most water is 0.9 to 0.98."},
          {"substeps", "Substeps", "Time", K::Int, {1.0f, 0.0f, 0.0f}, 1.0f, 8.0f, 1.0f, 16.0f, "",
           "At least this many steps a frame. Fast water takes more on its own: no drop crosses more than two "
           "cells a step."},
          seed("Time", "Another number: the drops of the sources at other places.")},
         1});

    t.push_back(
        {"rbd_solver", "RBD Solver", "Simulation",
         "Rigid bodies: the pieces of something broken -- a Voronoi Fracture's -- fall, knock into each other, "
         "into the floor and into the objects linked into Colliders (a keyed one is a wrecking ball), glued to "
         "the pieces they touch -- one body with them -- until a knock harder than Glue breaks them apart, "
         "puffing dust and throwing grit. Attributes of the pieces set them apart: density, v, w, active (0: "
         "it stays), glue, release -- the seconds when a charge breaks its joints -- with kick and vanish "
         "(blown to dust), crush (crushed to dust by a hard knock), and cluster with clusterglue -- RBD Cluster's "
         "chunks, glued stronger inside. Steel bars linked into Rebar -- a Rebar node's -- hold the pieces they "
         "run through once the glue breaks: they bend, pull out of small pieces and tear. A network linked into "
         "Constraints -- RBD Constraints', edited -- is the glue instead: its lines are the joints, as strong as "
         "their strength says. A guide linked into Guide -- the pieces, moved as the shot wants them to go -- "
         "leads them: they follow it until its time is up, their glue breaks or something stops them further "
         "from it than Reach; a piece's attribute guide says how much it leads that one. Link it into the "
         "Output's Looks: it is "
         "simulated and drawn. Its Collider into a Liquid, Pyro Solver or Rain: they go round the pieces; its "
         "Dust into a Pyro Solver's Sources: the dust is smoke, pushed out by the air the pieces squeeze out.",
         {{"pieces", "Pieces", PinType::Geometry},
          {"colliders", "Colliders", PinType::Collider, true},
          {"rebar", "Rebar", PinType::Geometry},
          {"constraints", "Constraints", PinType::Geometry},
          {"guide", "Guide", PinType::Geometry}},
         {{"look", "Look", PinType::Look},
          {"rigid", "Rigid", PinType::Rigid},
          {"collider", "Collider", PinType::Collider},
          {"dust", "Dust", PinType::Source}},
         {text("attribute", "Piece Attribute", "Pieces", "piece",
               "What says which piece a primitive is of -- a Voronoi Fracture's piece. Without it, what touches "
               "what is one piece."),
          {"density", "Density", "Physics", K::Float, {2000.0f, 0.0f, 0.0f}, 100.0f, 8000.0f, 1.0f, 1e6f,
           "kg/m\xc2\xb3", "How heavy a cubic metre is: 2400 concrete, 700 wood, 7800 steel."},
          {"friction", "Friction", "Physics", K::Float, {0.6f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 10.0f, "",
           "How hard pieces grip what they slide on: 0 ice, 1 rubber."},
          {"bounce", "Bounce", "Physics", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "How much of its speed a piece keeps when it knocks into something: 0 a thud, 1 a rubber ball."},
          {"gravity", "Gravity", "Physics", K::Float, {9.81f, 0.0f, 0.0f}, 0.0f, 20.0f, -100.0f, 100.0f,
           "m/s\xc2\xb2", "How hard the pieces are pulled down."},
          {"floor", "Floor", "Physics", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A floor at height 0 that the pieces land on. Off, they fall for ever."},
          {"glue", "Glue", "Glue", K::Float, {500.0f, 0.0f, 0.0f}, 0.0f, 5000.0f, 0.0f, 1e9f, "kPa",
           "How hard the faces where two pieces touch hold together: kilonewtons a square metre. Glued pieces "
           "move as one body; a knock harder than a joint holds breaks it for good, and Spread of it goes on "
           "to the joints beyond. 0: no glue -- the pieces fall apart at once. A piece's attribute glue makes "
           "its joints stronger or weaker; release breaks them at a time -- a charge."},
          {"spread", "Spread", "Glue", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "How much of a knock goes on through a joint to the pieces beyond: 0.5 half -- a hard knock breaks "
           "the glue far round where it lands -- 0 none: only the pieces it lands on come loose. Houdini's "
           "Propagate Rate."},
          {"rings", "Rings", "Glue", K::Int, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, 1000.0f, "",
           "How many rings of pieces round those a knock lands on it can break loose, however hard it is: 1 "
           "the pieces next to them, 2 the ones next to those... A keyed object is unstoppable -- it would "
           "break a whole wall at once -- with 1 or 2 it punches a hole. 0: as far as Spread carries it. "
           "Houdini's Propagate Iterations."},
          {"rebar_strength", "Steel Strength", "Rebar", K::Float, {500.0f, 0.0f, 0.0f}, 200.0f, 800.0f, 1.0f, 1e5f,
           "MPa",
           "How hard the steel of the bars holds before it yields: 500 for today's bars. A 12 mm bar holds some "
           "57 kN pulled; bent, it bends and stays bent. Pulled further, it stretches, and tears."},
          {"bond", "Bond", "Rebar", K::Float, {5.0f, 0.0f, 0.0f}, 0.0f, 15.0f, 0.0f, 1e4f, "MPa",
           "How hard the concrete grips a bar, along its surface: a piece the bar runs 20 cm through holds it "
           "with some 38 kN. On each side of a crack the pieces that hold the bar anchor it together; where "
           "that is less than the steel holds -- near the end of a bar, or of a torn one -- the bar slides out "
           "of them and the concrete falls off it, elsewhere the steel yields. 0: the bars hold nothing."},
          {"stretch", "Stretch", "Rebar", K::Float, {0.1f, 0.0f, 0.0f}, 0.0f, 0.5f, 0.0f, 100.0f, "",
           "How much longer a bar gets before it tears, as a share of what of it yields: 0.1 a tenth -- of the "
           "bar bare between two pieces and twenty times its diameter."},
          {"guide_strength", "Strength", "Guide", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "How hard the guide steers the pieces: 1 onto where it has them at every step, less a softer pull "
           "that lags behind, 0 not at all. They still knock into things. Keyed down, it hands them over to "
           "the simulation bit by bit."},
          {"guide_until", "Until", "Guide", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, 1e6f, "s",
           "Seconds after which the guide leads nothing: the pieces go their own way. 0: all along."},
          {"guide_reach", "Reach", "Guide", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, 1e6f, "m",
           "How far a body may be from where the guide has it -- stopped by the ground, or by what it hit -- "
           "before it goes its own way. 0: however far."},
          {"guide_let_go", "Let Go When Broken", "Guide", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A piece whose glue breaks goes its own way: what the guide throws down breaks up freely where it "
           "lands."},
          {"substeps", "Substeps", "Time", K::Int, {2.0f, 0.0f, 0.0f}, 1.0f, 8.0f, 1.0f, 16.0f, "",
           "Steps of the solver a frame: more for fast pieces and tall stacks, which then stand steadier."},
          {"rest", "Freeze at Rest", "Time", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A body that has come to rest -- gone nowhere for half a second, lying on the floor or on what "
           "lies still -- is frozen and costs nothing, until something comes at it faster than 1 m/s, water "
           "or the gas push it, a charge goes off in it or a keyed object reaches it; what lies on it wakes "
           "with it. Piles of rubble step many times faster. Off: every body is stepped to the end -- "
           "Houdini's Allow Deactivation off."},
          {"buoyancy", "Buoyancy", "Fluids", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 100.0f, "",
           "How much the water of the Liquid Solver holds the pieces up: 1 as much as the water they push aside "
           "weighs -- what is lighter than water (Density under 1000: wood) floats, rocking on the waves, the "
           "rest sinks slower. 0: not at all."},
          {"water_drag", "Water Drag", "Fluids", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 100.0f, "",
           "How hard the water carries the pieces and the grit along and slows them down: a flood sweeps them "
           "away, grit sinks slowly. 0: not at all."},
          {"air_drag", "Air Drag", "Fluids", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 100.0f, "",
           "How much of the Pyro Solver's flow carries the grit and the pieces: the dust cloud's wind, a blast. "
           "Heavy pieces barely feel it; grit rolls out with the dust. 0: none -- still air holds the grit back "
           "all the same."},
          {"dust", "Dust", "Dust", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, 1000.0f, "",
           "Smoke a joint gives off as it breaks, into the Pyro Solver its Dust is linked into."},
          {"impact_dust", "Impact Dust", "Dust", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, 1000.0f, "",
           "Smoke a hard knock gives off -- a piece landing, two pieces crashing together."},
          {"dust_size", "Puff Size", "Dust", K::Float, {0.3f, 0.0f, 0.0f}, 0.05f, 2.0f, 0.01f, 100.0f, "m",
           "How big a puff of dust is."},
          {"debris", "Debris", "Dust", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, 100.0f, "",
           "Grit a break or a knock throws out: small stones that fly out of the cracks, tumbling, knock into "
           "the pieces, the objects and the floor, bounce off and come to rest -- riding on a piece that moves "
           "until it throws them off. 0: none."},
          {"trail", "Trail", "Dust", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 100.0f, "",
           "Dust the pieces that came loose leave behind them as they fly fast -- for a second and a half after "
           "they broke off, the more the bigger and the faster they are. 0: none."},
          {"air", "Air Push", "Dust", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 100.0f, "",
           "The air the pieces squeeze out as they crush and knock: it swells the puffs and pushes the dust "
           "out along the ground. 1: as much as they would; 0: none."},
          {"color", "Color", "Look", K::Color, {0.62f, 0.6f, 0.57f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of the pieces where they have no Cd of their own."},
          {"inside_color", "Inside Color", "Look", K::Color, {0.5f, 0.47f, 0.43f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of the faces the fracture cut: what was inside."},
          text("inside_group", "Inside Group", "Look", "inside",
               "The group of those faces -- a Voronoi Fracture's Inside Group."),
          {"rebar_color", "Rebar Color", "Look", K::Color, {0.3f, 0.25f, 0.21f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of the bars: steel gone brown with rust."}},
         1});

    t.push_back(
        {"cloth_solver", "Cloth Solver", "Simulation",
         "Cloth, ropes and soft bodies (XPBD, as Houdini's Vellum): the polygons of the geometry linked into "
         "Geometry are cloth -- their edges hold their length, the cloth its folds as Bend says -- open "
         "polylines are ropes, and closed meshes with Pressure above 0 balloons that hold their volume. Points "
         "whose attribute pin is 1 go where the geometry has them at each frame: animated, they carry the cloth "
         "-- a flag on a pole, a curtain on a rail. It falls, drapes over the objects and the pieces of an RBD "
         "Solver linked into Colliders, a Thickness away, and over itself; the wind of the Forces and the flow "
         "of a Pyro Solver's gas blow it. Link it into the Output's Looks: it is simulated and drawn; Cloth "
         "Geometry brings it back as geometry.",
         {{"geometry", "Geometry", PinType::Geometry},
          {"colliders", "Colliders", PinType::Collider, true},
          {"forces", "Forces", PinType::Force, true}},
         {{"look", "Look", PinType::Look}, {"cloth", "Cloth", PinType::Cloth}},
         {{"density", "Density", "Cloth", K::Float, {0.3f, 0.0f, 0.0f}, 0.02f, 2.0f, 1e-4f, 1e4f, "kg/m\xc2\xb2",
           "How heavy a square metre of the cloth is -- a metre of a rope: 0.1 silk, 0.3 cotton, 0.8 canvas. A "
           "point's attribute mass, kg, in its place."},
          {"stretch", "Stretch", "Cloth", K::Float, {10000.0f, 0.0f, 0.0f}, 100.0f, 100000.0f, 0.01f, 1e9f, "N/m",
           "How hard an edge holds its length: 10000 cotton, barely stretching; a few hundred rubber."},
          {"shear", "Shear", "Cloth", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 100000.0f, 0.01f, 1e9f, "N/m",
           "How hard a quad holds its shape -- a pull along the bias: 1 woven cloth that drapes and droops; as "
           "much as Stretch a tarp, a sheet of plastic, paper."},
          {"bend", "Bend", "Cloth", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 50.0f, 0.0f, 1e9f, "N/m",
           "How hard the cloth holds its folds: 0.1 silk, 1 cotton, 10 canvas, 1000 cardboard. A rope: how hard "
           "it keeps straight."},
          {"pressure", "Pressure", "Cloth", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 100.0f, "",
           "Closed meshes -- a sphere, a box -- hold this share of the volume they have at rest: 1 a balloon, a "
           "cushion, more blows them up. 0: they are cloth like the rest."},
          {"tear", "Tear", "Cloth", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 100.0f, "",
           "How much longer than it was an edge stretches before it tears -- 0.3 thirty percent; 0 never. "
           "Torn, the cloth opens along its edges, a rope parts, a balloon bursts. A point's attribute tear "
           "scales it: 0.5 tears at half the stretch -- a seam, a perforation."},
          {"thickness", "Thickness", "Collisions", K::Float, {0.01f, 0.0f, 0.0f}, 0.002f, 0.1f, 1e-4f, 1.0f, "m",
           "How far from the floor, the objects and itself the cloth stays."},
          {"friction", "Friction", "Collisions", K::Float, {0.4f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, 10.0f, "",
           "How hard it grips what it lies on: 0 slides off, 1 stays."},
          {"self_collision", "Self Collision", "Collisions", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "It does not pass through itself: what folds lies on itself."},
          {"floor", "Floor", "Collisions", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A floor at height 0."},
          {"air_drag", "Air Drag", "Air", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 100.0f, "",
           "How hard the air pushes it: still air as it falls -- a sheet floats down, a ball drops -- the wind "
           "of the Forces, the flow of a Pyro Solver's gas. Closed meshes only from outside. 0: not at all."},
          {"damping", "Damping", "Air", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 5.0f, 0.0f, 1000.0f, "1/s",
           "How fast its motion dies away of itself."},
          {"gravity", "Gravity", "Air", K::Float, {9.81f, 0.0f, 0.0f}, 0.0f, 20.0f, -100.0f, 100.0f, "m/s\xc2\xb2",
           "How hard it is pulled down."},
          {"substeps", "Substeps", "Time", K::Int, {20.0f, 0.0f, 0.0f}, 5.0f, 60.0f, 1.0f, 200.0f, "",
           "Steps a frame. More: stiffer and steadier -- fast collisions, heavy points on light ones."},
          {"color", "Color", "Look", K::Color, {0.62f, 0.2f, 0.16f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "The colour of the cloth where it has no Cd of its own."}},
         1});

    // --- render ---------------------------------------------------------------------
    t.push_back(
        {"volume_look", "Volume Look", "Render",
         "How the gas is drawn: smoke that stops and scatters light, fire that glows. Changing it draws "
         "the frames again -- nothing is simulated again. The sun and the sky are the Output's.",
         {{"gas", "Gas", PinType::Gas}},
         {{"look", "Look", PinType::Look}},
         volumeLookParams(),
         2});
    t.push_back({"rain", "Rain", "Simulation",
                 "Rain from a cloud: drops fall from the box, the wind blows them slanting, they land on the "
                 "floor, on the objects linked into Colliders and in the water -- spraying off a solid, "
                 "ringing the water's surface -- and the floor gets wet. It is drawn as it falls: link it into "
                 "the Output's Looks.",
                 {{"forces", "Forces", PinType::Force, true}, {"colliders", "Colliders", PinType::Collider, true}},
                 {{"look", "Look", PinType::Look}, {"rain", "Rain", PinType::Rain}},
                 {{"center", "Position", "Cloud", K::Vector, {0.0f, 2.5f, 0.0f}, -2.0f, 5.0f, -kBig, kBig, "m",
                   "The middle of the cloud: y is how high the drops start."},
                  {"size", "Size", "Cloud", K::Vector, {3.0f, 0.5f, 3.0f}, 0.1f, 10.0f, 0.01f, 100.0f, "m",
                   "How wide, thick and deep the cloud is: the rain falls under it."},
                  {"rate", "Rate", "Rain", K::Float, {800.0f, 0.0f, 0.0f}, 0.0f, 4000.0f, 0.0f, 20000.0f, "1/m\xc2\xb2s",
                   "Drops a second on each square metre: drizzle to downpour."},
                  {"speed", "Speed", "Rain", K::Float, {7.0f, 0.0f, 0.0f}, 1.0f, 15.0f, 0.1f, 50.0f, "m/s",
                   "How fast the drops fall: some 7 m/s for rain, less for drizzle."},
                  {"splash", "Splash", "Rain", K::Float, {3.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, 20.0f, "",
                   "Droplets a drop throws up where it lands on something solid."},
                  {"ripples", "Ripples", "Rain", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 3.0f, 0.0f, 20.0f, "",
                   "How hard a drop rings the water it falls in."},
                  seed("Rain", "Another number: the drops at other places."),
                  {"start", "Start", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                   "When it starts to rain."},
                  {"end", "End", "Time", K::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, 0.0f, kBig, "s",
                   "When it stops. At or before the start, it never does."},
                  {"color", "Color", "Look", K::Color, {0.75f, 0.8f, 0.9f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "The colour of the drops in the light of the sky; in a render, the tint of the water."},
                  {"opacity", "Opacity", "Look", K::Float, {0.35f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "How much of what is behind a drop it hides. In a render, each drop is water a ray meets as "
                   "much of the time as this says -- a drop smeared by its motion."},
                  {"streak", "Streak", "Look", K::Float, {0.5f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 4.0f, "",
                   "How long a drop is drawn: as far as it falls in this share of a frame -- motion blur. The "
                   "renderers draw it so too."},
                  {"wet", "Wet Floor", "Look", K::Float, {0.6f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "How wet the floor looks: darker, and shining with the sky. In a render, whatever faces up "
                   "under the rain."}},
                 1});
    t.back().handles = {"center", nullptr, nullptr, "size", nullptr, nullptr};
    t.push_back({"water_look", "Water Look", "Render",
                 "How the water is drawn: a surface that reflects the sky and the sun and bends the light that "
                 "goes in, water that takes on its colour with depth, and white foam and spray. Changing it "
                 "draws the frames again -- nothing is simulated again.",
                 {{"liquid", "Liquid", PinType::Liquid}},
                 {{"look", "Look", PinType::Look}},
                 {{"color", "Deep Color", "Water", K::Color, {0.1f, 0.42f, 0.5f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "The colour the water takes on where it is deep: blue-green for the sea, brown for a river."},
                  {"clarity", "Clarity", "Water", K::Float, {1.5f, 0.0f, 0.0f}, 0.05f, 10.0f, 0.01f, kBig, "m",
                   "How far one sees into it: murky to crystal clear."},
                  {"foam", "Foam", "Water", K::Float, {1.0f, 0.0f, 0.0f}, 0.0f, 2.0f, 0.0f, kBig, "",
                   "How white the spray and the foam of fast water are drawn. 0: none."},
                  {"surface", "Surface", "Water", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "Draw the water's surface. Off, the water is simulated all the same, and only what the "
                   "network shows of it is seen -- its particles through Liquid Points."}},
                 1});
    t.push_back({"camera", "Camera", "Render",
                 "The camera of the shot: where it stands, which way it looks, its lens and the size of its "
                 "picture. Linked into the Output's Camera, it is what `prototype sim` and Render Image render "
                 "through, and what the viewport shows when it looks through the camera (0). It looks along "
                 "its own -z.",
                 {},
                 {{"camera", "Camera", PinType::Camera}},
                 {{"center", "Position", "Camera", K::Vector, {3.0f, 1.3f, 3.8f}, -10.0f, 10.0f, -kBig, kBig, "m",
                   "Where the camera stands."},
                  {"rotation", "Rotation", "Camera", K::Vector, {-10.0f, 38.0f, 0.0f}, -180.0f, 180.0f, -kBig, kBig,
                   "\xc2\xb0",
                   "Degrees about x (tilt), then y (pan), then z. At 0 it looks along -z, level."},
                  {"focal", "Focal Length", "Lens", K::Float, {38.0f, 0.0f, 0.0f}, 12.0f, 200.0f, 1.0f, 5000.0f, "mm",
                   "The lens, as on a full frame camera: 24 mm wide angle, 38 mm some 35\xc2\xb0 of view, 85 mm "
                   "a portrait, 200 mm a long lens that flattens the depth."},
                  {"width", "Width", "Image", K::Int, {1280.0f, 0.0f, 0.0f}, 16.0f, 3840.0f, 16.0f, 8192.0f, "px",
                   "The picture's width: with the height, its shape (the frame in the viewport) and the size "
                   "renders are made at."},
                  {"height", "Height", "Image", K::Int, {720.0f, 0.0f, 0.0f}, 16.0f, 2160.0f, 16.0f, 8192.0f, "px",
                   "The picture's height."},
                  plateFile("The plate: what the camera filmed, drawn behind the CG when you look through the "
                            "camera and in renders -- a picture (.exr, .png, .jpg) or a numbered sequence of them: "
                            "plate.####.exr, plate.$F4.exr, plate.%04d.exr. A relative path is read from the "
                            "network's folder."),
                  plateFrame(1.0f, "The number of the plate's frame at frame 1: 1001 for a plate numbered from "
                                   "1001.")},
                 1});
    t.back().handles = {"center", "rotation", nullptr, nullptr, nullptr, nullptr};
    t.push_back({"usd_camera", "USD Camera", "Render",
                 "A camera from a USD file -- a matchmove's, a layout's -- where it stands, which way it looks and "
                 "its lens at each frame, as the file has them. Linked into the Output's Camera, it is what renders "
                 "look through: the effect sits in the shot the plate was filmed in.",
                 {},
                 {{"camera", "Camera", PinType::Camera}},
                 {usdFile("A USD file with the camera: .usd, .usda, .usdc or .usdz. A relative path is read from the "
                          "network's folder."),
                  {"prim", "Prim", "Camera", K::Text, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "",
                   "The camera's prim: /World/cam. Empty: the stage's first camera."},
                  frameOffset(),
                  {"width", "Width", "Image", K::Int, {1920.0f, 0.0f, 0.0f}, 16.0f, 3840.0f, 16.0f, 8192.0f, "px",
                   "The picture's width. The lens is fitted to it: what the camera sees from side to side is what "
                   "its film back (horizontal aperture) and focal length say."},
                  {"height", "Height", "Image", K::Int, {0.0f, 0.0f, 0.0f}, 0.0f, 2160.0f, 0.0f, 8192.0f, "px",
                   "The picture's height. 0: as the film back is shaped -- width x vertical / horizontal aperture."},
                  {"metres", "Metres, Y Up", "Camera", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
                   "The stage's units and up axis made the program's (metres, Y up), as USD Import does. Off: as "
                   "the file has them."},
                  plateFile("The plate: the footage this camera was matched to, drawn behind the CG when you "
                            "look through the camera and in renders -- a picture (.exr, .png, .jpg) or a numbered "
                            "sequence: plate.####.exr, plate.$F4.exr, plate.%04d.exr. A relative path is read "
                            "from the network's folder."),
                  plateFrame(0.0f, "The number of the plate's frame at frame 1. 0: the shot's own -- the time "
                                   "code the camera is read at, 1001 for a shot from 1001.")},
                 1});
    t.push_back({"output", "Output", "Render",
                 "Where the network ends: what the viewport shows and `prototype sim` renders -- every look "
                 "linked into it, in one scene, lit by one sun and one sky, at one frame rate -- through the "
                 "camera linked into Camera, if there is one.",
                 {{"look", "Looks", PinType::Look, true}, {"camera", "Camera", PinType::Camera}},
                 {},
                 outputParams(),
                 2});

    for (NodeType& type : t) {
        const std::string c = type.category;
        type.bypassable = c == "Objects" || c == "Sources" || c == "Forces" || c == "Geometry";
    }
    return t;
}



bool validName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    if (std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

std::string formatNumber(float x) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof buf, x);
    return std::string(buf, res.ptr);
}

bool parseNumber(std::string_view text, float& out) {
    // Not std::from_chars: libc++ before LLVM 20 (FreeBSD 14, older macOS)
    // has it for integers only. A stream in the classic locale reads "1.5"
    // whatever the user's locale says; the characters are checked first, as
    // libc++'s stream would also take hex, "inf" and "nan".
    if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string_view::npos) return false;
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    float v = 0.0f;
    in >> v;
    if (in.fail() || in.peek() != std::char_traits<char>::eof() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

/// "a \"b\" c" -- text in quotes, as files write it: a quote and a
/// backslash after a backslash, a line break as \n, a tab as \t -- all of a
/// snippet on one line.
std::string quotedPath(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
    }
    return out + '"';
}

/// The other way round -- or, not in quotes, the text as it is.
bool unquoted(std::string_view text, std::string& out) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    out.clear();
    if (text.empty() || text.front() != '"') {
        out = text;
        return true;
    }
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            const char e = text[++i];
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        } else if (c == '"') {
            return i + 1 == text.size();  // nothing after the closing quote
        } else {
            out += c;
        }
    }
    return false;  // no closing quote
}

/// Where a line's comment starts: a '#' that is not inside quotes.
size_t commentStart(std::string_view line) {
    bool inside = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && inside) {
            ++i;
        } else if (line[i] == '"') {
            inside = !inside;
        } else if (line[i] == '#' && !inside) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::vector<std::string_view> splitWords(std::string_view s) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != ',') ++i;
        if (i > start) words.push_back(s.substr(start, i - start));
    }
    return words;
}

ParamValue keep(const ParamDef& def, ParamValue v) {
    if (isText(def.kind)) return def.value;  // its value is text
    const int n = def.kind == K::Vector || def.kind == K::Color ? 3 : 1;
    for (int i = 0; i < 3; ++i) {
        if (i >= n) {
            v[static_cast<size_t>(i)] = 0.0f;
            continue;
        }
        float& x = v[static_cast<size_t>(i)];
        if (!std::isfinite(x)) x = def.value[static_cast<size_t>(i)];
        if (def.kind == K::Int || def.kind == K::Choice || def.kind == K::Toggle) x = std::round(x);
        x = std::clamp(x, def.lo, def.hi);
    }
    if (def.kind == K::Choice) {
        v[0] = std::clamp(v[0], 0.0f, static_cast<float>(std::max<size_t>(def.choices.size(), 1) - 1));
    }
    return v;
}

// --- files of earlier versions ------------------------------------------------------

/// A node type as an earlier version wrote it -- its parameters, so that its
/// values read as they were meant -- and how to turn such a node into the
/// type that replaced it.
struct Legacy {
    NodeType type;          ///< name, the parameters, `version`: the files up to it
    /// Turns `node` into its successor; `nodes` and `links` are the rest of
    /// the file, for what moved to another node.
    void (*upgrade)(Node& node, std::vector<Node>& nodes, const std::vector<Link>& links);
};

/// The node of `type` that `from`'s output `pin` leads to, if any.
Node* fedBy(std::vector<Node>& nodes, const std::vector<Link>& links, int from, const char* pin, const char* type) {
    for (const Link& l : links) {
        if (l.from != from || l.output != pin) continue;
        for (Node& n : nodes) {
            if (n.id == l.to && n.type == type) return &n;
        }
    }
    return nullptr;
}

/// Every value of `node` under the parameters of `type`: the ones set, the
/// rest at the defaults of that type.
std::map<std::string, ParamValue> allValues(const Node& node, const NodeType& type) {
    std::map<std::string, ParamValue> v;
    for (const ParamDef& d : type.params) v[d.name] = d.value;
    for (const auto& [name, value] : node.params) v[name] = value;
    return v;
}

/// `node` becomes a node of `type` with `values`; only those that differ
/// from the defaults of `type` are kept, as always.
void become(Node& node, const NodeType& type, const std::map<std::string, ParamValue>& values) {
    node.type = type.name;
    node.version = type.version;
    node.params.clear();
    for (const ParamDef& d : type.params) {
        const auto it = values.find(d.name);
        if (it != values.end() && keep(d, it->second) != d.value) node.params[d.name] = keep(d, it->second);
    }
}

const std::vector<Legacy>& legacyTypes();

/// Sphere Source, Box Source (version 1) -> Pyro Source: the shape a choice,
/// a sphere's radius half its size.
void upgradeSource(Node& node, std::vector<Node>&, const std::vector<Link>&) {
    const bool box = node.type == "box_source";
    const NodeType* old = nullptr;
    for (const Legacy& l : legacyTypes()) {
        if (node.type == l.type.name) old = &l.type;
    }
    std::map<std::string, ParamValue> v = allValues(node, *old);
    if (!box) {
        const float d = 2.0f * v["radius"][0];
        v["size"] = {d, d, d};
    }
    v["shape"] = {box ? 1.0f : 0.0f, 0.0f, 0.0f};
    become(node, *findNodeType("pyro_source"), v);
}

/// Sphere Collider, Box Collider (version 1) -> Object.
void upgradeCollider(Node& node, std::vector<Node>&, const std::vector<Link>&) {
    const bool box = node.type == "box_collider";
    const NodeType* old = nullptr;
    for (const Legacy& l : legacyTypes()) {
        if (node.type == l.type.name) old = &l.type;
    }
    std::map<std::string, ParamValue> v = allValues(node, *old);
    if (!box) {
        const float d = 2.0f * v["radius"][0];
        v["size"] = {d, d, d};
    }
    v["shape"] = {box ? 1.0f : 0.0f, 0.0f, 0.0f};
    become(node, *findNodeType("object"), v);
}

/// Volume Look 1 -> 2: the light, the sky and the image went to the Output
/// it feeds; those set move there.
void upgradeVolumeLook(Node& node, std::vector<Node>& nodes, const std::vector<Link>& links) {
    Node* out = fedBy(nodes, links, node.id, "look", "output");
    for (const char* name : kEnvironment) {
        const auto it = node.params.find(name);
        if (it == node.params.end()) continue;
        if (out) out->params[name] = it->second;
        node.params.erase(it);
    }
    node.version = 2;
}

/// Pyro Solver 1 -> 2: the frame rate went to the Output its gas reaches,
/// through a look.
void upgradePyroSolver(Node& node, std::vector<Node>& nodes, const std::vector<Link>& links) {
    const auto it = node.params.find("fps");
    if (it != node.params.end()) {
        Node* look = fedBy(nodes, links, node.id, "gas", "volume_look");
        Node* out = look ? fedBy(nodes, links, look->id, "look", "output") : nullptr;
        if (out) out->params["fps"] = it->second;
        node.params.erase(it);
    }
    node.version = 2;
}

/// Output 1 -> 2: its Looks take several; the rest came from the others.
void upgradeOutput(Node& node, std::vector<Node>&, const std::vector<Link>&) { node.version = 2; }

const std::vector<Legacy>& legacyTypes() {
    static const std::vector<Legacy> types = [] {
        std::vector<Legacy> l;
        l.push_back({{"pyro_solver", "Pyro Solver", "Simulation", "", {}, {}, pyroSolverParams(true), 1}, upgradePyroSolver});
        l.push_back({{"volume_look", "Volume Look", "Render", "", {}, {}, legacyVolumeLookParams(), 1}, upgradeVolumeLook});
        l.push_back({{"output", "Output", "Render", "", {}, {}, {outputParams().front()}, 1}, upgradeOutput});
        const std::vector<PinDef> source = {{"source", "Source", PinType::Source}};
        const std::vector<PinDef> collider = {{"collider", "Collider", PinType::Collider}};
        l.push_back({{"sphere_source", "Sphere Source", "Sources", "", {}, source, legacySourceParams(true), 1},
                     upgradeSource});
        l.push_back({{"box_source", "Box Source", "Sources", "", {}, source, legacySourceParams(false), 1},
                     upgradeSource});
        l.push_back({{"sphere_collider", "Sphere Collider", "Colliders", "", {}, collider,
                      legacyColliderParams(true), 1},
                     upgradeCollider});
        l.push_back({{"box_collider", "Box Collider", "Colliders", "", {}, collider, legacyColliderParams(false), 1},
                     upgradeCollider});
        return l;
    }();
    return types;
}

/// The old type a node of `name` and `version` is, if it is one.
const Legacy* legacyType(std::string_view name, int version) {
    for (const Legacy& l : legacyTypes()) {
        if (name == l.type.name && version <= l.type.version) return &l;
    }
    return nullptr;
}

}  // namespace

// --- the types -----------------------------------------------------------------------

const char* pinTypeName(PinType type) {
    switch (type) {
        case PinType::Source: return "source";
        case PinType::Force: return "force";
        case PinType::Collider: return "collider";
        case PinType::Gas: return "gas";
        case PinType::Look: return "look";
        case PinType::Water: return "water";
        case PinType::Liquid: return "liquid";
        case PinType::Camera: return "camera";
        case PinType::Geometry: return "geometry";
        case PinType::Rain: return "rain";
        case PinType::Rigid: return "rigid";
        case PinType::Cloth: return "cloth";
    }
    return "?";
}

const ParamDef* NodeType::param(std::string_view n) const {
    for (const ParamDef& p : params) {
        if (n == p.name) return &p;
    }
    return nullptr;
}

const PinDef* NodeType::input(std::string_view n) const {
    for (const PinDef& p : inputs) {
        if (n == p.name) return &p;
    }
    return nullptr;
}

const PinDef* NodeType::output(std::string_view n) const {
    for (const PinDef& p : outputs) {
        if (n == p.name) return &p;
    }
    return nullptr;
}

bool isValidName(std::string_view name) { return validName(name); }

const std::vector<NodeType>& nodeTypes() {
    static const std::vector<NodeType> types = buildTypes();
    return types;
}

const NodeType* findNodeType(std::string_view name) {
    for (const NodeType& t : nodeTypes()) {
        if (name == t.name) return &t;
    }
    // A digital asset's: kept by the library as long as the program runs.
    if (const auto def = AssetLibrary::instance().find(name)) return &def->type;
    return nullptr;
}

std::vector<const NodeType*> allNodeTypes() {
    std::vector<const NodeType*> out;
    for (const NodeType& t : nodeTypes()) out.push_back(&t);
    for (const NodeType* t : assetTypes()) out.push_back(t);
    return out;
}

const std::vector<const char*>& nodeCategories() {
    static const std::vector<const char*> c = {"Geometry", "Objects", "Sources", "Forces", "Simulation", "Render", "Assets"};
    return c;
}

const char* interpName(Interp interp) {
    switch (interp) {
        case Interp::Smooth: return "smooth";
        case Interp::Linear: return "linear";
        case Interp::Step: return "step";
    }
    return "smooth";
}

ParamValue evaluate(const std::vector<Key>& keys, float frame, ParamKind kind) {
    if (keys.empty()) return {};
    if (frame <= keys.front().frame) return keys.front().value;
    if (frame >= keys.back().frame) return keys.back().value;
    size_t i = 0;
    while (i + 2 < keys.size() && keys[i + 1].frame <= frame) ++i;
    const Key &a = keys[i], &b = keys[i + 1];
    const bool steps = kind == K::Toggle || kind == K::Choice;
    if (a.interp == Interp::Step || steps) return a.value;
    const float span = std::max(b.frame - a.frame, 1e-6f);
    const float t = std::clamp((frame - a.frame) / span, 0.0f, 1.0f);
    ParamValue v{};
    for (size_t c = 0; c < 3; ++c) {
        if (a.interp == Interp::Linear) {
            v[c] = a.value[c] + (b.value[c] - a.value[c]) * t;
            continue;
        }
        // The slope at a key: flat at the first and the last, and where the
        // value turns; elsewhere Catmull-Rom's, limited so that the curve
        // does not overshoot between the keys (Fritsch and Carlson).
        auto slope = [&](size_t k) {
            if (k == 0 || k + 1 >= keys.size()) return 0.0f;
            const float before = (keys[k].value[c] - keys[k - 1].value[c]) / std::max(keys[k].frame - keys[k - 1].frame, 1e-6f);
            const float after = (keys[k + 1].value[c] - keys[k].value[c]) / std::max(keys[k + 1].frame - keys[k].frame, 1e-6f);
            if (before * after <= 0.0f) return 0.0f;
            const float m = (keys[k + 1].value[c] - keys[k - 1].value[c]) /
                            std::max(keys[k + 1].frame - keys[k - 1].frame, 1e-6f);
            const float most = 3.0f * std::min(std::fabs(before), std::fabs(after));
            return std::clamp(m, -most, most);
        };
        const float t2 = t * t, t3 = t2 * t;
        v[c] = (2.0f * t3 - 3.0f * t2 + 1.0f) * a.value[c] + (t3 - 2.0f * t2 + t) * span * slope(i) +
               (-2.0f * t3 + 3.0f * t2) * b.value[c] + (t3 - t2) * span * slope(i + 1);
    }
    if (kind == K::Int) {
        for (float& x : v) x = std::round(x);
    }
    return v;
}

std::string formatParam(const ParamDef& def, const ParamValue& v) {
    switch (def.kind) {
        case K::Toggle: return v[0] != 0.0f ? "on" : "off";
        case K::Choice: {
            const size_t i = static_cast<size_t>(std::max(0.0f, v[0]));
            return i < def.choices.size() ? def.choices[i] : formatNumber(v[0]);
        }
        case K::Vector:
        case K::Color: return formatNumber(v[0]) + ' ' + formatNumber(v[1]) + ' ' + formatNumber(v[2]);
        case K::File:
        case K::Text:
        case K::Code:
        case K::Data: return "\"\"";  // the text is the node's, not the value's
        case K::Float:
        case K::Int: break;
    }
    return formatNumber(v[0]);
}

bool parseParam(const ParamDef& def, std::string_view text, ParamValue& out, std::string& error) {
    const std::vector<std::string_view> w = splitWords(text);
    ParamValue v = def.value;
    switch (def.kind) {
        case K::File:
        case K::Text:
        case K::Code:
        case K::Data: {
            std::string path;
            if (!unquoted(text, path)) break;
            out = v;
            return true;
        }
        case K::Toggle: {
            if (w.size() == 1) {
                const std::string_view s = w[0];
                if (s == "on" || s == "true" || s == "yes" || s == "1") v[0] = 1.0f;
                else if (s == "off" || s == "false" || s == "no" || s == "0") v[0] = 0.0f;
                else break;
                out = v;
                return true;
            }
            break;
        }
        case K::Choice: {
            if (w.size() != 1) break;
            for (size_t i = 0; i < def.choices.size(); ++i) {
                if (w[0] == def.choices[i]) {
                    v[0] = static_cast<float>(i);
                    out = v;
                    return true;
                }
            }
            error = std::string(def.name) + " is one of:";
            for (const char* c : def.choices) error += std::string(" ") + c;
            return false;
        }
        case K::Vector:
        case K::Color: {
            // Three numbers, or one for all three.
            if (w.size() != 3 && w.size() != 1) break;
            bool ok = true;
            for (size_t i = 0; i < 3; ++i) ok = ok && parseNumber(w[w.size() == 1 ? 0 : i], v[i]);
            if (!ok) break;
            out = keep(def, v);
            return true;
        }
        case K::Float:
        case K::Int: {
            if (w.size() != 1 || !parseNumber(w[0], v[0])) break;
            out = keep(def, v);
            return true;
        }
    }
    switch (def.kind) {
        case K::File: error = std::string(def.name) + " is a path, in quotes if it has spaces"; break;
        case K::Text:
        case K::Code:
        case K::Data: error = std::string(def.name) + " is text, in quotes if it has spaces"; break;
        case K::Toggle: error = std::string(def.name) + " is on or off"; break;
        case K::Vector:
        case K::Color: error = std::string(def.name) + " wants three numbers, like 0 1 0"; break;
        case K::Int: error = std::string(def.name) + " wants a whole number"; break;
        default: error = std::string(def.name) + " wants a number"; break;
    }
    error += ", not '" + std::string(text) + "'";
    return false;
}

// --- the network ---------------------------------------------------------------------

int Network::indexOf(int id) const {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

Node* Network::node(int id) {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes_[static_cast<size_t>(i)];
}

const Node* Network::node(int id) const {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &nodes_[static_cast<size_t>(i)];
}

const Node* Network::named(std::string_view name) const {
    for (const Node& n : nodes_) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

std::string Network::uniqueName(std::string_view base) const {
    std::string stem(base);
    if (!validName(stem)) stem = "node";
    // "fire3" -> stem "fire": copies number on from there.
    while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
    if (stem.empty()) stem = "node";
    for (int i = 1;; ++i) {
        std::string name = stem + std::to_string(i);
        if (!named(name)) return name;
    }
}

int Network::add(std::string_view type, float x, float y) {
    const NodeType* t = findNodeType(type);
    if (!t) return 0;
    Node n;
    n.id = nextId_++;
    n.type = t->name;
    n.version = t->version;
    n.name = uniqueName(t->name);
    n.x = x;
    n.y = y;
    nodes_.push_back(std::move(n));
    revision_ = nextRevision();
    return nodes_.back().id;
}

bool Network::remove(int id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    std::erase_if(asset_.promoted, [&](const Promotion& p) { return p.node == nodes_[static_cast<size_t>(i)].name; });
    nodes_.erase(nodes_.begin() + i);
    std::erase_if(links_, [&](const Link& l) { return l.from == id || l.to == id; });
    revision_ = nextRevision();
    return true;
}

bool Network::rename(int id, std::string_view name, std::string* error) {
    Node* n = node(id);
    if (!n) return false;
    if (n->name == name) return true;
    if (!validName(name)) {
        if (error) *error = "a name is letters, digits and _, not starting with a digit";
        return false;
    }
    if (named(name)) {
        if (error) *error = "another node is called " + std::string(name);
        return false;
    }
    for (Promotion& p : asset_.promoted) {
        if (p.node == n->name) p.node = name;
    }
    n->name = name;
    revision_ = nextRevision();
    return true;
}

void Network::setAsset(AssetInfo info) {
    if (info == asset_) return;
    asset_ = std::move(info);
    revision_ = nextRevision();
}

bool Network::promote(int id, std::string_view param, bool on) {
    const Node* n = node(id);
    const ParamDef* d = n ? def(*n, param) : nullptr;
    if (!d) return false;
    const auto it = std::find_if(asset_.promoted.begin(), asset_.promoted.end(),
                                 [&](const Promotion& p) { return p.node == n->name && p.param == param; });
    if (!on) {
        if (it == asset_.promoted.end()) return true;
        asset_.promoted.erase(it);
        revision_ = nextRevision();
        return true;
    }
    if (it != asset_.promoted.end()) return true;
    // Its own name, unless another node's parameter has it already.
    std::string name(param);
    auto taken = [&](const std::string& s) {
        return std::any_of(asset_.promoted.begin(), asset_.promoted.end(), [&](const Promotion& p) { return p.name == s; });
    };
    if (taken(name)) {
        for (int k = 2; taken(name = std::string(param) + std::to_string(k)); ++k) {}
    }
    asset_.promoted.push_back({n->name, std::string(param), name, {}});
    revision_ = nextRevision();
    return true;
}

const Promotion* Network::promotion(int id, std::string_view param) const {
    const Node* n = node(id);
    if (!n) return nullptr;
    for (const Promotion& p : asset_.promoted) {
        if (p.node == n->name && p.param == param) return &p;
    }
    return nullptr;
}

bool Network::canConnect(int from, std::string_view output, int to, std::string_view input,
                         std::string* error) const {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    const Node* a = node(from);
    const Node* b = node(to);
    if (!a || !b) return fail("no such node");
    if (from == to) return fail("a node cannot feed itself");
    const NodeType* ta = findNodeType(a->type);
    const NodeType* tb = findNodeType(b->type);
    if (!ta || !tb) return fail("unknown node type");
    const PinDef* out = ta->output(output);
    const PinDef* in = tb->input(input);
    if (!out) return fail(a->name + " has no output " + std::string(output));
    if (!in) return fail(b->name + " has no input " + std::string(input));
    if (out->type != in->type) {
        return fail(std::string("a ") + pinTypeName(out->type) + " cannot go into " + in->label + ", which takes a " +
                    pinTypeName(in->type));
    }
    for (const Link& l : links_) {
        if (l.from == from && l.output == output && l.to == to && l.input == input) return fail("already linked");
    }
    // No loops: what `to` leads to must not lead back to `from`.
    std::vector<int> stack{to}, seen{to};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (n == from) return fail("that would make a loop: " + b->name + " leads to " + a->name + " already");
        for (const Link& l : links_) {
            if (l.from == n && std::find(seen.begin(), seen.end(), l.to) == seen.end()) {
                seen.push_back(l.to);
                stack.push_back(l.to);
            }
        }
    }
    return true;
}

bool Network::connect(int from, std::string_view output, int to, std::string_view input, std::string* error) {
    if (!canConnect(from, output, to, input, error)) return false;
    const PinDef* in = findNodeType(node(to)->type)->input(input);
    if (!in->many) std::erase_if(links_, [&](const Link& l) { return l.to == to && l.input == input; });
    links_.push_back({from, std::string(output), to, std::string(input)});
    revision_ = nextRevision();
    return true;
}

bool Network::disconnect(const Link& link) {
    const auto it = std::find(links_.begin(), links_.end(), link);
    if (it == links_.end()) return false;
    links_.erase(it);
    revision_ = nextRevision();
    return true;
}

std::vector<Link> Network::linksInto(int to, std::string_view input) const {
    std::vector<Link> out;
    for (const Link& l : links_) {
        if (l.to == to && l.input == input) out.push_back(l);
    }
    return out;
}

namespace {

const char* interned(const std::string& s) { return internText(s); }

ParamDef spareDef(const lang::Channel& ch) {
    const char* name = interned(ch.name);
    const char* help = interned("What the snippet reads with ch(\"" + ch.name + "\").");
    switch (ch.type) {
        case lang::Type::Int:
            return {name, name, "Parameters", ParamKind::Int, {0.0f, 0.0f, 0.0f}, 0.0f, 10.0f, -kBig, kBig, "", help};
        case lang::Type::Vec2:
        case lang::Type::Vec3:
        case lang::Type::Vec4:
            return {name, name, "Parameters", ParamKind::Vector, {0.0f, 0.0f, 0.0f}, -1.0f, 1.0f, -kBig, kBig, "", help};
        case lang::Type::String: {
            ParamDef d{name, name, "Parameters", ParamKind::Text, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, "", help};
            d.text = "";
            return d;
        }
        default:
            return {name, name, "Parameters", ParamKind::Float, {0.0f, 0.0f, 0.0f}, 0.0f, 1.0f, -kBig, kBig, "", help};
    }
}

}  // namespace

const char* internText(const std::string& text) {
    static std::mutex mu;
    static std::set<std::string> pool;
    std::lock_guard<std::mutex> lock(mu);
    return pool.insert(text).first->c_str();
}

const ParamDef* Network::def(const Node& n, std::string_view name) const {
    const NodeType* t = findNodeType(n.type);
    if (const ParamDef* d = t ? t->param(name) : nullptr) return d;
    for (const ParamDef& d : n.spares) {
        if (name == d.name) return &d;
    }
    return nullptr;
}

const ParamDef* Network::paramDef(int id, std::string_view name) const {
    const Node* n = node(id);
    return n ? def(*n, name) : nullptr;
}

std::vector<const ParamDef*> Network::params(int id) const {
    std::vector<const ParamDef*> out;
    const Node* n = node(id);
    if (!n) return out;
    if (const NodeType* t = findNodeType(n->type)) {
        for (const ParamDef& d : t->params) out.push_back(&d);
    }
    for (const ParamDef& d : n->spares) out.push_back(&d);
    return out;
}

bool Network::syncSpares(Node& n) {
    const NodeType* t = findNodeType(n.type);
    const bool wrangle = t && t->core && std::string_view(t->core) == "attribwrangle";
    std::vector<ParamDef> next;
    if (wrangle) {
        const auto it = n.texts.find("snippet");
        const std::string snippet = it != n.texts.end() ? it->second : std::string();
        std::string error;
        const auto prog = snippet.empty() ? nullptr : lang::Program::parse(snippet, error);
        // A snippet that does not parse -- half typed -- keeps what it asked for.
        if (!snippet.empty() && !prog) return false;
        if (prog) {
            for (const lang::Channel& ch : prog->channels()) {
                if (!validName(ch.name) || t->param(ch.name)) continue;
                next.push_back(spareDef(ch));
            }
        }
    }
    auto same = [](const ParamDef& a, const ParamDef& b) { return std::string_view(a.name) == b.name && a.kind == b.kind; };
    bool changed = next.size() != n.spares.size();
    for (const ParamDef& old : n.spares) {
        const bool kept = std::any_of(next.begin(), next.end(), [&](const ParamDef& d) { return same(d, old); });
        if (kept) continue;
        changed = true;
        n.params.erase(old.name);
        n.texts.erase(old.name);
        n.keys.erase(old.name);
    }
    for (size_t i = 0; !changed && i < next.size(); ++i) changed = !same(next[i], n.spares[i]);
    n.spares = std::move(next);
    return changed;
}

ParamValue Network::param(int id, std::string_view name) const {
    const Node* n = node(id);
    if (!n) return {};
    const auto it = n->params.find(std::string(name));
    if (it != n->params.end()) return it->second;
    const ParamDef* d = def(*n, name);
    return d ? d->value : ParamValue{};
}

bool Network::setParam(int id, std::string_view name, const ParamValue& value) {
    Node* n = node(id);
    const ParamDef* d = n ? def(*n, name) : nullptr;
    if (!d) return false;
    const ParamValue v = keep(*d, value);
    const std::string key(name);
    const auto it = n->params.find(key);
    const ParamValue before = it != n->params.end() ? it->second : d->value;
    // Stored only when it differs from the default: the file lists the
    // changes, and a default that improves reaches the networks that kept it.
    if (v == d->value) n->params.erase(key);
    else n->params[key] = v;
    if (v != before) revision_ = nextRevision();
    return true;
}

bool Network::setParam(int id, std::string_view name, std::string_view text, std::string* error) {
    const Node* n = node(id);
    const NodeType* t = n ? findNodeType(n->type) : nullptr;
    const ParamDef* d = n ? def(*n, name) : nullptr;
    if (!d) {
        if (error) {
            *error = !n ? "no such node" : !t ? "unknown node type " + n->type : n->name + " has no parameter " + std::string(name);
        }
        return false;
    }
    if (isText(d->kind)) {
        std::string path;
        if (!unquoted(text, path)) {
            if (error) *error = std::string(name) + ": a quote is not closed";
            return false;
        }
        return setText(id, name, path);
    }
    ParamValue v;
    std::string why;
    if (!parseParam(*d, text, v, why)) {
        if (error) *error = why;
        return false;
    }
    return setParam(id, name, v);
}

std::string Network::text(int id, std::string_view name) const {
    const Node* n = node(id);
    if (!n) return {};
    const auto it = n->texts.find(std::string(name));
    if (it != n->texts.end()) return it->second;
    const ParamDef* d = def(*n, name);
    return d && d->text ? d->text : std::string();
}

bool Network::setText(int id, std::string_view name, std::string_view value) {
    Node* n = node(id);
    const ParamDef* d = n ? def(*n, name) : nullptr;
    if (!d || !isText(d->kind)) return false;
    const std::string before = text(id, name);
    // Kept only when it differs from the default, as a number is.
    const std::string key(name);
    if (value == (d->text ? d->text : "")) n->texts.erase(key);
    else n->texts[key] = std::string(value);
    bool changed = before != value;
    // A snippet asks for its own parameters.
    if (changed && name == "snippet" && syncSpares(*n)) changed = true;
    if (changed) revision_ = nextRevision();
    return true;
}

bool Network::resetParam(int id, std::string_view name) {
    Node* n = node(id);
    if (!n) return false;
    const std::string key(name);
    size_t erased = n->params.erase(key) + n->texts.erase(key) + n->keys.erase(key);
    if (const ParamDef* d = def(*n, name)) {
        for (const std::string& ch : channels(*d)) erased += n->exprs.erase(ch);
    }
    if (erased > 0) revision_ = nextRevision();
    return true;
}

bool Network::isDefault(int id, std::string_view name) const {
    const Node* n = node(id);
    const std::string key(name);
    return !n || (n->params.find(key) == n->params.end() && n->texts.find(key) == n->texts.end() &&
                  n->keys.find(key) == n->keys.end() && !hasExpression(id, name));
}

// --- expressions -------------------------------------------------------------------

namespace {

constexpr int kDeepest = 16;  ///< expressions that refer on further than this go round in a loop

struct Parsed {
    std::shared_ptr<const lang::Expression> expr;
    std::string error;
};

/// An expression, parsed once for all networks and threads.
Parsed parsedExpression(const std::string& text) {
    static std::mutex mu;
    static std::map<std::string, Parsed> cache;
    std::lock_guard<std::mutex> lock(mu);
    const auto it = cache.find(text);
    if (it != cache.end()) return it->second;
    if (cache.size() > 4096) cache.clear();
    Parsed p;
    std::unique_ptr<lang::Expression> e = lang::Expression::parse(text, p.error);
    p.expr = std::shared_ptr<const lang::Expression>(std::move(e));
    if (p.expr && p.expr->type() == lang::Type::String) {
        p.expr.reset();
        p.error = "a number was expected, not text";
    }
    cache[text] = p;
    return p;
}

/// The parameters whose expressions this thread is evaluating: one that
/// asks for its own value, round others, is in a loop.
thread_local std::vector<std::pair<int, std::string>> evaluating;

struct Evaluating {
    Evaluating(int id, std::string name) { evaluating.emplace_back(id, std::move(name)); }
    ~Evaluating() { evaluating.pop_back(); }
    Evaluating(const Evaluating&) = delete;
    Evaluating& operator=(const Evaluating&) = delete;
};

bool beingEvaluated(int id, const std::string& name) {
    return std::find(evaluating.begin(), evaluating.end(), std::make_pair(id, name)) != evaluating.end();
}

/// "center.y" -> "center", 1; "sizex" -> "sizex", -1.
std::pair<std::string, int> splitChannel(std::string_view channel) {
    if (channel.size() > 2 && channel[channel.size() - 2] == '.') {
        const char c = channel.back();
        if (c == 'x' || c == 'y' || c == 'z') return {std::string(channel.substr(0, channel.size() - 2)), c - 'x'};
    }
    return {std::string(channel), -1};
}

}  // namespace

/// What an expression on a parameter reads: the frame, and the parameters
/// of this node and of the others, each at the same frame.
class ExpressionHost : public lang::Host {
public:
    ExpressionHost(const Network& net, const Node& node, float frame, int depth)
        : net_(net), node_(node), frame_(frame), depth_(depth) {}

    bool variable(std::string_view name, double& out) const override {
        double fps = 30.0;
        for (const Node& n : net_.nodes()) {
            if (n.type == "output") fps = std::max(1.0, static_cast<double>(net_.param(n.id, "fps")[0]));
        }
        if (name == "F") out = std::round(frame_);
        else if (name == "FF") out = frame_;
        else if (name == "T") out = frame_ / fps;
        else if (name == "FPS") out = fps;
        else return false;
        return true;
    }

    bool channel(std::string_view path, int component, double& out, std::string& error) const override {
        const Node* target = nullptr;
        const ParamDef* d = nullptr;
        int c = component;
        if (!resolve(path, target, d, c, error)) return false;
        if (isText(d->kind)) {
            error = "'" + std::string(path) + "' is text: chs() reads it";
            return false;
        }
        if (depth_ >= kDeepest || beingEvaluated(target->id, d->name)) {
            error = "expressions that refer to each other round in a loop (" + target->name + "/" + d->name + ")";
            return false;
        }
        std::string inner;
        const ParamValue v = net_.valueAtDepth(target->id, d->name, frame_, depth_ + 1, &inner);
        if (!inner.empty()) {
            error = inner;
            return false;
        }
        const bool vector = d->kind == ParamKind::Vector || d->kind == ParamKind::Color;
        out = v[vector ? static_cast<size_t>(std::clamp(c, 0, 2)) : 0];
        return true;
    }

    bool channelText(std::string_view path, std::string& out, std::string& error) const override {
        const Node* target = nullptr;
        const ParamDef* d = nullptr;
        int c = 0;
        if (!resolve(path, target, d, c, error)) return false;
        if (!isText(d->kind)) {
            error = "'" + std::string(path) + "' is not text";
            return false;
        }
        out = net_.text(target->id, d->name);
        return true;
    }

    /// "sizex", "../box1/sizex", "box1/center.y", "box1/centery": the node
    /// and its parameter -- and the component, when the path names one.
    bool resolve(std::string_view path, const Node*& target, const ParamDef*& d, int& component, std::string& error) const {
        std::string p(path);
        while (p.rfind("../", 0) == 0) p = p.substr(3);
        if (p.rfind("./", 0) == 0) p = p.substr(2);
        target = &node_;
        std::string param = p;
        if (const size_t slash = p.rfind('/'); slash != std::string::npos) {
            const std::string nodeName = p.substr(0, slash);
            param = p.substr(slash + 1);
            target = net_.named(nodeName);
            if (!target) {
                error = "no node '" + nodeName + "'";
                return false;
            }
        }
        d = net_.def(*target, param);
        if (!d) {
            // A component: center.y, or centery.
            auto [base, c] = splitChannel(param);
            if (c < 0 && param.size() > 1) {
                const char last = param.back();
                if (last == 'x' || last == 'y' || last == 'z') {
                    base = param.substr(0, param.size() - 1);
                    c = last - 'x';
                }
            }
            const ParamDef* b = c >= 0 ? net_.def(*target, base) : nullptr;
            if (b && (b->kind == ParamKind::Vector || b->kind == ParamKind::Color)) {
                d = b;
                component = c;
            }
        }
        if (!d) {
            error = target->name + " has no parameter '" + param + "'";
            return false;
        }
        return true;
    }

private:
    const Network& net_;
    const Node& node_;
    float frame_;
    int depth_;
};

std::vector<std::string> Network::channels(const ParamDef& d) {
    if (isText(d.kind)) return {};
    if (d.kind == ParamKind::Vector || d.kind == ParamKind::Color) {
        const std::string n = d.name;
        return {n + ".x", n + ".y", n + ".z"};
    }
    return {d.name};
}

std::string Network::expression(int id, std::string_view channel) const {
    const Node* n = node(id);
    if (!n) return {};
    const auto it = n->exprs.find(std::string(channel));
    return it == n->exprs.end() ? std::string() : it->second;
}

bool Network::setExpression(int id, std::string_view channel, std::string_view text) {
    Node* n = node(id);
    if (!n) return false;
    const auto [base, c] = splitChannel(channel);
    const ParamDef* d = def(*n, base);
    if (!d || isText(d->kind)) return false;
    const bool vector = d->kind == ParamKind::Vector || d->kind == ParamKind::Color;
    if (vector != (c >= 0)) return false;  // a vector by its components, a number by its name
    std::string t(text);
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    const std::string key(channel);
    const auto it = n->exprs.find(key);
    if (t.empty()) {
        if (it == n->exprs.end()) return true;
        n->exprs.erase(it);
    } else {
        if (it != n->exprs.end() && it->second == t) return true;
        n->exprs[key] = t;
    }
    revision_ = nextRevision();
    return true;
}

bool Network::hasExpression(int id, std::string_view name) const {
    const Node* n = node(id);
    if (!n || n->exprs.empty()) return false;
    const ParamDef* d = def(*n, name);
    if (!d) return false;
    for (const std::string& ch : channels(*d)) {
        if (n->exprs.count(ch)) return true;
    }
    return false;
}

std::string Network::expressionError(int id, std::string_view channel, float frame) const {
    const Node* n = node(id);
    if (!n) return {};
    const auto it = n->exprs.find(std::string(channel));
    if (it == n->exprs.end()) return {};
    const Parsed p = parsedExpression(it->second);
    if (!p.expr) return p.error;
    const Evaluating guard(id, splitChannel(channel).first);
    const ExpressionHost host(*this, *n, frame, 0);
    std::string error;
    double v = 0.0;
    if (!p.expr->evalFloat(host, v, error)) return error;
    return {};
}

ParamValue Network::valueAt(int id, std::string_view name, float frame) const { return valueAtDepth(id, name, frame, 0); }

ParamValue Network::valueAtDepth(int id, std::string_view name, float frame, int depth, std::string* failure) const {
    const Node* n = node(id);
    if (!n) return {};
    const ParamDef* d = def(*n, name);
    ParamValue v = param(id, name);
    if (!d) return v;
    const auto k = n->keys.find(std::string(name));
    if (k != n->keys.end()) v = evaluate(k->second, frame, d->kind);
    if (n->exprs.empty() || isText(d->kind)) return v;
    const std::vector<std::string> chans = channels(*d);
    bool any = false;
    const Evaluating guard(id, d->name);
    for (size_t c = 0; c < chans.size(); ++c) {
        const auto it = n->exprs.find(chans[c]);
        if (it == n->exprs.end()) continue;
        const Parsed p = parsedExpression(it->second);
        if (!p.expr) {  // does not parse: the value stands
            if (failure && failure->empty()) *failure = n->name + "/" + chans[c] + ": " + p.error;
            continue;
        }
        const ExpressionHost host(*this, *n, frame, depth);
        std::string error;
        const lang::Type type = p.expr->type();
        bool ok = false;
        if (type == lang::Type::Vec2 || type == lang::Type::Vec3 || type == lang::Type::Vec4) {
            Vec3 x;
            if ((ok = p.expr->evalVector(host, x, error))) v[c] = x[static_cast<int>(c)];
        } else {
            double x = 0.0;
            if ((ok = p.expr->evalFloat(host, x, error) && std::isfinite(x))) v[c] = static_cast<float>(x);
        }
        any = any || ok;
        if (!ok && failure && failure->empty()) *failure = error.empty() ? n->name + "/" + chans[c] + ": not a number" : error;
    }
    return any ? keep(*d, v) : v;
}

bool Network::varies(int id, std::string_view name) const { return variesDepth(id, name, 0); }

bool Network::variesDepth(int id, std::string_view name, int depth) const {
    const Node* n = node(id);
    if (!n) return false;
    if (n->keys.count(std::string(name))) return true;
    if (n->exprs.empty() || depth > kDeepest) return false;
    const ParamDef* d = def(*n, name);
    if (!d) return false;
    for (const std::string& ch : channels(*d)) {
        const auto it = n->exprs.find(ch);
        if (it == n->exprs.end()) continue;
        const Parsed p = parsedExpression(it->second);
        if (!p.expr) continue;
        if (p.expr->readsTime()) return true;
        // It varies when what it reads does.
        const ExpressionHost host(*this, *n, 1.0f, depth);
        for (const lang::Channel& c : p.expr->channels()) {
            const Node* target = nullptr;
            const ParamDef* td = nullptr;
            int comp = 0;
            std::string error;
            if (host.resolve(c.name, target, td, comp, error) && variesDepth(target->id, td->name, depth + 1)) return true;
        }
    }
    return false;
}

bool Network::setKey(int id, std::string_view name, float frame, const ParamValue& value, Interp interp) {
    Node* n = node(id);
    const ParamDef* d = n ? def(*n, name) : nullptr;
    if (!d || isText(d->kind) || !std::isfinite(frame)) return false;
    std::vector<Key>& keys = n->keys[std::string(name)];
    const Key key{frame, keep(*d, value), interp};
    auto at = std::lower_bound(keys.begin(), keys.end(), frame - 1e-4f,
                               [](const Key& k, float f) { return k.frame < f; });
    if (at != keys.end() && std::fabs(at->frame - frame) <= 1e-4f) {
        if (*at == key) return true;
        *at = key;
    } else {
        keys.insert(at, key);
    }
    revision_ = nextRevision();
    return true;
}

bool Network::removeKey(int id, std::string_view name, float frame) {
    Node* n = node(id);
    if (!n) return false;
    const auto it = n->keys.find(std::string(name));
    if (it == n->keys.end()) return false;
    std::vector<Key>& keys = it->second;
    const auto at = std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return std::fabs(k.frame - frame) <= 1e-4f; });
    if (at == keys.end()) return false;
    if (keys.size() == 1) {
        // The last key: the parameter keeps its value, no longer animated.
        const ParamValue v = at->value;
        n->keys.erase(it);
        setParam(id, name, v);
    } else {
        keys.erase(at);
    }
    revision_ = nextRevision();
    return true;
}

bool Network::clearKeys(int id, std::string_view name, float frame) {
    Node* n = node(id);
    if (!n) return false;
    const auto it = n->keys.find(std::string(name));
    if (it == n->keys.end()) return false;
    const ParamValue v = valueAt(id, name, frame);
    n->keys.erase(it);
    setParam(id, name, v);
    revision_ = nextRevision();
    return true;
}

const std::vector<Key>* Network::keys(int id, std::string_view name) const {
    const Node* n = node(id);
    if (!n) return nullptr;
    const auto it = n->keys.find(std::string(name));
    return it == n->keys.end() ? nullptr : &it->second;
}

bool Network::anyAnimated() const {
    for (const Node& n : nodes_) {
        if (!n.keys.empty()) return true;
        for (const auto& [channel, text] : n.exprs) {
            if (varies(n.id, splitChannel(channel).first)) return true;
        }
    }
    return false;
}

bool Network::setParamAt(int id, std::string_view name, float frame, const ParamValue& value) {
    if (const std::vector<Key>* k = keys(id, name)) {
        // A key there: it changes; else a new one, as smooth as its neighbour.
        Interp interp = Interp::Smooth;
        for (const Key& key : *k) {
            if (key.frame <= frame + 1e-4f) interp = key.interp;
        }
        return setKey(id, name, frame, value, interp);
    }
    return setParam(id, name, value);
}

std::vector<float> Network::keyFrames(int id) const {
    std::vector<float> frames;
    for (const Node& n : nodes_) {
        if (id != 0 && n.id != id) continue;
        for (const auto& [name, keys] : n.keys) {
            for (const Key& k : keys) frames.push_back(k.frame);
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end(), [](float a, float b) { return std::fabs(a - b) <= 1e-4f; }),
                 frames.end());
    return frames;
}

bool Network::setDisplay(int id) {
    if (id != 0) {
        const Node* n = node(id);
        const NodeType* t = n ? findNodeType(n->type) : nullptr;
        if (!t || !t->core) return false;
    }
    bool changed = false;
    for (Node& n : nodes_) {
        const bool on = n.id == id;
        changed = changed || n.display != on;
        n.display = on;
    }
    if (changed) revision_ = nextRevision();
    return true;
}

int Network::displayed() const {
    for (const Node& n : nodes_) {
        if (n.display) return n.id;
    }
    return 0;
}

bool Network::setBypass(int id, bool on) {
    Node* n = node(id);
    if (!n) return false;
    if (n->bypass != on) {
        n->bypass = on;
        revision_ = nextRevision();
    }
    return true;
}

// --- files ---------------------------------------------------------------------------

std::string Network::save() const {
    std::string out = "pgsim " + std::to_string(kFormatVersion) + "\n";
    if (!asset_.name.empty()) {
        out += "asset " + asset_.name + ' ' + std::to_string(asset_.version) + ' ' + quotedPath(asset_.label) + '\n';
        if (!asset_.help.empty()) out += "help " + quotedPath(asset_.help) + '\n';
        for (const Promotion& p : asset_.promoted) {
            out += "promote " + p.node + ' ' + p.param + ' ' + p.name + ' ' + quotedPath(p.label) + '\n';
        }
    }
    for (const Node& n : nodes_) {
        out += "node " + std::to_string(n.id) + ' ' + n.type + ' ' + std::to_string(n.version) + ' ' + n.name + ' ' +
               formatNumber(n.x) + ' ' + formatNumber(n.y) + '\n';
        const NodeType* t = findNodeType(n.type);
        if (t) {
            // In the order of the type's table: the order the editor shows.
            for (const ParamDef& d : t->params) {
                if (isText(d.kind)) {
                    const auto text = n.texts.find(d.name);
                    if (text != n.texts.end()) out += std::string("  param ") + d.name + ' ' + quotedPath(text->second) + '\n';
                    continue;
                }
                const auto it = n.params.find(d.name);
                if (it != n.params.end()) out += std::string("  param ") + d.name + ' ' + formatParam(d, it->second) + '\n';
            }
            // What the snippet asks for, after them.
            for (const ParamDef& d : n.spares) {
                if (isText(d.kind)) {
                    const auto text = n.texts.find(d.name);
                    if (text != n.texts.end()) out += std::string("  param ") + d.name + ' ' + quotedPath(text->second) + '\n';
                    continue;
                }
                const auto it = n.params.find(d.name);
                if (it != n.params.end()) out += std::string("  param ") + d.name + ' ' + formatParam(d, it->second) + '\n';
            }
            // The keys, parameter by parameter in the same order.
            auto keysOf = [&](const ParamDef& d) {
                const auto it = n.keys.find(d.name);
                if (it == n.keys.end()) return;
                for (const Key& k : it->second) {
                    out += std::string("  key ") + d.name + ' ' + formatNumber(k.frame) + ' ' + interpName(k.interp) + ' ' +
                           formatParam(d, k.value) + '\n';
                }
            };
            for (const ParamDef& d : t->params) keysOf(d);
            for (const ParamDef& d : n.spares) keysOf(d);
            // The expressions, channel by channel in the same order.
            auto exprsOf = [&](const ParamDef& d) {
                for (const std::string& ch : channels(d)) {
                    const auto it = n.exprs.find(ch);
                    if (it != n.exprs.end()) out += "  expr " + ch + ' ' + quotedPath(it->second) + '\n';
                }
            };
            for (const ParamDef& d : t->params) exprsOf(d);
            for (const ParamDef& d : n.spares) exprsOf(d);
        } else {
            // A type this program does not know: its values as they came.
            for (const auto& [name, v] : n.params) {
                out += "  param " + name + ' ' + formatNumber(v[0]) + ' ' + formatNumber(v[1]) + ' ' +
                       formatNumber(v[2]) + '\n';
            }
        }
        if (n.bypass) out += "  bypass\n";
        if (n.display) out += "  display\n";
    }
    for (const Link& l : links_) {
        out += "link " + std::to_string(l.from) + '.' + l.output + " -> " + std::to_string(l.to) + '.' + l.input + '\n';
    }
    // The digital assets it uses, as they are now: the file is whole without the library.
    std::set<std::string> used;
    for (const Node& n : nodes_) {
        if (used.count(n.type)) continue;
        const auto def = AssetLibrary::instance().find(n.type);
        if (!def) continue;
        used.insert(n.type);
        out += "definition " + n.type + '\n';
        const std::string text = def->net->save();
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t end = std::min(text.find('\n', pos), text.size());
            out += "| " + text.substr(pos, end - pos) + '\n';
            pos = end + 1;
        }
        out += "end\n";
    }
    return out;
}

bool Network::load(std::string_view whole, Network& out, std::string& error, std::vector<std::string>* warnings) {
    // The definitions of the assets it uses, first: its nodes are of their types.
    std::string rest;
    {
        size_t pos = 0;
        int lineNo = 0;
        while (pos < whole.size()) {
            const size_t end = std::min(whole.find('\n', pos), whole.size());
            const std::string_view line = whole.substr(pos, end - pos);
            pos = end + 1;
            ++lineNo;
            if (line.rfind("definition ", 0) != 0) {
                rest.append(line);
                rest.push_back('\n');
                continue;
            }
            // definition NAME, then its file a line each after "| ", then end
            const int first = lineNo;
            std::string inner;
            bool closed = false;
            while (pos < whole.size()) {
                const size_t e = std::min(whole.find('\n', pos), whole.size());
                const std::string_view l = whole.substr(pos, e - pos);
                pos = e + 1;
                ++lineNo;
                rest.push_back('\n');  // the line numbers of the rest stay
                if (l == "end" || l == "end\r") {
                    closed = true;
                    break;
                }
                if (l.rfind("|", 0) == 0) inner.append(l.substr(l.size() > 1 && l[1] == ' ' ? 2 : 1));
                inner.push_back('\n');
            }
            rest.push_back('\n');
            if (!closed) {
                error = "line " + std::to_string(first) + ": a definition without its end";
                return false;
            }
            Network def;
            std::string why;
            if (!load(inner, def, why) || !AssetLibrary::instance().addIfNewer(def, why)) {
                if (warnings) warnings->push_back("line " + std::to_string(first) + ": the asset " +
                                                  std::string(line.substr(11)) + " it carries: " + why);
            }
        }
    }
    const std::string_view text = rest;
    Network net;
    auto warn = [&](int line, const std::string& what) {
        if (warnings) warnings->push_back("line " + std::to_string(line) + ": " + what);
    };
    auto fail = [&](int line, const std::string& what) {
        error = "line " + std::to_string(line) + ": " + what;
        return false;
    };
    // Links go in once every node is known.
    struct PendingLink {
        int line;
        Link link;
    };
    std::vector<PendingLink> links;
    // Values of the parameters a snippet asks for: set once the snippet is
    // read, whatever line it is on.
    struct PendingValue {
        int line;
        int node;
        bool key;
        std::string name, value;
        float frame = 1.0f;
        Interp interp = Interp::Smooth;
        bool expr = false;
    };
    std::vector<PendingValue> spareValues;
    bool header = false;
    Node* current = nullptr;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (const size_t hash = commentStart(line); hash != std::string_view::npos) line = line.substr(0, hash);
        const std::vector<std::string_view> w = splitWords(line);
        if (w.empty()) {
            if (end == text.size()) break;
            continue;
        }
        if (!header) {
            float version = 0.0f;
            if (w.size() != 2 || w[0] != "pgsim" || !parseNumber(w[1], version)) {
                return fail(lineNo, "not a simulation network: it starts with 'pgsim 1'");
            }
            if (version > static_cast<float>(kFormatVersion)) {
                return fail(lineNo, "written by a newer version (format " + std::string(w[1]) + ")");
            }
            header = true;
            continue;
        }
        if (w[0] == "asset") {
            // asset NAME VERSION "LABEL"
            float version = 0.0f;
            if (w.size() < 3 || !parseNumber(w[2], version)) return fail(lineNo, "an asset line is: asset NAME VERSION \"LABEL\"");
            net.asset_.name = w[1];
            net.asset_.version = std::max(1, static_cast<int>(version));
            if (w.size() > 3) unquoted(line.substr(static_cast<size_t>(w[3].data() - line.data())), net.asset_.label);
            continue;
        }
        if (w[0] == "help") {
            if (w.size() > 1) unquoted(line.substr(static_cast<size_t>(w[1].data() - line.data())), net.asset_.help);
            continue;
        }
        if (w[0] == "promote") {
            // promote NODE PARAM NAME "LABEL"
            if (w.size() < 4) return fail(lineNo, "a promote line is: promote NODE PARAM NAME \"LABEL\"");
            Promotion p{std::string(w[1]), std::string(w[2]), std::string(w[3]), {}};
            if (w.size() > 4) unquoted(line.substr(static_cast<size_t>(w[4].data() - line.data())), p.label);
            net.asset_.promoted.push_back(std::move(p));
            continue;
        }
        if (w[0] == "node") {
            float id = 0.0f, version = 0.0f;
            Node n;
            if (w.size() != 7 || !parseNumber(w[1], id) || id < 1.0f || id > 1e7f || !parseNumber(w[3], version) ||
                !parseNumber(w[5], n.x) || !parseNumber(w[6], n.y)) {
                return fail(lineNo, "a node line is: node ID TYPE VERSION NAME X Y");
            }
            n.id = static_cast<int>(id);
            if (net.node(n.id)) return fail(lineNo, "two nodes have the id " + std::string(w[1]));
            n.type = w[2];
            n.version = static_cast<int>(version);
            n.name = w[4];
            if (!validName(n.name)) {
                warn(lineNo, "'" + n.name + "' is not a name; renamed");
                n.name = net.uniqueName(n.type);
            } else if (net.named(n.name)) {
                const std::string again = net.uniqueName(n.name);
                warn(lineNo, "two nodes are called " + n.name + "; the second is now " + again);
                n.name = again;
            }
            net.nextId_ = std::max(net.nextId_, n.id + 1);
            net.nodes_.push_back(std::move(n));
            current = &net.nodes_.back();
            continue;
        }
        if (w[0] == "param") {
            if (!current) return fail(lineNo, "a param belongs to the node above it, and there is none");
            if (w.size() < 3) return fail(lineNo, "a param line is: param NAME VALUE");
            const std::string name(w[1]);
            const std::string_view value = line.substr(static_cast<size_t>(w[2].data() - line.data()));
            // A node of an old type reads by the parameters it had then.
            const Legacy* legacy = legacyType(current->type, current->version);
            const NodeType* t = legacy ? &legacy->type : findNodeType(current->type);
            if (!t) {
                // Kept as numbers, for a newer program to read back.
                ParamValue v{};
                const auto words = splitWords(value);
                for (size_t i = 0; i < std::min<size_t>(3, words.size()); ++i) parseNumber(words[i], v[i]);
                current->params[name] = v;
                continue;
            }
            const ParamDef* d = t->param(name);
            if (!d) {
                spareValues.push_back({lineNo, current->id, false, name, std::string(value)});
                continue;
            }
            if (isText(d->kind)) {
                std::string path;
                if (!unquoted(value, path)) {
                    warn(lineNo, current->name + ": " + name + " has a quote that is not closed; left at the default");
                } else if (path != (d->text ? d->text : "")) {
                    current->texts[name] = path;
                }
                continue;
            }
            ParamValue v;
            std::string why;
            if (!parseParam(*d, value, v, why)) {
                warn(lineNo, current->name + ": " + why + "; left at the default");
                continue;
            }
            if (v != d->value) current->params[name] = v;
            continue;
        }
        if (w[0] == "key") {
            // key NAME FRAME INTERP VALUE
            if (!current) return fail(lineNo, "a key belongs to the node above it, and there is none");
            if (w.size() < 5) return fail(lineNo, "a key line is: key NAME FRAME smooth|linear|step VALUE");
            const NodeType* t = findNodeType(current->type);
            const ParamDef* d = t ? t->param(w[1]) : nullptr;
            float frame = 0.0f;
            Interp interp = Interp::Smooth;
            const bool known = w[3] == "smooth" || w[3] == "linear" || w[3] == "step";
            if (known) interp = w[3] == "linear" ? Interp::Linear : w[3] == "step" ? Interp::Step : Interp::Smooth;
            if (!d && t && known && parseNumber(w[2], frame)) {
                spareValues.push_back({lineNo, current->id, true, std::string(w[1]),
                                       std::string(line.substr(static_cast<size_t>(w[4].data() - line.data()))), frame, interp});
                continue;
            }
            if (!d || isText(d->kind)) {
                warn(lineNo, current->name + " has no parameter " + std::string(w[1]) + " to animate; key dropped");
                continue;
            }
            if (!parseNumber(w[2], frame) || !known) {
                warn(lineNo, current->name + ": a key is: key NAME FRAME smooth|linear|step VALUE; dropped");
                continue;
            }
            ParamValue v;
            std::string why;
            const std::string_view value = line.substr(static_cast<size_t>(w[4].data() - line.data()));
            if (!parseParam(*d, value, v, why)) {
                warn(lineNo, current->name + ": " + why + "; key dropped");
                continue;
            }
            std::vector<Key>& keys = current->keys[std::string(w[1])];
            auto at = std::lower_bound(keys.begin(), keys.end(), frame, [](const Key& k, float f) { return k.frame < f; });
            if (at != keys.end() && std::fabs(at->frame - frame) <= 1e-4f) *at = {frame, v, interp};
            else keys.insert(at, {frame, v, interp});
            continue;
        }
        if (w[0] == "expr") {
            // expr CHANNEL "TEXT" -- set once every parameter is known
            if (!current) return fail(lineNo, "an expr belongs to the node above it, and there is none");
            if (w.size() < 3) return fail(lineNo, "an expr line is: expr PARAM \"EXPRESSION\"");
            std::string text;
            const std::string_view value = line.substr(static_cast<size_t>(w[2].data() - line.data()));
            if (!unquoted(value, text)) {
                warn(lineNo, current->name + ": an expression with a quote that is not closed; dropped");
                continue;
            }
            PendingValue pv{lineNo, current->id, false, std::string(w[1]), text};
            pv.expr = true;
            spareValues.push_back(std::move(pv));
            continue;
        }
        if (w[0] == "bypass") {
            if (!current) return fail(lineNo, "bypass belongs to the node above it, and there is none");
            current->bypass = true;
            continue;
        }
        if (w[0] == "display") {
            if (!current) return fail(lineNo, "display belongs to the node above it, and there is none");
            const NodeType* t = findNodeType(current->type);
            if (!t || !t->core) {
                warn(lineNo, current->name + " has no geometry to display; the flag is left off");
                continue;
            }
            for (Node& other : net.nodes_) other.display = false;  // one at a time: the last one says
            current->display = true;
            continue;
        }
        if (w[0] == "link") {
            // link A.out -> B.in
            auto pin = [](std::string_view s, int& node, std::string& name) {
                const size_t dot = s.find('.');
                float id = 0.0f;
                if (dot == std::string_view::npos || !parseNumber(s.substr(0, dot), id)) return false;
                node = static_cast<int>(id);
                name = s.substr(dot + 1);
                return !name.empty();
            };
            Link l;
            if (w.size() != 4 || w[2] != "->" || !pin(w[1], l.from, l.output) || !pin(w[3], l.to, l.input)) {
                return fail(lineNo, "a link line is: link NODE.output -> NODE.input");
            }
            links.push_back({lineNo, l});
            continue;
        }
        return fail(lineNo, "unknown line '" + std::string(w[0]) + "'");
    }
    if (!header) return fail(1, "empty: a network starts with 'pgsim 1'");
    for (Node& n : net.nodes_) syncSpares(n);
    for (const PendingValue& p : spareValues) {
        Node* n = net.node(p.node);
        if (p.expr) {
            if (!n || !net.setExpression(n->id, p.name, p.value)) {
                warn(p.line, (n ? n->name : std::string("?")) + " has no parameter " + p.name + " to drive; expression dropped");
            }
            continue;
        }
        const ParamDef* d = n ? net.def(*n, p.name) : nullptr;
        if (!d) {
            const NodeType* t = n ? findNodeType(n->type) : nullptr;
            warn(p.line, (n ? n->name : std::string("?")) + " (" + (t ? t->label : "?") + ") has no parameter " + p.name +
                             (p.key ? " to animate; key dropped" : "; dropped"));
            continue;
        }
        if (isText(d->kind)) {
            std::string text;
            if (p.key || !unquoted(p.value, text)) {
                warn(p.line, n->name + ": " + p.name + " is left at the default");
            } else if (!text.empty()) {
                n->texts[p.name] = text;
            }
            continue;
        }
        ParamValue v;
        std::string why;
        if (!parseParam(*d, p.value, v, why)) {
            warn(p.line, n->name + ": " + why + (p.key ? "; key dropped" : "; left at the default"));
            continue;
        }
        if (p.key) {
            std::vector<Key>& keys = n->keys[p.name];
            auto at = std::lower_bound(keys.begin(), keys.end(), p.frame, [](const Key& k, float f) { return k.frame < f; });
            if (at != keys.end() && std::fabs(at->frame - p.frame) <= 1e-4f) *at = {p.frame, v, p.interp};
            else keys.insert(at, {p.frame, v, p.interp});
        } else if (v != d->value) {
            n->params[p.name] = v;
        }
    }
    std::vector<Link> pending;
    for (const PendingLink& p : links) pending.push_back(p.link);
    net.upgrade(pending);
    for (const PendingLink& p : links) {
        const Node* a = net.node(p.link.from);
        const Node* b = net.node(p.link.to);
        if (!a || !b) {
            warn(p.line, "a link to a node that is not there; dropped");
            continue;
        }
        if (!findNodeType(a->type) || !findNodeType(b->type)) {
            net.links_.push_back(p.link);  // cannot be checked; kept for whoever knows the type
            continue;
        }
        std::string why;
        if (!net.connect(p.link.from, p.link.output, p.link.to, p.link.input, &why)) {
            warn(p.line, "link " + a->name + "." + p.link.output + " -> " + b->name + "." + p.link.input + ": " + why +
                             "; dropped");
        }
    }
    // Promoted parameters that are not there.
    std::erase_if(net.asset_.promoted, [&](const Promotion& p) {
        const Node* n = net.named(p.node);
        if (n && net.def(*n, p.param)) return false;
        if (warnings) warnings->push_back("the promoted " + p.node + "." + p.param + " is not there; dropped");
        return true;
    });
    net.revision_ = nextRevision();
    out = std::move(net);
    return true;
}

uint64_t Network::nextRevision() {
    static std::atomic<uint64_t> last{0};
    return last.fetch_add(1, std::memory_order_relaxed) + 1;
}

void Network::upgrade(const std::vector<Link>& links) {
    for (Node& n : nodes_) {
        if (const Legacy* l = legacyType(n.type, n.version)) l->upgrade(n, nodes_, links);
    }
}

// --- compile -------------------------------------------------------------------------

bool Compiled::errors() const {
    return std::any_of(problems.begin(), problems.end(), [](const Problem& p) { return p.level == Problem::Level::Error; });
}

bool Compiled::isActive(int node) const { return std::binary_search(active.begin(), active.end(), node); }

const Look& Compiled::lookAt(int frame) const {
    return poses.empty() ? look : poses[static_cast<size_t>(std::clamp(frame, 1, static_cast<int>(poses.size())) - 1)].look;
}

const std::vector<Solid>& Compiled::solidsAt(int frame) const {
    return poses.empty() ? solids : poses[static_cast<size_t>(std::clamp(frame, 1, static_cast<int>(poses.size())) - 1)].solids;
}

const Camera& Compiled::cameraAt(int frame) const {
    return poses.empty() ? camera : poses[static_cast<size_t>(std::clamp(frame, 1, static_cast<int>(poses.size())) - 1)].camera;
}

/// What compiling a network once for each frame shares: meshes read and
/// geometry cooked once.
struct Network::CompileMemo {
    std::map<int, std::shared_ptr<const MeshShape>> meshes;  // from files, by node
    std::map<int, std::shared_ptr<const MeshShape>> shapes;  // from geometry, by node
    std::map<int, std::shared_ptr<const Geometry>> pieces;   // an RBD Solver's, by node
    std::map<int, std::shared_ptr<const Geometry>> rebar;    // ... and its bars
    std::map<int, std::shared_ptr<const Geometry>> constraints;  // ... and its network of glue
    std::map<int, RigidGuideRest> guideRest;                     // ... and its pieces as a guide reads them
    std::map<int, std::shared_ptr<const Geometry>> cloth;    // a Cloth Solver's geometry at frame 1, by node
    std::unique_ptr<GeometryGraph> own;
    GeometryGraph* cooker = nullptr;
};

namespace {

/// How many lines of a network of glue join a piece that `pieces` does not
/// have: an end whose point names -- in `attribute` -- a piece none of them
/// is. None where its points name no pieces: those go to the nearest.
size_t strayJoints(const Geometry& network, const Geometry* pieces, const std::string& attribute) {
    const AttributeArray* names = network.points().find(attribute);
    if (!pieces || !names || (names->type() != AttrType::Int && names->type() != AttrType::Float)) return 0;
    // The pieces there are, as the solver numbers them (pieceOfPrimitives).
    std::set<int32_t> have;
    if (const AttributeArray* a = pieces->primitives().find(attribute); a && a->type() == AttrType::Int) {
        for (const int32_t v : a->read<int32_t>()) have.insert(v);
    } else if (const AttributeArray* b = pieces->points().find(attribute); b && b->type() == AttrType::Int) {
        const auto v = b->read<int32_t>();
        for (size_t p = 0; p < pieces->primitiveCount(); ++p) {
            const auto c = pieces->primitivePoints(p);
            if (!c.empty()) have.insert(v[c[0]]);
        }
    } else {
        int count = 0;
        pieceOfPrimitives(*pieces, attribute, count);
        for (int i = 0; i < count; ++i) have.insert(i);
    }
    auto named = [&](uint32_t point) {
        const int32_t v = names->type() == AttrType::Int ? names->read<int32_t>()[point]
                                                         : static_cast<int32_t>(std::lround(names->read<float>()[point]));
        return have.count(v) > 0;
    };
    size_t lost = 0;
    for (size_t prim = 0; prim < network.primitiveCount(); ++prim) {
        const auto c = network.primitivePoints(prim);
        if (c.size() < 2 || !named(c.front()) || !named(c.back())) ++lost;
    }
    return lost;
}

/// How far and which way a rotation turns into the next in `dt`: the axis,
/// as long as radians per second.
Vec3 spinBetween(const Vec3& fromDegrees, const Vec3& toDegrees, float dt) {
    if (fromDegrees == toDegrees || dt <= 0.0f) return {};
    const Rotation a = Rotation::fromEuler(fromDegrees), b = Rotation::fromEuler(toDegrees);
    // The turn from a to b, b a^T; GLM indexes [column][row].
    const Mat3 m = b.matrix() * glm::transpose(a.matrix());
    const float cosine = std::clamp(0.5f * (m[0][0] + m[1][1] + m[2][2] - 1.0f), -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    const Vec3 axis(m[1][2] - m[2][1], m[2][0] - m[0][2], m[0][1] - m[1][0]);  // 2 sin(angle) along the axis
    const float twiceSine = length(axis);
    if (twiceSine < 1e-7f) return {};
    return axis * (angle / (twiceSine * dt));
}

}  // namespace

Compiled Network::compile(const std::string& folder, GeometryGraph* geometry) const {
    CompileMemo memo;
    Compiled c = compileFrame(folder, geometry, 1.0f, memo, false);
    if (!anyAnimated() && !c.fileAnimation && !c.guideMoves && !c.clothMoves) return c;

    // Animated: the network at every frame.
    const int count = std::max(1, c.frames);
    const float dt = c.world.timeStep;
    auto track = std::make_shared<std::vector<World>>();
    track->reserve(static_cast<size_t>(count));
    c.poses.reserve(static_cast<size_t>(count));
    for (int f = 1; f <= count; ++f) {
        Compiled at = f == 1 ? c : compileFrame(folder, geometry, static_cast<float>(f), memo, true);
        World w = std::move(at.world);
        // The grids, the frame rate and what is simulated are frame 1's: they
        // cannot change as the simulation runs.
        w.animation = {};
        w.timeStep = c.world.timeStep;
        w.hasGas = c.world.hasGas;
        w.hasWater = c.world.hasWater;
        w.hasRain = c.world.hasRain;
        w.hasRigid = c.world.hasRigid;
        w.keepParticles = c.world.keepParticles;
        w.gas.solver.size = c.world.gas.solver.size;
        w.gas.solver.resolution = c.world.gas.solver.resolution;
        w.water.solver.size = c.world.water.solver.size;
        w.water.solver.resolution = c.world.water.solver.resolution;
        w.water.solver.closedSides = c.world.water.solver.closedSides;
        track->push_back(std::move(w));
        c.poses.push_back({std::move(at.look), std::move(at.solids), at.camera});
    }
    // A camera from a file turns as the file says; of the angles that say
    // it, those nearest the frame before -- no flips from 180 to -180.
    if (const Node* cam = node(c.camera.node); cam && cam->type == "usd_camera") {
        for (size_t k = 1; k < c.poses.size(); ++k) {
            Camera& now = c.poses[k].camera;
            now.rotation = Camera::rotationFor(now.forward(), now.up(), c.poses[k - 1].camera.rotation);
        }
    }
    // How what is animated moves: from each frame to the next, the one
    // before it -- frame 1 as frame 2 will find it.
    auto previous = [&](size_t k) { return k == 0 ? std::min<size_t>(1, track->size() - 1) : k - 1; };
    auto sign = [](size_t k) { return k == 0 ? -1.0f : 1.0f; };
    auto colliders = [&](auto member) {
        for (size_t k = 0; k < track->size(); ++k) {
            std::vector<Collider>& now = member((*track)[k]);
            const std::vector<Collider>& before = member((*track)[previous(k)]);
            for (Collider& col : now) {
                for (const Collider& b : before) {
                    if (b.node != col.node || col.node == 0) continue;
                    col.velocity = (col.center - b.center) * (sign(k) / dt);
                    col.spin = sign(k) > 0.0f ? spinBetween(b.rotation, col.rotation, dt) : spinBetween(col.rotation, b.rotation, dt);
                    break;
                }
            }
        }
    };
    colliders([](World& w) -> std::vector<Collider>& { return w.gas.colliders; });
    colliders([](World& w) -> std::vector<Collider>& { return w.water.colliders; });
    colliders([](World& w) -> std::vector<Collider>& { return w.rain.colliders; });
    colliders([](World& w) -> std::vector<Collider>& { return w.rigid.colliders; });
    for (size_t k = 0; k < track->size(); ++k) {
        World& now = (*track)[k];
        const World& before = (*track)[previous(k)];
        for (Emitter& e : now.gas.emitters) {
            for (const Emitter& b : before.gas.emitters) {
                if (b.node == e.node && e.node != 0) e.moving = (e.center - b.center) * (sign(k) / dt);
            }
        }
        for (WaterSource& s : now.water.sources) {
            for (const WaterSource& b : before.water.sources) {
                if (b.node == s.node && s.node != 0) s.moving = (s.center - b.center) * (sign(k) / dt);
            }
        }
    }
    // The objects as they are drawn move too.
    for (size_t k = 0; k < c.poses.size(); ++k) {
        for (Solid& s : c.poses[k].solids) {
            for (const Collider& col : (*track)[k].gas.colliders) {
                if (col.node == s.body.node) s.body = col;
            }
        }
    }
    // Only the look or the camera animated: what is simulated is the same
    // all along, and needs no frames of its own.
    const bool same = std::all_of(track->begin(), track->end(), [&](const World& w) { return w == track->front(); });
    if (!same) c.world.animation.frames = track;
    c.world.gas = track->front().gas;
    c.world.water = track->front().water;
    c.world.rain = track->front().rain;
    c.world.rigid = track->front().rigid;

    // What cannot be animated is said.
    for (const Node& n : nodes_) {
        std::set<std::string> changing;
        for (const auto& [name, keys] : n.keys) changing.insert(name);
        for (const auto& [channel, text] : n.exprs) {
            const std::string name = splitChannel(channel).first;
            if (varies(n.id, name)) changing.insert(name);
        }
        for (const std::string& name : changing) {
            const bool fixed = (n.type == "pyro_solver" && (name == "size" || name == "resolution")) ||
                               (n.type == "liquid_solver" &&
                                (name == "size" || name == "resolution" || name == "closed_sides" || name == "sparse")) ||
                               (n.type == "output" && (name == "frames" || name == "fps")) ||
                               (n.type == "rbd_solver" && name != "color" && name != "inside_color" && name != "rebar_color") ||
                               (n.type == "cloth_solver" && name != "color");
            if (fixed) {
                c.problems.push_back({Problem::Level::Warning, n.id,
                                      "'" + name + "' cannot change as the simulation runs: its value at frame 1 holds."});
            }
        }
    }
    return c;
}

Compiled Network::compileFrame(const std::string& folder, GeometryGraph* geometry, float frame, CompileMemo& memo,
                               bool quiet) const {
    Compiled c;
    auto problem = [&](Problem::Level level, int node, std::string message) {
        if (!quiet) c.problems.push_back({level, node, std::move(message)});
    };
    using L = Problem::Level;
    auto f = [&](const Node& n, const char* name) { return valueAt(n.id, name, frame)[0]; };
    auto v3 = [&](const Node& n, const char* name) {
        const ParamValue p = valueAt(n.id, name, frame);
        return Vec3(p[0], p[1], p[2]);
    };
    auto whole = [&](const Node& n, const char* name) { return static_cast<int>(std::lround(f(n, name))); };
    // A mesh from its file, read once per compile -- and once in all while
    // it is in use (loadMesh); a relative path from the network's folder.
    std::map<int, std::shared_ptr<const MeshShape>>& meshes = memo.meshes;
    auto meshOf = [&](const Node& n) -> std::shared_ptr<const MeshShape> {
        if (static_cast<Shape>(whole(n, "shape")) != Shape::Mesh) return nullptr;
        if (const auto it = meshes.find(n.id); it != meshes.end()) return it->second;
        std::shared_ptr<const MeshShape> mesh;
        const std::string name = text(n.id, "file");
        if (name.empty()) {
            problem(L::Warning, n.id, "The shape is a mesh: choose its OBJ file.");
        } else {
            std::filesystem::path path(name);
            if (path.is_relative() && !folder.empty()) path = std::filesystem::path(folder) / path;
            std::string why;
            mesh = loadMesh(path.string(), why);
            if (!mesh) problem(L::Warning, n.id, "The mesh cannot be read -- " + why + ". Its box stands in.");
        }
        meshes[n.id] = mesh;
        return mesh;
    };
    // Geometry linked into a Shape: cooked at frame 1, the first time one is
    // asked for -- in the editor's graph, or in one of our own.
    std::unique_ptr<GeometryGraph>& own = memo.own;
    GeometryGraph*& cooker = memo.cooker;
    float firstStep = 1.0f / 30.0f;
    for (const Node& n : nodes_) {
        if (n.type == "output") {
            firstStep = 1.0f / std::max(param(n.id, "fps")[0], 1.0f);
            break;
        }
    }
    // Whether geometry comes from a simulation: something upstream brings one back.
    auto fromSimulation = [&](int id) {
        std::vector<int> stack{id}, seen{id};
        while (!stack.empty()) {
            const Node* n = node(stack.back());
            stack.pop_back();
            if (!n) continue;
            if (n->type == "liquid_points" || n->type == "liquid_surface" || n->type == "rain_points" ||
                n->type == "gas_volume" || n->type == "rbd_pieces" || n->type == "cloth_geometry") {
                return true;
            }
            for (const Link& l : links_) {
                if (l.to == n->id && std::find(seen.begin(), seen.end(), l.from) == seen.end()) {
                    seen.push_back(l.from);
                    stack.push_back(l.from);
                }
            }
        }
        return false;
    };
    auto startCooker = [&]() {
        if (cooker) return;
        if (!geometry) own = std::make_unique<GeometryGraph>();
        cooker = geometry ? geometry : own.get();
        cooker->sync(*this, folder);
    };
    std::map<int, std::shared_ptr<const MeshShape>>& shapes = memo.shapes;
    auto geometryShape = [&](const Node& n) -> std::shared_ptr<const MeshShape> {
        const std::vector<Link> in = linksInto(n.id, "shape");
        if (in.empty()) return nullptr;
        if (const auto it = shapes.find(n.id); it != shapes.end()) return it->second;
        startCooker();
        std::shared_ptr<const MeshShape> mesh;
        const GeometryPtr geo = cooker->cook(in.front().from, 1, firstStep);
        const std::string error = cooker->error(in.front().from);
        if (!error.empty()) problem(L::Warning, in.front().from, error);
        if (fromSimulation(in.front().from)) {
            problem(L::Warning, n.id, "Its shape comes from a simulation, which has not run when shapes are made: "
                                      "its own shape stands in.");
        } else if (geo) {
            mesh = meshFromGeometry(*geo);
            if (!mesh) problem(L::Warning, n.id, "The geometry linked into Shape is empty: its own shape stands in.");
            else if (geo->primitiveCount() == 0 && geo->pointCount() > kMaxShapePoints) {
                problem(L::Warning, n.id, "More than " + std::to_string(kMaxShapePoints) +
                                              " points: balls round the first " + std::to_string(kMaxShapePoints) +
                                              " only.");
            }
        }
        shapes[n.id] = mesh;
        return mesh;
    };
    // Where a shape is: its own parameters -- or the geometry's, where it is.
    struct Placement {
        Shape shape;
        Vec3 center, rotation, size;
        std::shared_ptr<const MeshShape> mesh;
    };
    auto placementOf = [&](const Node& n) {
        if (auto mesh = geometryShape(n)) return Placement{Shape::Mesh, mesh->center(), Vec3(), mesh->half() * 2.0f, mesh};
        return Placement{static_cast<Shape>(whole(n, "shape")), v3(n, "center"), v3(n, "rotation"), v3(n, "size"), meshOf(n)};
    };
    auto colliderOf = [&](const Node& n) {
        Collider col;
        const Placement p = placementOf(n);
        col.shape = p.shape;
        col.center = p.center;
        col.rotation = p.rotation;
        col.size = p.size;
        col.mesh = p.mesh;
        col.node = n.id;
        return col;
    };
    // Every return goes through here: `active` is searched, so sorted.
    c.display = displayed();
    c.model = c.display && std::all_of(nodes_.begin(), nodes_.end(), [](const Node& n) {
        const NodeType* t = findNodeType(n.type);
        return n.bypass || !t || t->core || n.type == "output" || n.type == "camera" || n.type == "usd_camera" ||
               n.type == "object";
    });
    auto done = [&]() {
        // What feeds geometry that takes part -- into a shape, or shown --
        // takes part too.
        std::vector<int> stack = c.active;
        if (c.display) stack.push_back(c.display);
        std::vector<int> seen = stack;
        while (!stack.empty()) {
            const int id = stack.back();
            stack.pop_back();
            const Node* to = node(id);
            const NodeType* t = to ? findNodeType(to->type) : nullptr;
            if (!t) continue;
            for (const Link& l : links_) {
                if (l.to != id) continue;
                const PinDef* pin = t->input(l.input);
                if (!pin || pin->type != PinType::Geometry) continue;
                if (std::find(seen.begin(), seen.end(), l.from) != seen.end()) continue;
                seen.push_back(l.from);
                stack.push_back(l.from);
                c.active.push_back(l.from);
            }
        }
        if (c.display) c.active.push_back(c.display);
        std::sort(c.active.begin(), c.active.end());
        c.active.erase(std::unique(c.active.begin(), c.active.end()), c.active.end());
        return c;
    };

    for (const Node& n : nodes_) {
        if (!findNodeType(n.type)) problem(L::Error, n.id, "Unknown node type '" + n.type + "' -- from a newer version?");
    }

    // The objects: all of them are in the scene, drawn, whatever feeds what.
    for (const Node& n : nodes_) {
        if (n.type != "object" || n.bypass) continue;
        c.solids.push_back({colliderOf(n), v3(n, "color"), static_cast<Matte>(std::clamp(whole(n, "matte"), 0, 2))});
        c.active.push_back(n.id);
    }

    // Output <- Volume Look <- Pyro Solver: the spine.
    const Node* output = nullptr;
    for (const Node& n : nodes_) {
        if (n.type != "output") continue;
        if (!output) output = &n;
        else problem(L::Warning, n.id, "Another Output: only " + output->name + " is used.");
    }
    if (!output) {
        if (!c.model) problem(L::Error, 0, "No Output node. Add one (Render > Output) and link a Volume Look into it.");
        return done();
    }
    c.output = output->id;
    c.active.push_back(output->id);
    c.frames = std::max(1, whole(*output, "frames"));

    auto upstream = [&](const Node& n, const char* input) -> const Node* {
        const std::vector<Link> in = linksInto(n.id, input);
        return in.empty() ? nullptr : node(in.front().from);
    };
    // The sun, the sky, the image and the frame rate: the Output's, for all.
    c.world.timeStep = 1.0f / std::max(f(*output, "fps"), 1.0f);
    Look& k = c.look;
    k.lightAzimuth = f(*output, "light_azimuth");
    k.lightElevation = f(*output, "light_elevation");
    k.lightColor = v3(*output, "light_color");
    k.lightIntensity = f(*output, "light_intensity");
    k.skyColor = v3(*output, "sky_color");
    k.skyIntensity = f(*output, "sky_intensity");
    k.exposure = f(*output, "exposure");
    k.floor = f(*output, "floor") != 0.0f;
    k.groundColor = v3(*output, "ground_color");
    k.grid = f(*output, "grid") != 0.0f;
    k.skyBehind = f(*output, "sky_behind") != 0.0f;
    k.floorMatte = static_cast<Matte>(std::clamp(whole(*output, "floor_matte"), 0, 2));
    // How the path tracer renders it.
    render::Settings& r = c.render;
    r.samples = std::max(1, whole(*output, "render_samples"));
    r.bounces = std::clamp(whole(*output, "render_bounces"), 0, 64);
    r.denoise = f(*output, "render_denoise") != 0.0f;
    r.fstop = std::max(f(*output, "render_fstop"), 0.0f);
    r.focus = std::max(f(*output, "render_focus"), 0.0f);
    r.clamp = std::max(f(*output, "render_clamp"), 0.01f);
    r.sunAngle = std::clamp(f(*output, "render_sun_angle"), 0.01f, 30.0f);
    r.shutter = std::clamp(f(*output, "render_motion_blur"), 0.0f, 1.0f);
    r.sky = static_cast<render::Settings::Sky>(std::clamp(whole(*output, "render_sky"), 0, 2));
    r.skyImage = text(output->id, "render_sky_image");
    if (!r.skyImage.empty() && !folder.empty() && std::filesystem::path(r.skyImage).is_relative()) {
        r.skyImage = (std::filesystem::path(folder) / r.skyImage).lexically_normal().string();
    }
    if (r.sky == render::Settings::Sky::Image && frame <= 1.0f) {
        std::error_code ec;
        if (r.skyImage.empty()) {
            problem(L::Warning, output->id, "Sky is Image, but there is no Sky Image: the physical sky lights it");
        } else if (!std::filesystem::is_regular_file(r.skyImage, ec)) {
            problem(L::Warning, output->id, "no sky image: " + r.skyImage + " is not there");
        }
    }
    r.skyRotation = f(*output, "render_sky_rotation");
    r.skyStrength = std::max(f(*output, "render_sky_strength"), 0.0f);
    r.skySun = f(*output, "render_sky_sun") != 0.0f;
    r.clouds = std::clamp(f(*output, "render_clouds"), 0.0f, 1.0f);
    r.cloudSize = std::clamp(f(*output, "render_cloud_size"), 0.01f, 100.0f);
    r.cloudWind = std::max(f(*output, "render_cloud_wind"), 0.0f);
    r.cloudDirection = f(*output, "render_cloud_direction");
    r.view = static_cast<render::Settings::View>(std::clamp(whole(*output, "render_view"), 0, 2));
    r.detail = std::clamp(f(*output, "render_detail"), 0.0f, 1.0f);
    r.textures = f(*output, "render_textures") != 0.0f;
    r.textureFolder = text(output->id, "render_texture_folder");
    if (!r.textureFolder.empty() && !folder.empty() && std::filesystem::path(r.textureFolder).is_relative()) {
        r.textureFolder = (std::filesystem::path(folder) / r.textureFolder).lexically_normal().string();
    }
    // The camera of the shot.
    if (const Node* cam = upstream(*output, "camera")) {
        Camera& m = c.camera;
        if (cam->type == "usd_camera") {
            // A matchmove's camera, at this frame, from its file.
            std::string file = text(cam->id, "file");
            if (!file.empty() && !folder.empty() && std::filesystem::path(file).is_relative()) {
                file = (std::filesystem::path(folder) / file).lexically_normal().string();
            }
            std::string error;
            std::vector<std::string> warnings;
            bool varies = false;
            double timeCode = 0.0;
            if (file.empty()) {
                problem(L::Error, cam->id, "no file: which USD file has the camera?");
            } else if (cameraFromUsd(file, text(cam->id, "prim"), frame, 1.0f / c.world.timeStep, f(*cam, "offset"),
                                     whole(*cam, "width"), whole(*cam, "height"), f(*cam, "metres") != 0.0f, m, error,
                                     &warnings, &varies, &timeCode)) {
                for (const std::string& w : warnings) problem(L::Warning, cam->id, w);
                c.fileAnimation = c.fileAnimation || varies;
                c.hasCamera = true;
            } else {
                problem(L::Error, cam->id, error);
            }
            m.node = cam->id;
            // The plate's frames follow the shot's time codes: 1001 at the frame that reads 1001.
            m.plateFrame = whole(*cam, "plate_frame");
            if (m.plateFrame == 0) m.plateFrame = static_cast<int>(std::lround(timeCode)) - (static_cast<int>(std::lround(frame)) - 1);
        } else {
            m.position = v3(*cam, "center");
            m.rotation = v3(*cam, "rotation");
            m.focal = f(*cam, "focal");
            m.width = whole(*cam, "width");
            m.height = whole(*cam, "height");
            m.node = cam->id;
            m.plateFrame = whole(*cam, "plate_frame");
            m = m.sanitized();
            c.hasCamera = true;
        }
        // The footage behind it.
        std::string plate = text(cam->id, "plate");
        if (!plate.empty() && !folder.empty() && std::filesystem::path(plate).is_relative()) {
            plate = (std::filesystem::path(folder) / plate).lexically_normal().string();
        }
        m.plate = plate;
        if (c.hasCamera) {
            c.render.width = m.width;
            c.render.height = m.height;
        }
        if (!plate.empty() && frame <= 1.0f) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(m.plateFile(1), ec)) {
                problem(L::Warning, cam->id, "no plate at frame 1: " + m.plateFile(1) + " is not there");
            }
        }
        c.active.push_back(cam->id);
    }

    // What feeds a solver's input, in the order it was linked. Bypassed nodes
    // stay out; so do nodes of a type this program does not know (reported
    // above).
    auto feeding = [&](const Node* solver, const char* input) {
        std::vector<const Node*> out;
        for (const Link& l : linksInto(solver->id, input)) {
            const Node* n = node(l.from);
            if (n && !n->bypass && findNodeType(n->type)) out.push_back(n);
        }
        return out;
    };
    // The forces linked into a solver, in their order.
    auto forcesOf = [&](const Node* solver) {
        std::vector<Force> out;
        for (const Node* n : feeding(solver, "forces")) {
            Force force;
            force.node = n->id;
            const std::string& t = n->type;
            if (const ParamDef* d = findNodeType(t)->param("mask")) force.mask = static_cast<Mask>(whole(*n, d->name));
            if (t == "turbulence") {
                force.kind = ForceKind::Turbulence;
                force.strength = f(*n, "strength");
                force.scale = f(*n, "scale");
                force.speed = f(*n, "speed");
                force.seed = static_cast<uint32_t>(whole(*n, "seed"));
            } else if (t == "wind") {
                force.kind = ForceKind::Wind;
                force.direction = v3(*n, "direction");
                force.speed = f(*n, "speed");
                force.strength = f(*n, "strength");
                force.gusts = f(*n, "gusts");
                force.seed = static_cast<uint32_t>(whole(*n, "seed"));
            } else if (t == "vortex") {
                force.kind = ForceKind::Vortex;
                force.center = v3(*n, "center");
                force.direction = v3(*n, "axis");
                force.radius = f(*n, "radius");
                force.height = f(*n, "height");
                force.speed = f(*n, "speed");
                force.lift = f(*n, "lift");
                force.suction = f(*n, "suction");
                force.strength = f(*n, "strength");
            } else if (t == "attractor") {
                force.kind = ForceKind::Attractor;
                force.center = v3(*n, "center");
                force.radius = f(*n, "radius");
                force.strength = f(*n, "strength");
            } else if (t == "drag") {
                force.kind = ForceKind::Drag;
                force.strength = f(*n, "strength");
            } else {
                continue;
            }
            out.push_back(force);
            c.active.push_back(n->id);
        }
        return out;
    };
    // Does a domain overlap the box from a to b?
    auto overlapsDomain = [](const Domain& domain, const Vec3& a, const Vec3& b) {
        const Vec3 lo = domain.origin(), hi = domain.origin() + domain.size();
        return a.x < hi.x && b.x > lo.x && a.y < hi.y && b.y > lo.y && a.z < hi.z && b.z > lo.z;
    };

    // The pieces of an RBD Solver: its settings, the geometry linked into
    // Pieces (at frame 1), and the objects they knock into. One solver is
    // simulated; its look, its collider and its dust may each bring it in.
    std::map<int, std::shared_ptr<const Geometry>>& piecesMemo = memo.pieces;
    auto compileRigid = [&](const Node* solver) -> RigidScene* {
        RigidScene& r = c.world.rigid;
        if (c.rigid == solver->id) return &r;
        if (c.rigid) {
            problem(L::Warning, solver->id, "Another RBD Solver: only " + node(c.rigid)->name + " is simulated.");
            return nullptr;
        }
        c.rigid = solver->id;
        c.active.push_back(solver->id);
        c.world.hasRigid = true;
        RigidSettings& s = r.solver;
        s.density = f(*solver, "density");
        s.friction = f(*solver, "friction");
        s.bounce = f(*solver, "bounce");
        s.gravity = Vec3(0.0f, -f(*solver, "gravity"), 0.0f);
        s.floor = f(*solver, "floor") != 0.0f;
        s.glue = f(*solver, "glue") * 1000.0f;  // kPa
        s.spread = f(*solver, "spread");
        s.rings = whole(*solver, "rings");
        s.rebarStrength = f(*solver, "rebar_strength") * 1e6f;  // MPa
        s.bond = f(*solver, "bond") * 1e6f;
        s.stretch = f(*solver, "stretch");
        s.substeps = whole(*solver, "substeps");
        s.rest = f(*solver, "rest") != 0.0f;
        s.dust = f(*solver, "dust");
        s.dustSize = f(*solver, "dust_size");
        s.impactDust = f(*solver, "impact_dust");
        s.debris = f(*solver, "debris");
        s.trail = f(*solver, "trail");
        s.air = f(*solver, "air");
        s.guideStrength = f(*solver, "guide_strength");
        s.guideUntil = f(*solver, "guide_until");
        s.guideReach = f(*solver, "guide_reach");
        s.guideLetGo = f(*solver, "guide_let_go") != 0.0f;
        s.buoyancy = f(*solver, "buoyancy");
        s.waterDrag = f(*solver, "water_drag");
        s.airDrag = f(*solver, "air_drag");
        s.timeStep = c.world.timeStep;
        r.attribute = text(solver->id, "attribute");
        r.node = solver->id;
        if (!rigidAvailable()) {
            problem(L::Error, solver->id, "This build has no rigid bodies: it was built without Jolt (PG_WITH_JOLT=OFF).");
        }
        // The pieces: cooked once a compile.
        const std::vector<Link> in = linksInto(solver->id, "pieces");
        if (in.empty()) {
            problem(L::Warning, solver->id, "No pieces: link geometry into Pieces -- a Voronoi Fracture's.");
        } else if (const auto it = piecesMemo.find(solver->id); it != piecesMemo.end()) {
            r.pieces = it->second;
        } else {
            startCooker();
            const GeometryPtr geo = cooker->cook(in.front().from, 1, firstStep);
            const std::string error = cooker->error(in.front().from);
            if (!error.empty()) problem(L::Warning, in.front().from, error);
            if (fromSimulation(in.front().from)) {
                problem(L::Warning, solver->id, "Its pieces come from a simulation, which has not run when the "
                                                "pieces are taken: nothing to simulate.");
            } else if (!geo || geo->primitiveCount() == 0) {
                problem(L::Warning, solver->id, "The geometry linked into Pieces has no faces: nothing to simulate.");
            } else {
                r.pieces = geo;
                int count = 0;
                pieceOfPrimitives(*geo, r.attribute, count);
                if (count > 3000) {
                    problem(L::Warning, solver->id, std::to_string(count) + " pieces: it will be slow. Fewer, "
                                                    "bigger pieces go a long way.");
                }
            }
            piecesMemo[solver->id] = r.pieces;
        }
        // The bars: cooked once a compile, as the pieces are.
        const std::vector<Link> bars = linksInto(solver->id, "rebar");
        if (!bars.empty()) {
            if (const auto it = memo.rebar.find(solver->id); it != memo.rebar.end()) {
                r.rebar = it->second;
            } else {
                startCooker();
                const GeometryPtr geo = cooker->cook(bars.front().from, 1, firstStep);
                const std::string error = cooker->error(bars.front().from);
                if (!error.empty()) problem(L::Warning, bars.front().from, error);
                if (fromSimulation(bars.front().from)) {
                    problem(L::Warning, solver->id, "Its bars come from a simulation, which has not run when the "
                                                    "bars are taken: no bars.");
                } else if (!geo || geo->primitiveCount() == 0) {
                    problem(L::Warning, solver->id, "The geometry linked into Rebar has no lines: no bars.");
                } else {
                    r.rebar = geo;
                }
                memo.rebar[solver->id] = r.rebar;
            }
        }
        // The network of glue: cooked once a compile, as the pieces are.
        const std::vector<Link> network = linksInto(solver->id, "constraints");
        if (!network.empty()) {
            if (const auto it = memo.constraints.find(solver->id); it != memo.constraints.end()) {
                r.constraints = it->second;
            } else {
                startCooker();
                const GeometryPtr geo = cooker->cook(network.front().from, 1, firstStep);
                const std::string error = cooker->error(network.front().from);
                if (!error.empty()) problem(L::Warning, network.front().from, error);
                if (fromSimulation(network.front().from)) {
                    problem(L::Warning, solver->id, "Its constraints come from a simulation, which has not run when "
                                                    "they are taken: the glue is where the pieces touch.");
                } else if (!geo) {
                    // Not cooked -- its error said why: the glue is where the pieces touch.
                } else if (geo->primitiveCount() == 0) {
                    // No lines: no joints -- the pieces hold nothing, as with Glue 0.
                    r.constraints = geo;
                    problem(L::Warning, solver->id, "The network linked into Constraints has no lines: the pieces "
                                                    "are not glued.");
                } else {
                    r.constraints = geo;
                    // Its points name pieces: those that are not in Pieces join nothing.
                    const size_t lost = strayJoints(*geo, r.pieces.get(), r.attribute);
                    if (lost > 0) {
                        problem(L::Warning, solver->id,
                                std::to_string(lost) + " of the lines of Constraints join pieces that are not in "
                                "Pieces: they are left out.");
                    }
                }
                memo.constraints[solver->id] = r.constraints;
            }
        }
        // The guide: the pieces moved as they are to go -- cooked at this
        // frame; where it moves with time, the network is taken frame by frame.
        const std::vector<Link> guides = linksInto(solver->id, "guide");
        if (!guides.empty()) {
            startCooker();
            const int from = guides.front().from;
            const GeometryPtr geo = cooker->cook(from, static_cast<int>(std::lround(frame)), firstStep);
            const std::string error = cooker->error(from);
            if (!error.empty()) problem(L::Warning, from, error);
            if (fromSimulation(from)) {
                problem(L::Warning, solver->id, "Its guide comes from a simulation, which has not run when the guide "
                                                "is taken: nothing leads the pieces.");
            } else if (geo && r.pieces && geo->pointCount() != r.pieces->pointCount()) {
                problem(L::Warning, solver->id,
                        "The guide has " + std::to_string(geo->pointCount()) + " points, the pieces " +
                            std::to_string(r.pieces->pointCount()) +
                            ": it must be the pieces moved -- as many points, in the same order. Nothing leads them.");
            } else if (geo && r.pieces) {
                // Kept as where it has each piece -- a few bytes a piece a
                // frame, not the guide's points.
                auto rest = memo.guideRest.find(solver->id);
                if (rest == memo.guideRest.end()) {
                    rest = memo.guideRest.emplace(solver->id, rigidGuideRest(*r.pieces, r.attribute)).first;
                }
                r.guide = rigidGuide(*r.pieces, rest->second, *geo);
                const pg::Node* core = cooker->coreNode(from);
                if (core && cooker->engine().isTimeDependent(*core)) c.guideMoves = true;
            }
        }
        for (const Node* n : feeding(solver, "colliders")) {
            if (n->type == "rbd_solver") {
                problem(L::Warning, n->id, "The pieces of one RBD Solver do not knock into another's.");
                continue;
            }
            r.colliders.push_back(colliderOf(*n));
            c.active.push_back(n->id);
        }
        return &r;
    };

    // The cloth of a Cloth Solver: its settings, its geometry -- as it rests
    // (frame 1) and at this frame, where its pins go -- and what it falls on.
    auto compileCloth = [&](const Node* solver) -> ClothScene* {
        ClothScene& cs = c.world.cloth;
        if (c.cloth == solver->id) return &cs;
        if (c.cloth) {
            problem(L::Warning, solver->id, "Another Cloth Solver: only " + node(c.cloth)->name + " is simulated.");
            return nullptr;
        }
        c.cloth = solver->id;
        c.active.push_back(solver->id);
        c.world.hasCloth = true;
        ClothSettings& s = cs.solver;
        s.density = f(*solver, "density");
        s.stretch = f(*solver, "stretch");
        s.shear = f(*solver, "shear");
        s.bend = f(*solver, "bend");
        s.pressure = f(*solver, "pressure");
        s.tear = f(*solver, "tear");
        s.thickness = f(*solver, "thickness");
        s.friction = f(*solver, "friction");
        s.selfCollision = f(*solver, "self_collision") != 0.0f;
        s.floor = f(*solver, "floor") != 0.0f;
        s.airDrag = f(*solver, "air_drag");
        s.damping = f(*solver, "damping");
        s.gravity = Vec3(0.0f, -f(*solver, "gravity"), 0.0f);
        s.substeps = whole(*solver, "substeps");
        s.timeStep = c.world.timeStep;
        cs.node = solver->id;
        const std::vector<Link> in = linksInto(solver->id, "geometry");
        if (in.empty()) {
            problem(L::Warning, solver->id, "No cloth: link geometry into Geometry -- a grid, a line, a sphere.");
        } else {
            startCooker();
            const int from = in.front().from;
            if (fromSimulation(from)) {
                problem(L::Warning, solver->id, "Its geometry comes from a simulation, which has not run when the cloth "
                                                "is taken: nothing to simulate.");
            } else {
                auto rest = memo.cloth.find(solver->id);
                if (rest == memo.cloth.end()) {
                    const GeometryPtr geo = cooker->cook(from, 1, firstStep);
                    const std::string error = cooker->error(from);
                    if (!error.empty()) problem(L::Warning, from, error);
                    if (!geo || (geo->primitiveCount() == 0)) {
                        problem(L::Warning, solver->id, "The geometry linked into Geometry has no polygons and no "
                                                        "lines: nothing to simulate.");
                    }
                    rest = memo.cloth.emplace(solver->id, geo && geo->primitiveCount() > 0 ? geo : nullptr).first;
                }
                cs.geometry = rest->second;
                // Where the pins are to go: the geometry at this frame, when
                // it moves with time.
                const pg::Node* core = cooker->coreNode(from);
                if (cs.geometry && core && cooker->engine().isTimeDependent(*core)) {
                    c.clothMoves = true;
                    const GeometryPtr now = cooker->cook(from, static_cast<int>(std::lround(frame)), firstStep);
                    if (now && now->pointCount() == cs.geometry->pointCount()) cs.target = now;
                }
            }
        }
        cs.forces = forcesOf(solver);
        for (const Node* n : feeding(solver, "colliders")) {
            if (n->type == "rbd_solver") {
                if (RigidScene* r = compileRigid(n)) r->intoCloth = true;
                continue;
            }
            cs.colliders.push_back(colliderOf(*n));
            c.active.push_back(n->id);
        }
        return &cs;
    };

    // The gas of a Pyro Solver: its settings, and what feeds it.
    auto compileGas = [&](const Node* solver) {
        c.world.hasGas = true;
        SolverSettings& s = c.world.gas.solver;
        s.size = v3(*solver, "size");
        s.resolution = whole(*solver, "resolution");
        s.closedFloor = f(*solver, "closed_floor") != 0.0f;
        s.sparse = f(*solver, "sparse") != 0.0f;
        s.cutoff = f(*solver, "cutoff");
        s.timeStep = c.world.timeStep;
        s.substeps = whole(*solver, "substeps");
        s.pressureCycles = whole(*solver, "pressure_cycles");
        s.seed = static_cast<uint32_t>(whole(*solver, "seed"));
        s.buoyancy = f(*solver, "buoyancy");
        s.weight = f(*solver, "weight");
        s.vorticity = f(*solver, "vorticity");
        s.burnRate = f(*solver, "burn_rate");
        s.heatRelease = f(*solver, "heat_release");
        s.sootRelease = f(*solver, "soot_release");
        s.expansion = f(*solver, "expansion");
        s.flameLife = f(*solver, "flame_life");
        s.cooling = f(*solver, "cooling");
        s.smokeDecay = f(*solver, "smoke_decay");

        bool dust = false;
        for (const Node* n : feeding(solver, "sources")) {
            if (n->type == "rbd_solver") {
                // The dust of its broken glue, puffing in as it breaks.
                if (RigidScene* r = compileRigid(n)) r->dustIntoGas = dust = true;
                continue;
            }
            Emitter e;
            const Placement p = placementOf(*n);
            e.shape = p.shape;
            e.center = p.center;
            e.rotation = p.rotation;
            e.size = p.size;
            e.mesh = p.mesh;
            e.fuel = f(*n, "fuel");
            e.smoke = f(*n, "smoke");
            e.heat = f(*n, "heat");
            e.velocity = v3(*n, "velocity");
            e.expansion = f(*n, "expansion");
            e.flicker = f(*n, "flicker");
            e.flickerSize = f(*n, "flicker_size");
            e.seed = static_cast<uint32_t>(whole(*n, "seed"));
            e.start = f(*n, "start");
            e.end = f(*n, "end");
            e.motion = static_cast<Motion>(whole(*n, "motion"));
            e.motionSize = f(*n, "motion_size");
            e.motionPeriod = f(*n, "motion_period");
            e.node = n->id;
            c.world.gas.emitters.push_back(e);
            c.active.push_back(n->id);
        }
        c.world.gas.forces = forcesOf(solver);
        for (const Node* n : feeding(solver, "colliders")) {
            if (n->type == "rbd_solver") {
                if (RigidScene* r = compileRigid(n)) r->intoGas = true;
                continue;
            }
            c.world.gas.colliders.push_back(colliderOf(*n));
            c.active.push_back(n->id);
        }

        // Things that run but will not do what was meant.
        const Domain domain = c.world.gas.sanitized().solver.domain();
        auto overlaps = [&](const Vec3& a, const Vec3& b) { return overlapsDomain(domain, a, b); };
        if (c.world.gas.emitters.empty() && !dust) {
            problem(L::Warning, solver->id, "No sources: nothing will appear. Link a source into Sources.");
        }
        for (const Emitter& e : c.world.gas.emitters) {
            Vec3 a, b;
            e.shapeAt(0.0f).bounds(a, b);
            a = a - e.shapeAt(0.0f).center();
            b = b - e.shapeAt(0.0f).center();
            Vec3 reach;
            if (e.motion == Motion::Circle) reach = Vec3(e.motionSize, 0.0f, e.motionSize);
            if (e.motion == Motion::Sway) reach = Vec3(e.motionSize, 0.0f, 0.0f);
            if (!overlaps(e.center + a - reach, e.center + b + reach)) {
                problem(L::Warning, e.node, "Outside the solver's domain: none of its gas gets in.");
            }
            if (e.fuel <= 0.0f && e.smoke <= 0.0f && e.heat <= 0.0f) {
                problem(L::Warning, e.node, "Adds nothing: give it fuel, smoke or heat.");
            }
            for (const Collider& col : c.world.gas.colliders) {
                if (col.contains(e.center)) {
                    problem(L::Warning, e.node, "Inside a collider: no gas comes out of a solid.");
                    break;
                }
            }
        }
        for (const Collider& col : c.world.gas.colliders) {
            Vec3 a, b;
            col.instance().bounds(a, b);
            if (!overlaps(a, b)) problem(L::Warning, col.node, "Outside the solver's domain: nothing to collide with.");
        }
    };

    // The water of a Liquid Solver: its settings, and what feeds it.
    auto compileWater = [&](const Node* solver) {
        c.world.hasWater = true;
        LiquidSettings& s = c.world.water.solver;
        s.size = v3(*solver, "size");
        s.resolution = whole(*solver, "resolution");
        s.closedSides = f(*solver, "closed_sides") != 0.0f;
        s.sparse = f(*solver, "sparse") != 0.0f;
        s.timeStep = c.world.timeStep;
        s.substeps = whole(*solver, "substeps");
        s.flip = f(*solver, "flip");
        s.gravity = f(*solver, "gravity");
        s.seed = static_cast<uint32_t>(whole(*solver, "seed"));
        for (const Node* n : feeding(solver, "sources")) {
            WaterSource w;
            const Placement p = placementOf(*n);
            w.shape = p.shape;
            w.center = p.center;
            w.rotation = p.rotation;
            w.size = p.size;
            w.mesh = p.mesh;
            w.mode = static_cast<WaterMode>(whole(*n, "mode"));
            w.velocity = v3(*n, "velocity");
            w.seed = static_cast<uint32_t>(whole(*n, "seed"));
            w.start = f(*n, "start");
            w.end = f(*n, "end");
            w.node = n->id;
            c.world.water.sources.push_back(w);
            c.active.push_back(n->id);
        }
        c.world.water.forces = forcesOf(solver);
        for (const Node* n : feeding(solver, "colliders")) {
            if (n->type == "rbd_solver") {
                if (RigidScene* r = compileRigid(n)) r->intoWater = true;
                continue;
            }
            c.world.water.colliders.push_back(colliderOf(*n));
            c.active.push_back(n->id);
        }

        // Things that run but will not do what was meant.
        const Domain domain = c.world.water.sanitized().solver.domain();
        if (c.world.water.sources.empty()) {
            problem(L::Warning, solver->id, "No water: link a Water Source into Sources.");
        }
        for (const WaterSource& w : c.world.water.sources) {
            Vec3 a, b;
            w.instance().bounds(a, b);
            if (!overlapsDomain(domain, a, b)) {
                problem(L::Warning, w.node, "Outside the solver's domain: none of its water gets in.");
            }
            for (const Collider& col : c.world.water.colliders) {
                if (col.contains(w.center)) {
                    problem(L::Warning, w.node, "Inside a collider: the water there is left out.");
                    break;
                }
            }
        }
        for (const Collider& col : c.world.water.colliders) {
            Vec3 a, b;
            col.instance().bounds(a, b);
            if (!overlapsDomain(domain, a, b)) {
                problem(L::Warning, col.node, "Outside the liquid's domain: nothing to collide with.");
            }
        }
    };

    // Each look linked into the Output is a layer of the picture -- and of
    // what is simulated.
    const std::vector<Link> layers = linksInto(output->id, "look");
    if (layers.empty()) {
        if (!c.model) {
            problem(L::Error, output->id,
                    "Nothing to show: link a Volume Look, a Water Look, a Rain or an RBD Solver into Looks.");
        }
        return done();
    }
    for (const Link& layer : layers) {
        const Node* look = node(layer.from);
        if (!look || !findNodeType(look->type)) continue;
        if (look->type == "volume_look") {
            if (c.lookNode) {
                problem(L::Warning, look->id, "Another Volume Look: only " + node(c.lookNode)->name + " is drawn.");
                continue;
            }
            c.lookNode = look->id;
            c.active.push_back(look->id);
            k.smokeColor = v3(*look, "smoke_color");
            k.smokeDensity = f(*look, "smoke_density");
            k.occlusion = f(*look, "occlusion");
            k.flameIntensity = f(*look, "flame_intensity");
            k.flameStart = f(*look, "flame_start");
            k.flameRange = f(*look, "flame_range");
            k.fireLight = f(*look, "fire_light");
            const Node* solver = upstream(*look, "gas");
            if (!solver) {
                problem(L::Error, look->id, "No gas to draw: link a Pyro Solver into Gas.");
                continue;
            }
            c.solver = solver->id;
            c.active.push_back(solver->id);
            compileGas(solver);
        } else if (look->type == "rain") {
            if (c.rain) {
                problem(L::Warning, look->id, "Another Rain: only " + node(c.rain)->name + " falls.");
                continue;
            }
            c.rain = look->id;
            c.active.push_back(look->id);
            c.world.hasRain = true;
            RainSettings& r = c.world.rain.rain;
            r.center = v3(*look, "center");
            r.size = v3(*look, "size");
            r.rate = f(*look, "rate");
            r.speed = f(*look, "speed");
            r.splash = f(*look, "splash");
            r.ripples = f(*look, "ripples");
            r.seed = static_cast<uint32_t>(whole(*look, "seed"));
            r.start = f(*look, "start");
            r.end = f(*look, "end");
            r.timeStep = c.world.timeStep;
            c.world.rain.forces = forcesOf(look);
            for (const Node* n : feeding(look, "colliders")) {
                if (n->type == "rbd_solver") {
                    if (RigidScene* r = compileRigid(n)) r->intoRain = true;
                    continue;
                }
                c.world.rain.colliders.push_back(colliderOf(*n));
                c.active.push_back(n->id);
            }
            k.rainColor = v3(*look, "color");
            k.rainOpacity = f(*look, "opacity");
            k.rainStreak = f(*look, "streak");
            k.wetness = f(*look, "wet");
            if (r.center.y - 0.5f * r.size.y <= 0.0f) {
                problem(L::Warning, look->id, "The cloud is at or below the floor: raise it, or no rain falls.");
            }
            if (r.rate <= 0.0f) problem(L::Warning, look->id, "Rate 0: no drop falls.");
        } else if (look->type == "cloth_solver") {
            if (!compileCloth(look)) continue;
            k.cloth = true;
            k.clothColor = v3(*look, "color");
        } else if (look->type == "rbd_solver") {
            if (!compileRigid(look)) continue;
            k.pieces = true;
            k.piecesColor = v3(*look, "color");
            k.piecesInside = v3(*look, "inside_color");
            k.rebarColor = v3(*look, "rebar_color");
            k.insideGroup = text(look->id, "inside_group");
        } else if (look->type == "water_look") {
            if (c.waterLook) {
                problem(L::Warning, look->id, "Another Water Look: only " + node(c.waterLook)->name + " is drawn.");
                continue;
            }
            c.waterLook = look->id;
            c.active.push_back(look->id);
            k.waterColor = v3(*look, "color");
            k.waterClarity = f(*look, "clarity");
            k.foam = f(*look, "foam");
            k.waterSurface = f(*look, "surface") != 0.0f;
            const Node* solver = upstream(*look, "liquid");
            if (!solver) {
                problem(L::Error, look->id, "No water to draw: link a Liquid Solver into Liquid.");
                continue;
            }
            c.liquidSolver = solver->id;
            c.active.push_back(solver->id);
            compileWater(solver);
        }
    }
    // The nodes that bring a simulation back as geometry: from what is simulated.
    struct Back {
        const char* type;
        const char* input;
        int simulated;
        const char* solver;
    };
    const Back backs[] = {{"liquid_points", "liquid", c.liquidSolver, "Liquid Solver"},
                          {"liquid_surface", "liquid", c.liquidSolver, "Liquid Solver"},
                          {"rain_points", "rain", c.rain, "Rain"},
                          {"gas_volume", "gas", c.solver, "Pyro Solver"},
                          {"rbd_pieces", "rigid", c.rigid, "RBD Solver"},
                          {"cloth_geometry", "cloth", c.cloth, "Cloth Solver"}};
    for (const Node& n : nodes_) {
        if (n.bypass) continue;
        const Back* back = nullptr;
        for (const Back& b : backs) {
            if (n.type == b.type) back = &b;
        }
        if (!back) continue;
        const Node* from = upstream(n, back->input);
        if (!from) {
            problem(L::Warning, n.id, std::string("Nothing comes in: link a ") + back->solver + " into it.");
        } else if (from->id != back->simulated) {
            problem(L::Warning, n.id, from->name + " is not simulated -- it does not reach the Output -- so this is empty.");
        } else if (n.type == "liquid_points") {
            c.world.keepParticles = true;
        }
    }
    c.ok = c.world.any();
    return done();
}

// --- examples ------------------------------------------------------------------------

const std::vector<std::string>& Network::exampleNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const EmbeddedExample& e : embeddedExamples()) n.emplace_back(e.name);
        return n;
    }();
    return names;
}

const char* Network::exampleText(std::string_view name) {
    for (const EmbeddedExample& e : embeddedExamples()) {
        if (name == e.name) return e.text;
    }
    return nullptr;
}

bool Network::example(std::string_view name, Network& out) {
    const char* text = exampleText(name);
    std::string error;
    return text && load(text, out, error);
}

}  // namespace pg::sim
