#include "PyroView.h"

#include "pg/gl/Png.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <utility>

namespace pg::editor {

// --- the simulation thread ------------------------------------------------------
//
// The solver and its fields belong to the thread. After each step it copies
// them into a frame of its own, with the shadows worked out, and swaps that
// with the frame the view takes -- the buffers go back and forth, nothing is
// allocated once they have their size.

class PyroSimulation {
public:
    PyroSimulation(const PyroRequest& request, bool threaded) : request_(request), solver_(request.settings) {
        if (threaded) thread_ = std::thread([this] { loop(); });
    }

    ~PyroSimulation() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void setRequest(const PyroRequest& request) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            request_ = request;
            changed_ = true;
        }
        cv_.notify_all();
    }

    void reset() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            reset_ = true;
        }
        cv_.notify_all();
    }

    void stepOnce() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++steps_;
        }
        cv_.notify_all();
    }

    /// Without a thread: one round on the caller's.
    void pump() {
        std::unique_lock<std::mutex> lock(mu_);
        if (changed_ || reset_ || steps_ > 0 || request_.playing) round(lock);
    }

    /// The newest frame, if the caller has not had it yet.
    bool take(PyroFrame& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!fresh_) return false;
        std::swap(out, latest_);
        fresh_ = false;
        return true;
    }

private:
    using Clock = std::chrono::steady_clock;

    void loop() {
        std::unique_lock<std::mutex> lock(mu_);
        auto work = [&] { return quit_ || changed_ || reset_ || steps_ > 0; };
        for (;;) {
            cv_.wait(lock, [&] { return work() || request_.playing; });
            if (quit_) return;
            if (!work() && request_.realTime) {
                // Playing: the next step is due one time step after the last.
                const auto due = lastStep_ + std::chrono::duration_cast<Clock::duration>(
                                                 std::chrono::duration<double>(request_.settings.timeStep));
                if (cv_.wait_until(lock, due, [&] { return work() || !request_.playing; })) continue;
            }
            round(lock);
        }
    }

    /// Takes what was asked for, works on it unlocked, publishes the frame.
    void round(std::unique_lock<std::mutex>& lock) {
        const PyroRequest request = request_;
        const bool changed = changed_, reset = reset_, step = request_.playing || steps_ > 0;
        changed_ = reset_ = false;
        if (steps_ > 0) --steps_;
        lock.unlock();

        if (changed) solver_.setSettings(request.settings);  // a new resolution starts again
        if (reset) solver_.reset();
        if (step) {
            lastStep_ = Clock::now();
            solver_.step();
            next_.stepMs = std::chrono::duration<double, std::milli>(Clock::now() - lastStep_).count();
        }
        next_.density = solver_.density();
        next_.temperature = solver_.temperature();
        next_.flame = solver_.flame();
        next_.shadows = gl::VolumeRenderer::shadows(solver_.density(), request.style, request.light);
        next_.frame = solver_.frame();
        next_.time = solver_.time();
        next_.cells[0] = solver_.nx();
        next_.cells[1] = solver_.ny();
        next_.cells[2] = solver_.nz();

        lock.lock();
        std::swap(next_, latest_);
        next_.stepMs = latest_.stepMs;
        fresh_ = true;
    }

    std::mutex mu_;
    std::condition_variable cv_;
    PyroRequest request_;
    bool changed_ = false, reset_ = false, quit_ = false;
    int steps_ = 0;       // single steps asked for
    PyroFrame latest_;    // the view takes this one
    bool fresh_ = false;
    PyroFrame next_;      // the thread fills this one
    sim::PyroSolver solver_;
    Clock::time_point lastStep_ = Clock::now();
    std::thread thread_;
};

// --- the view -----------------------------------------------------------------------

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kResolutions[] = {32, 48, 64, 80, 96, 128};

void help(const char* text) {
    if (text && *text) ImGui::SetItemTooltip("%s", text);
}

}  // namespace

PyroView::PyroView(const gl::Api& gl) : volume_(gl), frame_(std::make_unique<PyroFrame>()) {
    if (!volume_.init(shaderLog_) && shaderLog_.empty()) shaderLog_ = "the volume shader did not compile";
    // Live first: 48 cells across keeps up with the clock on a few cores.
    // `pgshader pyro`, which has time, starts at the solver's 64.
    loadPreset("fire", 48);
}

PyroView::~PyroView() = default;

