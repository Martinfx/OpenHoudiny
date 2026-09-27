#include "pg/sim/Network.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <locale>
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

ParamDef shape(const char* help) {
    return {"shape",
            "Shape",
            "Shape",
            K::Choice,
            {0.0f, 0.0f, 0.0f},
            0.0f,
            4.0f,
            0.0f,
            4.0f,
            "",
            help,
            {"sphere", "box", "cylinder", "cone", "torus"},
            {"Sphere", "Box", "Cylinder", "Cone", "Torus"}};
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

std::vector<ParamDef> objectParams() {
    return {shape("Its shape: a ball, a box, a column, a cone, a ring."),
            position(Vec3(0.0f, 0.15f, 0.0f), "Where it is: its middle. y is its height above the floor."),
            rotation(),
            size(Vec3(0.3f, 0.3f, 0.3f),
                 "Width, height and depth, along its own axes: a ball's diameter, a box's edges, a ring's width "
                 "and thickness."),
            {"color", "Color", "Look", K::Color, {0.45f, 0.45f, 0.46f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
             "Its colour. Painting it simulates nothing again."}};
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

std::vector<NodeType> buildTypes() {
    std::vector<NodeType> t;

    // --- objects ----------------------------------------------------------------------
    t.push_back({"object", "Object", "Objects",
                 "A solid in the scene: a ball, a box, a column, a cone, a ring. It is drawn and casts shadows; "
                 "linked into a solver's Colliders, what the solver simulates goes round it. Move, turn and size "
                 "it in the viewport: W, E, R.",
                 {},
                 {{"collider", "Collider", PinType::Collider}},
                 objectParams(),
                 1});
    t.back().handles = {"center", "rotation", nullptr, "size", nullptr, nullptr};

    // --- sources --------------------------------------------------------------------
    t.push_back({"pyro_source", "Pyro Source", "Sources",
                 "A shape that gives off fuel, smoke and heat, and pushes the gas its way. Fuel makes fire; "
                 "smoke and heat without fuel make a column of smoke. A ball for a campfire, a box for a burning "
                 "log or a vent, a ring for a gas burner.",
                 {},
                 {{"source", "Source", PinType::Source}},
                 pyroSourceParams(),
                 2});
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
         {{"size", "Size", "Domain", K::Vector, {1.0f, 1.5f, 1.0f}, 0.1f, 5.0f, 0.1f, 20.0f, "m",
           "Width, height and depth of the box the gas lives in. It stands on the floor, centred."},
          {"resolution", "Resolution", "Domain", K::Int, {96.0f, 0.0f, 0.0f}, 16.0f, 256.0f, 16.0f, 256.0f, "",
           "Cells along the longest side. Twice as many: finer detail, and eight times the work."},
          {"closed_floor", "Closed Floor", "Domain", K::Toggle, {1.0f, 0.0f, 0.0f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
           "A floor the gas cannot pass. Off, the bottom is open like the sides and the top."},
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
           "How fast smoke thins out."}}});

    // --- render ---------------------------------------------------------------------
    t.push_back(
        {"volume_look", "Volume Look", "Render",
         "How the gas is drawn: smoke that stops and scatters light, fire that glows, the sun and the sky. "
         "Changing it draws the frames again -- nothing is simulated again.",
         {{"gas", "Gas", PinType::Gas}},
         {{"look", "Look", PinType::Look}},
         {{"smoke_color", "Color", "Smoke", K::Color, {0.75f, 0.75f, 0.77f}, 0.0f, 1.0f, 0.0f, 1.0f, "",
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
           "Draw the floor, with the shadow of the smoke and the glow of the fire on it."}}});
    t.push_back({"output", "Output", "Render",
                 "Where the network ends: what the viewport shows and `pgshader sim` renders.",
                 {{"look", "Look", PinType::Look}},
                 {},
                 {{"frames", "Frames", "Output", K::Int, {150.0f, 0.0f, 0.0f}, 1.0f, 1000.0f, 1.0f, 100000.0f, "",
                   "How many frames to simulate: the length of the timeline."}}});

    for (NodeType& type : t) {
        const std::string c = type.category;
        type.bypassable = c == "Objects" || c == "Sources" || c == "Forces";
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
    void (*upgrade)(Node& node);
};

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
void upgradeSource(Node& node) {
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
void upgradeCollider(Node& node) {
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

const std::vector<Legacy>& legacyTypes() {
    static const std::vector<Legacy> types = [] {
        std::vector<Legacy> l;
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

const std::vector<NodeType>& nodeTypes() {
    static const std::vector<NodeType> types = buildTypes();
    return types;
}

const NodeType* findNodeType(std::string_view name) {
    for (const NodeType& t : nodeTypes()) {
        if (name == t.name) return &t;
    }
    return nullptr;
}

const std::vector<const char*>& nodeCategories() {
    static const std::vector<const char*> c = {"Objects", "Sources", "Forces", "Simulation", "Render"};
    return c;
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
        case K::Float:
        case K::Int: break;
    }
    return formatNumber(v[0]);
}

bool parseParam(const ParamDef& def, std::string_view text, ParamValue& out, std::string& error) {
    const std::vector<std::string_view> w = splitWords(text);
    ParamValue v = def.value;
    switch (def.kind) {
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
    ++revision_;
    return nodes_.back().id;
}

bool Network::remove(int id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    nodes_.erase(nodes_.begin() + i);
    std::erase_if(links_, [&](const Link& l) { return l.from == id || l.to == id; });
    ++revision_;
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
    n->name = name;
    ++revision_;
    return true;
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
    return true;
}

bool Network::connect(int from, std::string_view output, int to, std::string_view input, std::string* error) {
    if (!canConnect(from, output, to, input, error)) return false;
    const PinDef* in = findNodeType(node(to)->type)->input(input);
    if (!in->many) std::erase_if(links_, [&](const Link& l) { return l.to == to && l.input == input; });
    links_.push_back({from, std::string(output), to, std::string(input)});
    ++revision_;
    return true;
}

bool Network::disconnect(const Link& link) {
    const auto it = std::find(links_.begin(), links_.end(), link);
    if (it == links_.end()) return false;
    links_.erase(it);
    ++revision_;
    return true;
}

std::vector<Link> Network::linksInto(int to, std::string_view input) const {
    std::vector<Link> out;
    for (const Link& l : links_) {
        if (l.to == to && l.input == input) out.push_back(l);
    }
    return out;
}

ParamValue Network::param(int id, std::string_view name) const {
    const Node* n = node(id);
    if (!n) return {};
    const auto it = n->params.find(std::string(name));
    if (it != n->params.end()) return it->second;
    const NodeType* t = findNodeType(n->type);
    const ParamDef* d = t ? t->param(name) : nullptr;
    return d ? d->value : ParamValue{};
}

bool Network::setParam(int id, std::string_view name, const ParamValue& value) {
    Node* n = node(id);
    const NodeType* t = n ? findNodeType(n->type) : nullptr;
    const ParamDef* d = t ? t->param(name) : nullptr;
    if (!d) return false;
    const ParamValue v = keep(*d, value);
    const std::string key(name);
    const auto it = n->params.find(key);
    const ParamValue before = it != n->params.end() ? it->second : d->value;
    // Stored only when it differs from the default: the file lists the
    // changes, and a default that improves reaches the networks that kept it.
    if (v == d->value) n->params.erase(key);
    else n->params[key] = v;
    if (v != before) ++revision_;
    return true;
}

bool Network::setParam(int id, std::string_view name, std::string_view text, std::string* error) {
    const Node* n = node(id);
    const NodeType* t = n ? findNodeType(n->type) : nullptr;
    const ParamDef* d = t ? t->param(name) : nullptr;
    if (!d) {
        if (error) {
            *error = !n ? "no such node" : !t ? "unknown node type " + n->type : n->name + " has no parameter " + std::string(name);
        }
        return false;
    }
    ParamValue v;
    std::string why;
    if (!parseParam(*d, text, v, why)) {
        if (error) *error = why;
        return false;
    }
    return setParam(id, name, v);
}

bool Network::resetParam(int id, std::string_view name) {
    Node* n = node(id);
    if (!n) return false;
    if (n->params.erase(std::string(name)) > 0) ++revision_;
    return true;
}

bool Network::isDefault(int id, std::string_view name) const {
    const Node* n = node(id);
    return !n || n->params.find(std::string(name)) == n->params.end();
}

bool Network::setBypass(int id, bool on) {
    Node* n = node(id);
    if (!n) return false;
    if (n->bypass != on) {
        n->bypass = on;
        ++revision_;
    }
    return true;
}

// --- files ---------------------------------------------------------------------------

std::string Network::save() const {
    std::string out = "pgsim " + std::to_string(kFormatVersion) + "\n";
    for (const Node& n : nodes_) {
        out += "node " + std::to_string(n.id) + ' ' + n.type + ' ' + std::to_string(n.version) + ' ' + n.name + ' ' +
               formatNumber(n.x) + ' ' + formatNumber(n.y) + '\n';
        const NodeType* t = findNodeType(n.type);
        if (t) {
            // In the order of the type's table: the order the editor shows.
            for (const ParamDef& d : t->params) {
                const auto it = n.params.find(d.name);
                if (it != n.params.end()) out += std::string("  param ") + d.name + ' ' + formatParam(d, it->second) + '\n';
            }
        } else {
            // A type this program does not know: its values as they came.
            for (const auto& [name, v] : n.params) {
                out += "  param " + name + ' ' + formatNumber(v[0]) + ' ' + formatNumber(v[1]) + ' ' +
                       formatNumber(v[2]) + '\n';
            }
        }
        if (n.bypass) out += "  bypass\n";
    }
    for (const Link& l : links_) {
        out += "link " + std::to_string(l.from) + '.' + l.output + " -> " + std::to_string(l.to) + '.' + l.input + '\n';
    }
    return out;
}

bool Network::load(std::string_view text, Network& out, std::string& error, std::vector<std::string>* warnings) {
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
    bool header = false;
    Node* current = nullptr;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
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
                warn(lineNo, current->name + " (" + t->label + ") has no parameter " + name + "; dropped");
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
        if (w[0] == "bypass") {
            if (!current) return fail(lineNo, "bypass belongs to the node above it, and there is none");
            current->bypass = true;
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
    net.revision_ = out.revision_ + 1;
    out = std::move(net);
    return true;
}

void Network::upgrade(const std::vector<Link>& links) {
    (void)links;
    for (Node& n : nodes_) {
        if (const Legacy* l = legacyType(n.type, n.version)) l->upgrade(n);
    }
}

// --- compile -------------------------------------------------------------------------

bool Compiled::errors() const {
    return std::any_of(problems.begin(), problems.end(), [](const Problem& p) { return p.level == Problem::Level::Error; });
}

bool Compiled::isActive(int node) const { return std::binary_search(active.begin(), active.end(), node); }

Compiled Network::compile() const {
    Compiled c;
    auto problem = [&](Problem::Level level, int node, std::string message) {
        c.problems.push_back({level, node, std::move(message)});
    };
    using L = Problem::Level;
    auto f = [&](const Node& n, const char* name) { return param(n.id, name)[0]; };
    auto v3 = [&](const Node& n, const char* name) {
        const ParamValue p = param(n.id, name);
        return Vec3(p[0], p[1], p[2]);
    };
    auto whole = [&](const Node& n, const char* name) { return static_cast<int>(std::lround(f(n, name))); };
    auto colliderOf = [&](const Node& n) {
        Collider col;
        col.shape = static_cast<Shape>(whole(n, "shape"));
        col.center = v3(n, "center");
        col.rotation = v3(n, "rotation");
        col.size = v3(n, "size");
        col.node = n.id;
        return col;
    };
    // Every return goes through here: `active` is searched, so sorted.
    auto done = [&]() {
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
        c.solids.push_back({colliderOf(n), v3(n, "color")});
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
        problem(L::Error, 0, "No Output node. Add one (Render > Output) and link a Volume Look into it.");
        return done();
    }
    c.output = output->id;
    c.active.push_back(output->id);
    c.frames = std::max(1, whole(*output, "frames"));

    auto upstream = [&](const Node& n, const char* input) -> const Node* {
        const std::vector<Link> in = linksInto(n.id, input);
        return in.empty() ? nullptr : node(in.front().from);
    };
    const Node* look = upstream(*output, "look");
    if (!look) {
        problem(L::Error, output->id, "Nothing to show: link a Volume Look into Look.");
        return done();
    }
    c.lookNode = look->id;
    c.active.push_back(look->id);
    Look& k = c.look;
    k.smokeColor = v3(*look, "smoke_color");
    k.smokeDensity = f(*look, "smoke_density");
    k.occlusion = f(*look, "occlusion");
    k.flameIntensity = f(*look, "flame_intensity");
    k.flameStart = f(*look, "flame_start");
    k.flameRange = f(*look, "flame_range");
    k.fireLight = f(*look, "fire_light");
    k.lightAzimuth = f(*look, "light_azimuth");
    k.lightElevation = f(*look, "light_elevation");
    k.lightColor = v3(*look, "light_color");
    k.lightIntensity = f(*look, "light_intensity");
    k.skyColor = v3(*look, "sky_color");
    k.skyIntensity = f(*look, "sky_intensity");
    k.exposure = f(*look, "exposure");
    k.floor = f(*look, "floor") != 0.0f;

    const Node* solver = upstream(*look, "gas");
    if (!solver) {
        problem(L::Error, look->id, "No gas to draw: link a Pyro Solver into Gas.");
        return done();
    }
    c.solver = solver->id;
    c.active.push_back(solver->id);
    SolverSettings& s = c.scene.solver;
    s.size = v3(*solver, "size");
    s.resolution = whole(*solver, "resolution");
    s.closedFloor = f(*solver, "closed_floor") != 0.0f;
    s.timeStep = 1.0f / std::max(f(*solver, "fps"), 1.0f);
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
    c.ok = true;

    // What feeds the solver, in the order it was linked. Bypassed nodes stay
    // out; so do nodes of a type this program does not know (reported above).
    auto feeding = [&](const char* input) {
        std::vector<const Node*> out;
        for (const Link& l : linksInto(solver->id, input)) {
            const Node* n = node(l.from);
            if (n && !n->bypass && findNodeType(n->type)) out.push_back(n);
        }
        return out;
    };
    for (const Node* n : feeding("sources")) {
        Emitter e;
        e.shape = static_cast<Shape>(whole(*n, "shape"));
        e.center = v3(*n, "center");
        e.rotation = v3(*n, "rotation");
        e.size = v3(*n, "size");
        e.fuel = f(*n, "fuel");
        e.smoke = f(*n, "smoke");
        e.heat = f(*n, "heat");
        e.velocity = v3(*n, "velocity");
        e.flicker = f(*n, "flicker");
        e.flickerSize = f(*n, "flicker_size");
        e.seed = static_cast<uint32_t>(whole(*n, "seed"));
        e.start = f(*n, "start");
        e.end = f(*n, "end");
        e.motion = static_cast<Motion>(whole(*n, "motion"));
        e.motionSize = f(*n, "motion_size");
        e.motionPeriod = f(*n, "motion_period");
        e.node = n->id;
        c.scene.emitters.push_back(e);
        c.active.push_back(n->id);
    }
    for (const Node* n : feeding("forces")) {
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
        c.scene.forces.push_back(force);
        c.active.push_back(n->id);
    }
    for (const Node* n : feeding("colliders")) {
        c.scene.colliders.push_back(colliderOf(*n));
        c.active.push_back(n->id);
    }

    // Things that run but will not do what was meant.
    const Domain domain = c.scene.sanitized().solver.domain();
    const Vec3 lo = domain.origin(), hi = domain.origin() + domain.size();
    auto overlaps = [&](const Vec3& a, const Vec3& b) {
        return a.x < hi.x && b.x > lo.x && a.y < hi.y && b.y > lo.y && a.z < hi.z && b.z > lo.z;
    };
    if (c.scene.emitters.empty()) {
        problem(L::Warning, solver->id, "No sources: nothing will appear. Link a source into Sources.");
    }
    for (const Emitter& e : c.scene.emitters) {
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
        for (const Collider& col : c.scene.colliders) {
            if (col.contains(e.center)) {
                problem(L::Warning, e.node, "Inside a collider: no gas comes out of a solid.");
                break;
            }
        }
    }
    for (const Collider& col : c.scene.colliders) {
        Vec3 a, b;
        col.instance().bounds(a, b);
        if (!overlaps(a, b)) problem(L::Warning, col.node, "Outside the solver's domain: nothing to collide with.");
    }
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
