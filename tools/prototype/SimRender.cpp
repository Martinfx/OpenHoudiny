// The Render tab: the scene the viewport shows, rendered by the path tracer
// (RenderView) as it gets less noisy -- through the camera, or the view.
#include "SimWorkspace.h"

#include "Theme.h"
#include "Widgets.h"

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

sim::Camera SimWorkspace::renderCamera(int width, int height) const {
    if (throughCamera_ && compiled_.hasCamera) {
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

void SimWorkspace::renderTab(int width, int height) {
    if (!renderView_) renderView_ = std::make_unique<RenderView>();
    if (renderAutoPaused_) {
        renderView_->setPaused(false);
        renderAutoPaused_ = false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const render::Settings& fromOutput = compiled_.render;

    // The toolbar: go on or stop, start again, how big, save; how far it got.
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
    ImGui::SetNextItemWidth(theme::px(70.0f));
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
        const double mpaths = static_cast<double>(st.paths) / std::max(st.seconds, 1e-3) * 1e-6;
        std::snprintf(progress, sizeof progress, "%d / %d samples  \xc2\xb7  %.1f s  \xc2\xb7  %.2f M paths/s%s", st.samples,
                      st.of, st.seconds, mpaths, paused ? "  \xc2\xb7  paused" : st.samples >= st.of ? "  \xc2\xb7  done" : "");
    } else {
        std::snprintf(progress, sizeof progress, "starting\xe2\x80\xa6");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", progress);

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
    // the geometry, the view, the size.
    render::Settings settings = fromOutput;
    settings.width = rw;
    settings.height = rh;
    const sim::Camera cam = renderCamera(rw, rh);
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
    if (key != renderKey_ || !(settings == renderSettings_)) {
        RenderView::Request r;
        r.input.geometry = renderer_.geometry();
        r.input.look = renderer_.look;
        if (levels_.empty()) {
            r.frame = shown_;
            r.input.solids = compiled_.solidsAt(current_);
        }
        r.input.camera = cam;
        r.input.sunAngle = settings.sunAngle;
        const sim::Domain dm = sceneBox();
        r.input.domain.lo = dm.origin();
        r.input.domain.hi = dm.origin() + dm.size();
        r.settings = settings;
        r.scene = key;
        renderView_->request(std::move(r));
        renderKey_ = key;
        renderSettings_ = settings;
    }

    // The newest picture into the texture.
    std::vector<uint8_t> rgba;
    int pw = 0, ph = 0;
    if (renderView_->takePicture(rgba, pw, ph)) {
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
    gl::Orbit& o = renderer_.orbit;
    if (ImGui::IsItemActive()) {
        const ImVec2 dlt = io.MouseDelta;
        if (dlt.x != 0.0f || dlt.y != 0.0f) {
            setThroughCamera(false);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift)) {
                float fwd[3], right[3], up[3];
                o.axes(fwd, right, up);
                const float k = o.distance * 0.0018f;
                for (int a = 0; a < 3; ++a) o.target[a] += right[a] * (-dlt.x * k) + up[a] * (dlt.y * k);
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                o.distance = std::clamp(o.distance * std::exp(dlt.y * 0.006f), 0.2f, 200.0f);
            } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                o.yaw -= dlt.x * 0.35f;
                o.pitch = std::clamp(o.pitch + dlt.y * 0.35f, -89.0f, 89.0f);
            }
            viewDirty_ = true;
        }
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        setThroughCamera(false);
        o.distance = std::clamp(o.distance * std::pow(0.88f, io.MouseWheel), 0.2f, 200.0f);
        viewDirty_ = true;
    }
    if (!st.error.empty()) d->AddText(ImVec2(lo.x + theme::px(8.0f), lo.y + theme::px(8.0f)), IM_COL32(240, 120, 110, 255),
                                      st.error.c_str());
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