bool PyroView::loadPreset(const std::string& name, int resolution) {
    PyroRequest request;
    if (name == "fire") {
        request.settings = sim::PyroSettings::fire();
        request.style = gl::VolumeStyle::fire();
    } else if (name == "smoke") {
        request.settings = sim::PyroSettings::smoke();
        request.style = gl::VolumeStyle::smoke();
    } else {
        return false;
    }
    // What the machine can take stays.
    request.settings.resolution = resolution > 0 ? std::clamp(resolution, 8, 256) : request_.settings.resolution;
    request.playing = request_.playing;
    request.realTime = request_.realTime;
    std::copy(std::begin(request_.light), std::end(request_.light), request.light);
    request_ = request;
    preset_ = name;
    // A new preset starts from an empty domain, on a new thread.
    simulation_ = std::make_unique<PyroSimulation>(request_, !synchronous_);
    hasFrame_ = false;
    apply();
    return true;
}

void PyroView::setSynchronous(bool on) {
    if (on == synchronous_) return;
    synchronous_ = on;
    simulation_ = std::make_unique<PyroSimulation>(request_, !synchronous_);
    apply();
}

void PyroView::apply() {
    const float az = lightAzimuth_ * kPi / 180.0f, el = lightElevation_ * kPi / 180.0f;
    request_.light[0] = std::cos(el) * std::cos(az);
    request_.light[1] = std::sin(el);
    request_.light[2] = std::cos(el) * std::sin(az);
    volume_.style = request_.style;
    std::copy(std::begin(request_.light), std::end(request_.light), volume_.lightDirection);
    simulation_->setRequest(request_);
}

void PyroView::shortcuts() {
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        request_.playing = !request_.playing;
        apply();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) simulation_->reset();
}

void PyroView::menuItems() {
    if (ImGui::MenuItem(request_.playing ? "Pause" : "Play", "Space")) {
        request_.playing = !request_.playing;
        apply();
    }
    if (ImGui::MenuItem("Step", nullptr, false, !request_.playing)) simulation_->stepOnce();
    if (ImGui::MenuItem("Start again", "Home")) simulation_->reset();
    ImGui::Separator();
    if (ImGui::MenuItem("Fire", nullptr, preset_ == "fire")) loadPreset("fire");
    if (ImGui::MenuItem("Smoke", nullptr, preset_ == "smoke")) loadPreset("smoke");
}

void PyroView::draw(float height) {
    if (synchronous_) simulation_->pump();
    if (simulation_->take(*frame_)) {
        volume_.upload(frame_->density, frame_->temperature, frame_->flame, frame_->shadows);
        hasFrame_ = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    const float width = ImGui::GetContentRegionAvail().x;
    panelWidth_ = std::clamp(panelWidth_, 300.0f, std::max(300.0f, width - 300.0f));
    const float splitter = 6.0f;

    viewport(width - panelWidth_ - splitter, height);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("pyro_splitter", ImVec2(splitter, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) panelWidth_ -= io.MouseDelta.x;
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::BeginChild("pyro_settings", ImVec2(panelWidth_, height));
    settingsPanel();
    ImGui::EndChild();
}

void PyroView::viewport(float width, float height) {
    ImGui::BeginChild("pyro_view", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (!shaderLog_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "The volume renderer is not available:");
        ImGui::TextWrapped("%s", shaderLog_.c_str());
    } else if (size.x >= 16.0f && size.y >= 16.0f) {
        const float scale = supersample_ ? 2.0f : 1.0f;
        if (hasFrame_) volume_.render(static_cast<int>(size.x * scale), static_cast<int>(size.y * scale));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (hasFrame_) {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(volume_.colorTexture())), size, ImVec2(0, 1),
                         ImVec2(1, 0));
        } else {
            ImGui::InvisibleButton("pyro_empty", size);
        }
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            gl::Orbit& orbit = volume_.orbit;
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                orbit.yaw -= io.MouseDelta.x * 0.4f;
                orbit.pitch = std::clamp(orbit.pitch + io.MouseDelta.y * 0.4f, -85.0f, 85.0f);
            }
            if (io.MouseWheel != 0.0f) orbit.distance = std::clamp(orbit.distance * (1.0f - io.MouseWheel * 0.08f), 1.0f, 12.0f);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) orbit = gl::VolumeRenderer::defaultOrbit();
        }
        // What is on screen, in the corner.
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + 10.0f, origin.y + 8.0f),
                                           ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                           (preset_ + "  |  " + status()).c_str());
    }
    ImGui::EndChild();
}

