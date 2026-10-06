// The Render tab: the scene the viewport shows, rendered by Cycles or the
// path tracer (RenderView) as it gets less noisy -- through the camera, or
// the view. And the shot rendered to the end by the same renderer, frame
// after frame, into a video or numbered PNGs (FrameRender, RenderJob).
#include "SimWorkspace.h"

#include "Theme.h"
#include "Widgets.h"
#include "pg/render/Cycles.h"
#include "pg/render/Denoise.h"
#include "pg/render/Plate.h"

#include <cmath>
#include <cstring>
#include <filesystem>

namespace pg::editor {

namespace fs = std::filesystem;
using theme::Icon;

namespace {

uint64_t mixed(uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 0x100000001b3ull; }
uint64_t mixed(uint64_t h, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return mixed(h, static_cast<uint64_t>(bits));
}

constexpr int kScales[] = {25, 50, 100};

}  // namespace

sim::Camera SimWorkspace::renderCamera(int width, int height, bool camera) const {
    if (camera && compiled_.hasCamera) {
        sim::Camera c = compiled_.cameraAt(current_);
        c.width = width;
        c.height = height;
        return c;
    }
    // The view's: its lens as wide as the viewport's.
    sim::Camera c;
    c.focal = 12.0f / std::tan(renderer_.orbit.fovY * 3.14159265f / 360.0f);
    c.width = width;
    c.height = height;
    return gl::cameraFrom(renderer_.orbit, c);
}

RenderView& SimWorkspace::renderView() {
    if (!renderView_) {
        renderView_ = std::make_unique<RenderView>();
        // Cycles' first pictures are of fewer, larger pixels anyway: the
        // whole pane from the start.
        if (renderView_->engine() == RenderView::Engine::Cycles) renderScale_ = 2;
    }
    return *renderView_;
}

RenderView::Request SimWorkspace::renderRequest(int width, int height, bool camera) {
    RenderView::Request r;
    r.input.geometry = renderer_.geometry();
    r.input.look = renderer_.look;
    if (levels_.empty()) {
        r.frame = shown_;
        r.input.solids = compiled_.solidsAt(current_);
    }
    r.input.camera = renderCamera(width, height, camera);
    // Where the shot's camera is a frame before and after, for Cycles'
    // blur.
    const bool shot = camera && compiled_.hasCamera;
    r.input.cameraMotion = shot && !compiled_.poses.empty();
    if (r.input.cameraMotion) {
        r.input.cameraBefore = compiled_.cameraAt(current_ - 1);
        r.input.cameraAfter = compiled_.cameraAt(current_ + 1);
    }
    r.input.frameTime = compiled_.world.timeStep;
    // Through the shot's camera, its plate behind the CG.
    if (shot) {
        const sim::Camera& c = compiled_.cameraAt(current_);
        const std::string file = c.plateFile(current_);
        if (file.empty()) {
            renderPlate_.reset();
        } else if (!renderPlate_ || renderPlate_->file != file || !(renderPlate_->camera == c.sanitized())) {
            // One that cannot be read: none -- the viewport says why.
            std::string why;
            renderPlate_ = render::loadPlate(file, c, why);
        }
        r.input.plate = renderPlate_;
    }
    r.settings = compiled_.render;
    r.settings.width = width;
    r.settings.height = height;
    r.input.sunAngle = r.settings.sunAngle;
    r.input.time = static_cast<float>(current_ - 1) * compiled_.world.timeStep;
    const sim::Domain dm = sceneBox();
    r.input.domain.lo = dm.origin();
    r.input.domain.hi = dm.origin() + dm.size();
    return r;
}

void SimWorkspace::finalSize(int& width, int& height) const {
    shotSize(width, height);
    const float scale = static_cast<float>(kScales[renderScale_]) / 100.0f;
    width = std::clamp(static_cast<int>(std::lround(static_cast<float>(width) * scale)), 16, 4096);
    height = std::clamp(static_cast<int>(std::lround(static_cast<float>(height) * scale)), 16, 4096);
}

std::string SimWorkspace::finalRenderer() {
    return renderView().engine() == RenderView::Engine::Cycles ? "Cycles" : "the Path Tracer";
}

void SimWorkspace::renderTab(int width, int height) {
    renderView();
    // While a render to the end runs, the tab shows its frames: its own
    // render waits, paused.
    const bool job = job_.running() && jobFinal_;
    if (renderAutoPaused_ && !job) {
        renderView_->setPaused(false);
        renderAutoPaused_ = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const render::Settings& fromOutput = compiled_.render;

    // The toolbar: go on or stop, start again, how big, save, the whole
    // shot; how far it got.
    RenderView::Status st = renderView_->status();
    const bool paused = renderView_->paused();
    if (theme::iconButton("render.go", paused ? Icon::Play : Icon::Pause, paused ? "Go on rendering" : "Pause")) {
        renderView_->setPaused(!paused);
    }
    ImGui::SameLine();
    if (theme::iconButton("render.again", Icon::Reset, "Render again from nothing")) renderView_->restart();
    ImGui::SameLine();
    if (theme::iconButton("render.save", Icon::Camera, "Save the render: PNG, or EXR with depth, albedo and normals\xe2\x80\xa6",
                          false, st.samples > 0)) {
        files_.open("Save render", {".png", ".exr"}, true, (fs::path(renderFolder()) / (stem() + "_render.png")).string());
        fileAction_ = FileAction::SaveRender;
    }
    ImGui::SameLine();
    // Every frame of the shot rendered to the end by this tab's renderer:
    // into a video, or a folder of PNGs.
    const std::string with = finalRenderer();
    if (theme::iconButton("render.shot", Icon::Film,
                          ("Render the shot with " + with + ": every frame, to the end, into a video or PNGs\xe2\x80\xa6").c_str(),
                          false, compiled_.ok)) {
        ImGui::OpenPopup("render.shot.menu");
    }
    if (ImGui::BeginPopup("render.shot.menu")) {
        int fw = 0, fh = 0;
        finalSize(fw, fh);
        ImGui::TextDisabled("With %s, %d \xc3\x97 %d, %d samples", with.c_str(), fw, fh, std::max(1, fromOutput.samples));
        if (ImGui::MenuItem("Video\xe2\x80\xa6")) chooseVideo(true);
        if (ImGui::MenuItem("Frames (PNG)\xe2\x80\xa6")) chooseFrames(true);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    // Which renderer: Cycles, Blender's, when the build has it -- or ours.
    const RenderView::Engine engine = renderView_->engine();
    // A combo as wide as its longest choice and its arrow.
    auto comboWidth = [](std::initializer_list<const char*> choices) {
        float w = 0.0f;
        for (const char* c : choices) w = std::max(w, ImGui::CalcTextSize(c).x);
        return w + 2.0f * ImGui::GetStyle().FramePadding.x + ImGui::GetFrameHeight() + theme::px(2.0f);
    };
    ImGui::SetNextItemWidth(comboWidth({RenderView::engineName(RenderView::Engine::Cycles),
                                        RenderView::engineName(RenderView::Engine::PathTracer)}));
    if (ImGui::BeginCombo("##render.engine", RenderView::engineName(engine))) {
        for (const RenderView::Engine e : {RenderView::Engine::Cycles, RenderView::Engine::PathTracer}) {
            const bool can = e != RenderView::Engine::Cycles || render::cyclesAvailable();
            if (ImGui::Selectable(RenderView::engineName(e), e == engine, can ? 0 : ImGuiSelectableFlags_Disabled)) {
                renderView_->setEngine(e);
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(render::cyclesAvailable() ? "Cycles: Blender's renderer. Path tracer: ours"
                                                    : "Cycles: not in this build (it needs OpenImageIO)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth({"100 %"}));
    char scaleText[16];
    std::snprintf(scaleText, sizeof scaleText, "%d %%", kScales[renderScale_]);
    if (ImGui::BeginCombo("##render.scale", scaleText)) {
        for (int k = 0; k < 3; ++k) {
            char item[16];
            std::snprintf(item, sizeof item, "%d %%", kScales[k]);
            if (ImGui::Selectable(item, k == renderScale_)) renderScale_ = k;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(throughCamera_ ? "Size: of the camera's picture" : "Size: of the pane");
    }
    ImGui::SameLine();
    if (theme::iconButton("render.settings", Icon::Output,
                          "Settings: the Output node's Render section -- samples, bounces, denoise, depth of field, "
                          "the sun's size")) {
        int output = 0;
        for (const sim::Node& n : net_.nodes()) {
            if (n.type == "output") output = n.id;
        }
        if (output) canvas_.select(output);
        else setMessage("No Output node: the render uses its defaults. Add one (Tab > Output) to set them.");
    }
    ImGui::SameLine();
    char progress[160];
    if (st.building) {
        std::snprintf(progress, sizeof progress, "building the scene\xe2\x80\xa6");
    } else if (st.of > 0) {
        // Paths a second: the path tracer's count; Cycles keeps none.
        char rate[48] = "";
        if (st.paths > 0) {
            std::snprintf(rate, sizeof rate, "  \xc2\xb7  %.2f M paths/s",
                          static_cast<double>(st.paths) / std::max(st.seconds, 1e-3) * 1e-6);
        }
        std::snprintf(progress, sizeof progress, "%d / %d samples  \xc2\xb7  %.1f s%s%s", st.samples, st.of, st.seconds,
                      rate, paused ? "  \xc2\xb7  paused" : st.samples >= st.of ? "  \xc2\xb7  done" : "");
    } else {
        std::snprintf(progress, sizeof progress, "starting\xe2\x80\xa6");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", progress);
    if (engine == RenderView::Engine::Cycles) {
        const std::string denoiser = render::cyclesDenoiser();
        ImGui::SetItemTooltip("%s, Blender's renderer; the gas %s; %s", render::cyclesVersion().c_str(),
                              render::gasAvailable() ? "in it" : "not rendered: built without NanoVDB",
                              denoiser.empty() ? "the noise left in: built without Open Image Denoise"
                                               : ("the noise taken out by " + denoiser).c_str());
    } else {
        ImGui::SetItemTooltip("Rays through %s; the gas %s; the noise taken out by %s",
                              render::rayEngineName(render::defaultRayEngine()).c_str(),
                              render::gasAvailable() ? ("through " + render::gasLibrary()).c_str()
                                                     : "not rendered: built without NanoVDB",
                              render::denoiserName(render::defaultDenoiser()).c_str());
    }

    // What it renders: the picture's size -- the camera's, or the pane's --
    // times the scale.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int paneW = std::max(16, static_cast<int>(avail.x)), paneH = std::max(16, static_cast<int>(avail.y));
    (void)width;
    (void)height;
    const bool camera = throughCamera_ && compiled_.hasCamera;
    const float scale = static_cast<float>(kScales[renderScale_]) / 100.0f;
    int rw = camera ? compiled_.cameraAt(current_).width : paneW;
    int rh = camera ? compiled_.cameraAt(current_).height : paneH;
    rw = std::clamp(static_cast<int>(std::lround(static_cast<float>(rw) * scale)), 16, 4096);
    rh = std::clamp(static_cast<int>(std::lround(static_cast<float>(rh) * scale)), 16, 4096);

    // Asked again whenever what it shows changes: the network, the frame,
    // the geometry, the view, the size -- not while a render to the end
    // runs: the tab shows its frames.
    render::Settings settings = fromOutput;
    settings.width = rw;
    settings.height = rh;
    const sim::Camera cam = renderCamera(rw, rh, throughCamera_);
    uint64_t key = mixed(0xcbf29ce484222325ull, static_cast<uint64_t>(compiledRevision_));
    key = mixed(key, static_cast<uint64_t>(current_));
    key = mixed(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shown_.get())));
    key = mixed(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(renderer_.geometry().get())));
    key = mixed(key, static_cast<uint64_t>(levels_.size()));
    for (const float v : {cam.position.x, cam.position.y, cam.position.z, cam.rotation.x, cam.rotation.y, cam.rotation.z,
                          cam.focal}) {
        key = mixed(key, v);
    }
    key = mixed(key, static_cast<uint64_t>(rw) << 32 | static_cast<uint64_t>(rh));
    if (!job && (key != renderKey_ || !(settings == renderSettings_))) {
        RenderView::Request r = renderRequest(rw, rh, throughCamera_);
        r.scene = key;
        renderView_->request(std::move(r));
        renderKey_ = key;
        renderSettings_ = settings;
    }

    // The newest picture into the texture.
    std::vector<uint8_t> rgba;
    int pw = 0, ph = 0;
    if (!job && renderView_->takePicture(rgba, pw, ph)) {
        if (!renderTexture_) gl_.GenTextures(1, &renderTexture_);
        gl_.BindTexture(gl::TEXTURE_2D, renderTexture_);
        gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
        gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
        gl_.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
        gl_.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(gl::RGBA8), pw, ph, 0, gl::RGBA, gl::UNSIGNED_BYTE, rgba.data());
        gl_.BindTexture(gl::TEXTURE_2D, 0);
        renderTextureW_ = pw;
        renderTextureH_ = ph;
    }

    // The picture, as big as the pane allows, on dark grey.
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const ImVec2 hi(lo.x + static_cast<float>(paneW), lo.y + static_cast<float>(paneH));
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(lo, hi, IM_COL32(24, 24, 27, 255));
    if (renderTexture_ && renderTextureW_ > 0) {
        const float fw = static_cast<float>(paneW), fh = static_cast<float>(paneH);
        const float aspect = static_cast<float>(renderTextureW_) / static_cast<float>(renderTextureH_);
        float iw = fw, ih = fh;
        if (fw / fh > aspect) iw = fh * aspect;
        else ih = fw / aspect;
        const ImVec2 a(lo.x + 0.5f * (fw - iw), lo.y + 0.5f * (fh - ih));
        d->AddImage(ImTextureRef(static_cast<ImTextureID>(renderTexture_)), a, ImVec2(a.x + iw, a.y + ih));
    }
    ImGui::SetCursorScreenPos(lo);
    ImGui::InvisibleButton("render.view", ImVec2(static_cast<float>(paneW), static_cast<float>(paneH)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);

    // The view turns here as in the viewport; the render starts again.
    // Leaving the camera, the view starts from where the camera is.
    auto leaveCamera = [&] {
        if (throughCamera_ && compiled_.hasCamera) {
            const sim::Camera& c = compiled_.cameraAt(current_);
            renderer_.orbit = gl::orbitThrough(c, focusOf(c));
        }
        setThroughCamera(false);
    };
    const bool hovered = ImGui::IsItemHovered();
    gl::Orbit& o = renderer_.orbit;
    if (ImGui::IsItemActive()) {
        const ImVec2 dlt = io.MouseDelta;
        if (dlt.x != 0.0f || dlt.y != 0.0f) {
            leaveCamera();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift)) {
                Vec3 fwd, right, up;
                o.axes(fwd, right, up);
                const float k = o.distance * 0.0018f;
                o.target += right * (-dlt.x * k) + up * (dlt.y * k);
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                o.distance = std::clamp(o.distance * std::exp(dlt.y * 0.006f), 0.2f, 200.0f);
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                o.yaw -= dlt.x * 0.35f;
                o.pitch = std::clamp(o.pitch + dlt.y * 0.35f, -89.0f, 89.0f);
            }
            viewDirty_ = true;
        }
    }
    if (hovered && io.MouseWheel != 0.0f) {
        leaveCamera();
        o.distance = std::clamp(o.distance * std::pow(0.88f, io.MouseWheel), 0.2f, 200.0f);
        viewDirty_ = true;
    }
    // 0: through the camera, or not -- as in the viewport.
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_0, false)) setThroughCamera(!throughCamera_);
    if (!st.error.empty()) {
        ui::overlayText(d, ImVec2(lo.x + theme::px(10.0f), lo.y + theme::px(10.0f)), IM_COL32(240, 120, 110, 255),
                        st.error.c_str());
    } else if (!(renderTexture_ && renderTextureW_ > 0) || (job && !jobShown_)) {
        // Nothing to show yet: what it waits for, in the middle.
        const char* what = job ? "Rendering the shot\xe2\x80\xa6"
                           : st.building ? "Building the scene\xe2\x80\xa6"
                                         : "Starting the render\xe2\x80\xa6";
        const ImVec2 t = ImGui::CalcTextSize(what);
        ui::overlayText(d, ImVec2(std::floor((lo.x + hi.x - t.x) * 0.5f), std::floor((lo.y + hi.y - t.y) * 0.5f)),
                        theme::kTextDim, what);
    }
}

bool SimWorkspace::renderShotFrame(int frame, std::vector<uint8_t>& rgb, std::string& error) {
    FrameRender& fr = *frameRender_;
    const FrameRender::Progress p = fr.progress();
    char line[160];
    if (p.state == FrameRender::State::Done && jobFrame_ == frame) {
        if (!fr.take(rgb, error)) return false;  // why: the job stops
        showFinalFrame(rgb);
        return true;
    }
    if (p.state == FrameRender::State::Building || p.state == FrameRender::State::Rendering) {
        if (p.state == FrameRender::State::Building) {
            std::snprintf(line, sizeof line, "Frame %d: making the scene\xe2\x80\xa6", frame);
        } else if (p.samples == 0) {
            // Cycles takes the scene in -- meshes, hierarchies, the gas --
            // before the first sample.
            std::snprintf(line, sizeof line, "Frame %d: %s gets the scene ready\xe2\x80\xa6  \xc2\xb7  %.0f s", frame,
                          RenderView::engineName(jobEngine_), p.seconds);
        } else {
            std::snprintf(line, sizeof line, "Frame %d: %d / %d samples  \xc2\xb7  %.0f s", frame, p.samples, p.of, p.seconds);
        }
        job_.tell(line, p.of > 0 ? static_cast<float>(p.samples) / static_cast<float>(p.of) : 0.0f);
        return false;
    }
    // Not started: the frame posed as the viewport shows it then -- the
    // simulation's frame, the objects and the look, the displayed geometry
    // cooked for it -- then rendered on the thread, through the shot's
    // camera.
    const std::shared_ptr<const sim::Frame> f = jobSimFrame(frame, error);
    if (!f) return false;
    current_ = frame;
    if (f != shown_) {
        shown_ = f;
        renderer_.setFrame(*f);
    }
    pose(frame);
    updatePieces(true);
    updateGeometry();
    if (cookedSerial_ != cookSerial_) {
        std::snprintf(line, sizeof line, "Frame %d: cooking the geometry\xe2\x80\xa6", frame);
        job_.tell(line);
        return false;
    }
    jobFrame_ = frame;
    fr.start(jobEngine_, renderRequest(jobWidth_, jobHeight_, true));
    std::snprintf(line, sizeof line, "Frame %d: making the scene\xe2\x80\xa6", frame);
    job_.tell(line);
    return false;
}

void SimWorkspace::showFinalFrame(const std::vector<uint8_t>& rgb) {
    const size_t n = static_cast<size_t>(jobWidth_) * static_cast<size_t>(jobHeight_);
    if (rgb.size() < n * 3) return;
    std::vector<uint8_t> rgba(n * 4);
    for (size_t i = 0; i < n; ++i) {
        rgba[4 * i] = rgb[3 * i];
        rgba[4 * i + 1] = rgb[3 * i + 1];
        rgba[4 * i + 2] = rgb[3 * i + 2];
        rgba[4 * i + 3] = 255;
    }
    if (!renderTexture_) gl_.GenTextures(1, &renderTexture_);
    gl_.BindTexture(gl::TEXTURE_2D, renderTexture_);
    gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl_.PixelStorei(gl::UNPACK_ALIGNMENT, 1);
    gl_.TexImage2D(gl::TEXTURE_2D, 0, static_cast<gl::GLint>(gl::RGBA8), jobWidth_, jobHeight_, 0, gl::RGBA, gl::UNSIGNED_BYTE,
                   rgba.data());
    gl_.BindTexture(gl::TEXTURE_2D, 0);
    renderTextureW_ = jobWidth_;
    renderTextureH_ = jobHeight_;
    jobShown_ = true;
}

void SimWorkspace::stopRender() {
    // Left for the viewport: the processor is the window's again.
    if (renderView_ && !renderView_->paused()) {
        renderView_->setPaused(true);
        renderAutoPaused_ = true;
    }
}

void SimWorkspace::saveRender(const std::string& path) {
    if (!renderView_) return;
    std::string error;
    if (renderView_->save(path, "prototype editor, frame " + std::to_string(current_), error)) {
        renderFolder_ = fs::path(path).parent_path().string();
        setMessage("Saved the render to " + path);
    } else {
        setMessage(error, true);
    }
}

}  // namespace pg::editor
