#pragma once
//
// The Pyro workspace of the editor: smoke and fire simulated live
// (pg::sim::PyroSolver) and drawn by the volume renderer, the settings beside.
//
// The solver runs on a thread of its own, so the view stays smooth: the camera
// turns at the rate of the screen, the gas moves at the rate of the solver.
// The sliders are built from pg::sim::pyroParams() and pg::gl::volumeParams():
// a setting added to those tables shows up here without touching this file.
//
#include "pg/gl/Volume.h"
#include "pg/sim/Pyro.h"

#include <memory>
#include <string>

namespace pg::editor {

/// What the view asks of the simulation.
struct PyroRequest {
    sim::PyroSettings settings;
    gl::VolumeStyle style;  ///< for the shadows, which depend on the smoke's density
    float light[3] = {-0.75f, 0.6f, 0.15f};
    bool playing = true;
    bool realTime = true;   ///< never ahead of the clock: one time step per time step
};

/// One frame of the simulation, as the renderer needs it.
struct PyroFrame {
    sim::Grid density, temperature, flame, shadows;
    int frame = 0;
    float time = 0.0f;
    int cells[3] = {0, 0, 0};
    double stepMs = 0.0;
};

class PyroSimulation;

class PyroView {
public:
    explicit PyroView(const gl::Api& gl);
    ~PyroView();
    PyroView(const PyroView&) = delete;
    PyroView& operator=(const PyroView&) = delete;

    /// "fire" or "smoke": the settings and the look, from the start -- at
    /// `resolution` cells across, or the resolution it had (0).
    bool loadPreset(const std::string& name, int resolution = 0);
    /// One step per frame on the calling thread instead of a thread of its
    /// own: what --screenshot uses, so that N frames are N steps.
    void setSynchronous(bool on);

    /// Space, Home: play or pause, start again.
    void shortcuts();
    /// The items of the Simulation menu.
    void menuItems();
    /// The workspace, `height` pixels tall: the view and the settings.
    void draw(float height);
    /// "64 x 96 x 64 cells  |  frame 120, 4.0 s  |  78 ms a step"
    std::string status() const;
    /// The view at twice `width` x `height`, averaged down, as a PNG.
    bool saveImage(const std::string& path, int width, int height, std::string& error);

private:
    void viewport(float width, float height);
    void settingsPanel();
    /// Hands the request to the simulation, and the look to the renderer.
    void apply();

    gl::VolumeRenderer volume_;
    std::string shaderLog_;  ///< why the volume shader did not compile, if it did not
    PyroRequest request_;
    std::unique_ptr<PyroSimulation> simulation_;
    std::unique_ptr<PyroFrame> frame_;
    bool hasFrame_ = false;
    bool synchronous_ = false;
    bool supersample_ = false;
    std::string preset_ = "fire";
    float lightAzimuth_ = 169.0f, lightElevation_ = 38.0f;  ///< degrees
    float panelWidth_ = 400.0f;
};

}  // namespace pg::editor
