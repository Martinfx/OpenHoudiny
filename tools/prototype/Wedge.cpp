#include "Wedge.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>

namespace pg::editor {

namespace fs = std::filesystem;

namespace {

/// A value as the folder's name and wedge.txt write it: the shortest that
/// reads back, a point whatever the locale.
std::string number(float v) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof text, v);
    return std::string(text, end.ptr);
}

}  // namespace

std::vector<float> Wedge::values(float from, float to, int count, bool whole) {
    std::vector<float> out;
    count = std::max(count, 1);
    for (int i = 0; i < count; ++i) {
        const float t = count == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(count - 1);
        float v = from + (to - from) * t;
        if (whole) v = std::round(v);
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

bool Wedge::start(const sim::Network& net, int node, const std::string& param, const std::vector<float>& values,
                  const std::string& root, const std::string& networkFolder, int frames, std::string& error) {
    if (running()) {
        error = "A wedge is baking already";
        return false;
    }
    const sim::Node* n = net.node(node);
    const sim::ParamDef* def = nullptr;
    if (n) {
        if (const sim::NodeType* t = sim::findNodeType(n->type)) {
            for (const sim::ParamDef& p : t->params) {
                if (p.name == param) def = &p;
            }
        }
    }
    if (!def || (def->kind != sim::ParamKind::Float && def->kind != sim::ParamKind::Int)) {
        error = "A wedge sets a number: a Float or an Int parameter";
        return false;
    }
    if (values.size() < 2) {
        error = "A wedge wants two values at least";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        error = root + ": cannot make it (" + ec.message() + ")";
        return false;
    }
    variants_.clear();
    texts_.clear();
    std::string note = "pgwedge 1\nnode " + n->name + "\nparam " + param + "\n";
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = std::clamp(values[i], def->lo, def->hi);
        sim::Network copy = net;
        copy.setParam(node, param, sim::ParamValue{v, 0.0f, 0.0f});
        Variant variant;
        variant.value = v;
        variant.folder = (fs::path(root) / (param + "_" + std::to_string(i + 1))).string();
        note += "variant " + fs::path(variant.folder).filename().string() + " " + number(v) + "\n";
        variants_.push_back(variant);
        texts_.push_back(copy.save());
    }
    if (!sim::writeWhole((fs::path(root) / "wedge.txt").string(), note, error)) return false;
    node_ = node;
    nodeName_ = n->name;
    param_ = param;
    root_ = root;
    networkFolder_ = networkFolder;
    frames_ = frames;
    cancelled_ = false;
    baking_ = -1;
    next();
    if (baking_ < 0) {
        error = variants_.empty() || variants_.front().why.empty() ? "The wedge did not start" : variants_.front().why;
        return false;
    }
    return true;
}

void Wedge::next() {
    baking_ = -1;
    if (cancelled_) return;
    for (size_t i = 0; i < variants_.size(); ++i) {
        Variant& v = variants_[i];
        if (v.state != Variant::State::Waiting) continue;
        std::string error;
        if (bake_.start(texts_[i], networkFolder_, v.folder, frames_, 10, false, error)) {
            v.state = Variant::State::Baking;
            baking_ = static_cast<int>(i);
            return;
        }
        v.state = Variant::State::Failed;
        v.why = error;
    }
}

void Wedge::cancel() {
    cancelled_ = true;
    bake_.cancel();
    for (Variant& v : variants_) {
        if (v.state == Variant::State::Waiting) v.state = Variant::State::Cancelled;
    }
}

bool Wedge::poll() {
    if (baking_ < 0) return false;
    bake_.poll();
    if (bake_.running()) return false;
    Variant& v = variants_[static_cast<size_t>(baking_)];
    v.seconds = bake_.seconds();
    if (!bake_.failed()) v.state = Variant::State::Done;
    else if (bake_.cancelled()) v.state = Variant::State::Cancelled;
    else {
        v.state = Variant::State::Failed;
        v.why = bake_.why();
    }
    next();
    return true;
}

bool Wedge::running() const { return baking_ >= 0; }

}  // namespace pg::editor