void PyroView::settingsPanel() {
    bool changed = false;
    // Presets and the transport.
    if (ImGui::Button("Fire")) loadPreset("fire");
    help("Fuel that burns into flames, heat and soot.");
    ImGui::SameLine();
    if (ImGui::Button("Smoke")) loadPreset("smoke");
    help("Hot smoke from a source, no fire.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(request_.playing ? "Pause" : "Play")) {
        request_.playing = !request_.playing;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(request_.playing);
    if (ImGui::Button("Step")) simulation_->stepOnce();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Start again")) simulation_->reset();

    int resolution = request_.settings.resolution;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::BeginCombo("Resolution", std::to_string(resolution).c_str())) {
        for (int r : kResolutions) {
            if (ImGui::Selectable(std::to_string(r).c_str(), r == resolution)) {
                request_.settings.resolution = r;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    help("Cells across the domain; it is 1.5 times as tall. Twice the resolution is 8 times the work.");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Real time", &request_.realTime);
    help("Never ahead of the clock. Off: as fast as the solver goes.");
    ImGui::Separator();

    // The settings of the solver, group by group, from its table.
    const char* group = nullptr;
    bool open = false;
    for (const sim::PyroParam& p : sim::pyroParams()) {
        if (!group || std::string(group) != p.group) {
            group = p.group;
            const bool fire = request_.settings.fuelRate > 0.0f;
            // Combustion matters only when there is fuel.
            const bool byDefault = std::string(group) != "Combustion" || fire;
            open = ImGui::CollapsingHeader(group, byDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }
        if (!open) continue;
        ImGui::PushID(p.name);
        changed |= ImGui::SliderFloat(p.label, &(request_.settings.*p.member), p.min, p.max, "%.3g");
        help(p.help);
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const gl::VolumeParam& p : gl::volumeParams()) {
            ImGui::PushID(p.name);
            changed |= ImGui::SliderFloat(p.label, &(request_.style.*p.member), p.min, p.max, "%.3g");
            help(p.help);
            ImGui::PopID();
        }
        changed |= ImGui::ColorEdit3("Smoke colour", request_.style.smokeColor, ImGuiColorEditFlags_Float);
        help("Share of the light the smoke scatters: pale for smoke, dark for soot.");
        changed |= ImGui::ColorEdit3("Light colour", request_.style.lightColor, ImGuiColorEditFlags_Float);
        changed |= ImGui::ColorEdit3("Sky colour", request_.style.skyColor, ImGuiColorEditFlags_Float);
        changed |= ImGui::SliderFloat("Light around", &lightAzimuth_, -180.0f, 180.0f, "%.0f deg");
        help("Where the sun is, around the domain.");
        changed |= ImGui::SliderFloat("Light height", &lightElevation_, -10.0f, 90.0f, "%.0f deg");
        ImGui::Checkbox("Supersample", &supersample_);
        help("Render the view at twice the size and scale it down: smoother, four times the work.");
    }

    if (ImGui::CollapsingHeader("Solver")) {
        float fps = 1.0f / request_.settings.timeStep;
        if (ImGui::SliderFloat("Steps a second", &fps, 10.0f, 120.0f, "%.0f")) {
            request_.settings.timeStep = 1.0f / fps;
            changed = true;
        }
        help("Simulated time per step: 1 / this. More steps, finer motion.");
        changed |= ImGui::SliderInt("Substeps", &request_.settings.substeps, 1, 4);
        help("Solver steps per step: for fast, violent gas.");
        changed |= ImGui::SliderInt("Pressure cycles", &request_.settings.pressureCycles, 1, 6);
        help("Multigrid V-cycles per step: how exactly the flow is kept from compressing.");
        int seed = static_cast<int>(request_.settings.seed);
        if (ImGui::InputInt("Seed", &seed)) {
            request_.settings.seed = static_cast<uint32_t>(std::max(seed, 0));
            changed = true;
        }
        help("The noise of the source and of the turbulence: another seed, another fire.");
    }
    if (changed) apply();
}

std::string PyroView::status() const {
    if (!hasFrame_) return "starting...";
    char text[160];
    std::snprintf(text, sizeof text, "%d x %d x %d cells  |  frame %d, %.1f s  |  %.0f ms a step%s", frame_->cells[0],
                  frame_->cells[1], frame_->cells[2], frame_->frame, frame_->time, frame_->stepMs,
                  request_.playing ? "" : "  |  paused");
    return text;
}

bool PyroView::saveImage(const std::string& path, int width, int height, std::string& error) {
    if (!hasFrame_ || !shaderLog_.empty()) {
        error = "nothing to save yet";
        return false;
    }
    volume_.render(width * 2, height * 2);
    if (!gl::writePng(path, width, height, 3, volume_.readPixels(2))) {
        error = path + ": cannot write";
        return false;
    }
    return true;
}

}  // namespace pg::editor
