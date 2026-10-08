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
                  const std::string& root, const std::string& networkFolder, int frames, std::string& error,
                  const std::vector<std::string>& cards) {
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
    lanes_.clear();
    for (const std::string& card : cards.empty() ? std::vector<std::string>{std::string()} : cards) {
        lanes_.emplace_back();
        lanes_.back().card = card;
    }
    next();
    if (!running()) {
        error = variants_.empty() || variants_.front().why.empty() ? "The wedge did not start" : variants_.front().why;
        return false;
    }
    return true;
}

void Wedge::next() {
    if (cancelled_) return;
    for (Lane& lane : lanes_) {
        if (lane.variant >= 0) continue;
        for (size_t i = 0; i < variants_.size(); ++i) {
            Variant& v = variants_[i];
            if (v.state != Variant::State::Waiting) continue;
            std::string error;
            if (lane.bake->start(texts_[i], networkFolder_, v.folder, frames_, 10, false, error, lane.card)) {
                v.state = Variant::State::Baking;
                v.card = lane.card;
                lane.variant = static_cast<int>(i);
                break;
            }
            v.state = Variant::State::Failed;
            v.why = error;
        }
    }
}

void Wedge::cancel() {
    cancelled_ = true;
    for (Lane& lane : lanes_) {
        if (lane.variant >= 0) lane.bake->cancel();
    }
    for (Variant& v : variants_) {
        if (v.state == Variant::State::Waiting) v.state = Variant::State::Cancelled;
    }
}

bool Wedge::poll() {
    bool ended = false;
    for (Lane& lane : lanes_) {
        if (lane.variant < 0) continue;
        lane.bake->poll();
        if (lane.bake->running()) continue;
        Variant& v = variants_[static_cast<size_t>(lane.variant)];
        v.seconds = lane.bake->seconds();
        if (!lane.bake->failed()) v.state = Variant::State::Done;
        else if (lane.bake->cancelled()) v.state = Variant::State::Cancelled;
        else {
            v.state = Variant::State::Failed;
            v.why = lane.bake->why();
        }
        lane.variant = -1;
        ended = true;
    }
    if (ended) next();
    return ended;
}

bool Wedge::running() const { return baking() > 0; }

int Wedge::baking() const {
    return static_cast<int>(std::count_if(lanes_.begin(), lanes_.end(), [](const Lane& l) { return l.variant >= 0; }));
}

int Wedge::ended() const {
    return static_cast<int>(std::count_if(variants_.begin(), variants_.end(), [](const Variant& v) {
        return v.state != Variant::State::Waiting && v.state != Variant::State::Baking;
    }));
}

const Bake* Wedge::bakeOf(size_t i) const {
    for (const Lane& lane : lanes_) {
        if (lane.variant == static_cast<int>(i)) return lane.bake.get();
    }
    return nullptr;
}

}  // namespace pg::editor
