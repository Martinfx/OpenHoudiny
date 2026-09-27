#include "RenderJob.h"

#include "Theme.h"

#include "pg/gl/Png.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace pg::editor {
namespace {

constexpr const char* kPopup = "Rendering##job";

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string duration(double seconds) {
    char buf[32];
    if (seconds < 60.0) std::snprintf(buf, sizeof buf, "%.0f s", seconds);
    else std::snprintf(buf, sizeof buf, "%d min %02d s", static_cast<int>(seconds) / 60, static_cast<int>(seconds) % 60);
    return buf;
}

}  // namespace

bool RenderJob::start(const std::string& target, const std::string& shown, const std::string& stem, int width, int height,
                      double fps, int first, int last, Draw draw, std::string& error) {
    if (running_) {
        error = "a render is still running";
        return false;
    }
    if (last < first) {
        error = "no frame to render";
        return false;
    }
    std::error_code ec;
    video_.reset();
    if (io::isVideoPath(target)) {
        const fs::path parent = fs::path(target).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        video_ = io::openVideo(target, width, height, fps, error);
        if (!video_) return false;
    } else {
        fs::create_directories(target, ec);
        if (!fs::is_directory(target, ec)) {
            error = target + ": cannot make the folder";
            return false;
        }
    }
    target_ = target;
    shown_ = shown;
    stem_ = stem;
    width_ = width;
    height_ = height;
    fps_ = fps;
    first_ = next_ = first;
    last_ = last;
    written_ = 0;
    drawnMs_ = 0.0;
    draw_ = std::move(draw);
    running_ = true;
    cancel_ = waiting_ = done_ = false;
    started_ = std::chrono::steady_clock::now();
    return true;
}

void RenderJob::step(double budgetMs) {
    if (!running_) return;
    if (cancel_) {
        finish("stopped", false);
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (next_ <= last_) {
        std::string error;
        const auto t = std::chrono::steady_clock::now();
        if (!draw_(next_, rgb_, error)) {
            if (!error.empty()) {
                finish(error, true);
                return;
            }
            waiting_ = true;  // on the simulation: next frame of the window, again
            return;
        }
        waiting_ = false;
        if (video_) {
            if (!video_->add(rgb_.data(), error)) {
                finish(error, true);
                return;
            }
        } else {
            char name[32];
            std::snprintf(name, sizeof name, "_%04d.png", next_);
            const std::string file = (fs::path(target_) / (stem_ + name)).string();
            if (!gl::writePng(file, width_, height_, 3, rgb_)) {
                finish(file + ": cannot write it", true);
                return;
            }
        }
        drawnMs_ += since(t);
        ++written_;
        ++next_;
        if (since(t0) >= budgetMs) break;
    }
    if (next_ > last_) finish("", false);
}

void RenderJob::finish(const std::string& why, bool failed) {
    std::string error;
    bool ok = !failed;
    if (video_) {
        // What was written stays a video, stopped or not.
        if (!video_->finish(error)) ok = false;
    }
    char rate[32];
    std::snprintf(rate, sizeof rate, "%g", fps_);
    const std::string what = video_ ? "Rendered " + std::to_string(written_) + " frames into " + shown_ + " (" + rate + " fps, " +
                                          video_->codec() + ")"
                                    : "Rendered " + std::to_string(written_) + " frames into " + shown_;
    if (!ok) message_ = (failed ? why : error) + (written_ > 0 ? " -- after " + std::to_string(written_) + " frames" : "");
    else if (!why.empty()) message_ = what + " -- " + why + " at frame " + std::to_string(next_);
    else message_ = what + " in " + duration(since(started_) / 1000.0);
    failed_ = !ok;
    path_ = target_;
    video_.reset();
    draw_ = nullptr;
    running_ = false;
    done_ = true;
}

bool RenderJob::takeResult(std::string& message, bool& failed, std::string& path) {
    if (!done_) return false;
    done_ = false;
    message = message_;
    failed = failed_;
    path = path_;
    return true;
}

void RenderJob::draw() {
    if (running_ && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(theme::px(460.0f), 0.0f));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    if (!running_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const int total = last_ - first_ + 1;
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted(video_ ? "Rendering a video" : "Rendering frames");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", shown_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    char label[64];
    std::snprintf(label, sizeof label, "%d / %d", written_, total);
    ImGui::ProgressBar(static_cast<float>(written_) / static_cast<float>(std::max(total, 1)), ImVec2(-1.0f, 0.0f), label);
    if (waiting_) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kYellow), "Waiting for the simulation to reach frame %d\xe2\x80\xa6",
                           next_);
    } else if (written_ > 0) {
        const double each = drawnMs_ / written_ / 1000.0;
        ImGui::TextDisabled("%.2f s a frame  \xc2\xb7  %s left", each, duration(each * (total - written_)).c_str());
    } else {
        ImGui::TextDisabled("The first frame\xe2\x80\xa6");
    }
    ImGui::Spacing();
    if (ImGui::Button(cancel_ ? "Stopping\xe2\x80\xa6" : "Stop", ImVec2(theme::px(110.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        cancel_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(video_ ? "what is rendered stays a video" : "the frames rendered stay");
    ImGui::EndPopup();
}

}  // namespace pg::editor
