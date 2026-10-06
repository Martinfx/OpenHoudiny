#include "FrameJob.h"

#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace pg::editor {
namespace {

constexpr const char* kPopup = "##framejob";

std::string duration(double s) {
    char text[32];
    if (s < 60.0) std::snprintf(text, sizeof text, "%.0f s", s);
    else if (s < 3600.0) std::snprintf(text, sizeof text, "%d min %02d s", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    else std::snprintf(text, sizeof text, "%d h %02d min", static_cast<int>(s) / 3600, static_cast<int>(s) % 3600 / 60);
    return text;
}

}  // namespace

FrameJob::~FrameJob() {
    stop_ = true;
    join();
}

void FrameJob::join() {
    if (thread_.joinable()) thread_.join();
}

bool FrameJob::start(std::string title, std::string what, int first, int last, Step step, Finish finish) {
    if (running_) return false;
    join();  // the one before, ended
    title_ = std::move(title);
    what_ = std::move(what);
    first_ = first;
    last_ = last;
    done_ = 0;
    stop_ = false;
    ended_ = false;
    running_ = true;
    started_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this, step = std::move(step), finish = std::move(finish)] {
        std::string error;
        bool ok = true;
        int done = 0;
        for (int f = first_; f <= last_ && !stop_; ++f) {
            if (!step(f, error)) {
                ok = false;
                break;
            }
            done_ = ++done;
        }
        std::string message;
        const bool finished = finish(done, stop_ && ok, message);
        {
            std::lock_guard<std::mutex> lock(mu_);
            message_ = ok ? message : error + (done > 0 ? " -- after " + std::to_string(done) + " frames" : "");
            failed_ = !ok || !finished;
        }
        ended_ = true;
        running_ = false;
    });
    return true;
}

bool FrameJob::takeResult(std::string& message, bool& failed) {
    if (!ended_) return false;
    ended_ = false;
    join();
    std::lock_guard<std::mutex> lock(mu_);
    message = message_;
    failed = failed_;
    return true;
}

void FrameJob::draw() {
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
    const int total = std::max(1, last_ - first_ + 1);
    const int done = done_;
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted(title_.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", what_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    char label[64];
    std::snprintf(label, sizeof label, "%d / %d", done, total);
    ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), ImVec2(-1.0f, 0.0f), label);
    const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    if (done > 0) {
        const double each = spent / done;
        ImGui::TextDisabled("%.2f s a frame  \xc2\xb7  %s left", each, duration(each * (total - done)).c_str());
    } else {
        ImGui::TextDisabled("The first frame\xe2\x80\xa6");
    }
    ImGui::Spacing();
    if (ImGui::Button(stop_ ? "Stopping\xe2\x80\xa6" : "Stop", ImVec2(theme::px(110.0f), 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        stop_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("the frames done stay");
    ImGui::EndPopup();
}

}  // namespace pg::editor
