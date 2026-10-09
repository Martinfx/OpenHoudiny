#include "Thumbnails.h"

#include <algorithm>

namespace pg::editor {

using namespace gl;

namespace {

/// A picture that changes as a simulation plays is drawn again at most this
/// often -- and, when drawing it takes long, as seldom as keeps it under a
/// twentieth of the time.
constexpr double kLiveEvery = 0.25;
constexpr double kLiveShare = 20.0;

}  // namespace

uint64_t mixKey(uint64_t key, const void* value, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(value);
    for (size_t i = 0; i < bytes; ++i) {
        key ^= p[i];
        key *= 1099511628211ull;
    }
    return key;
}

Thumbnails::~Thumbnails() {
    clear();
    collect();
    collect();
    if (readFbo_) gl_.DeleteFramebuffers(1, &readFbo_);
}

GLuint Thumbnails::texture(int node) const {
    const auto e = entries_.find(node);
    return e != entries_.end() ? e->second.texture : 0;
}

int Thumbnails::stale(int node, uint64_t key, uint64_t live, double now) const {
    const auto it = entries_.find(node);
    if (it == entries_.end()) return 1;
    const Entry& e = it->second;
    if (e.key != key) return 2;
    return e.live != live && now - e.drawn >= std::max(kLiveEvery, kLiveShare * e.ms / 1000.0) ? 3 : 0;
}

double Thumbnails::drawnAt(int node) const {
    const auto it = entries_.find(node);
    return it != entries_.end() ? it->second.drawn : 0.0;
}

void Thumbnails::take(int node, GLuint source, uint64_t key, uint64_t live, double now, double ms) {
    Entry& e = entries_[node];
    if (!e.texture) {
        gl_.GenTextures(1, &e.texture);
        gl_.BindTexture(TEXTURE_2D, e.texture);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), kWidth, kHeight, 0, RGBA, UNSIGNED_BYTE, nullptr);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        gl_.BindTexture(TEXTURE_2D, 0);
        gl_.GenFramebuffers(1, &e.fbo);
        gl_.BindFramebuffer(FRAMEBUFFER, e.fbo);
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, e.texture, 0);
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
    }
    if (!readFbo_) gl_.GenFramebuffers(1, &readFbo_);
    // Twice the size, averaged down: a linear blit at exactly 2:1 samples
    // each picture pixel between four of the source's.
    gl_.BindFramebuffer(READ_FRAMEBUFFER, readFbo_);
    gl_.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, source, 0);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, e.fbo);
    gl_.BlitFramebuffer(0, 0, 2 * kWidth, 2 * kHeight, 0, 0, kWidth, kHeight, COLOR_BUFFER_BIT,
                        static_cast<GLenum>(LINEAR));
    gl_.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
    gl_.BindFramebuffer(READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, 0);
    e.key = key;
    e.live = live;
    e.drawn = now;
    e.ms = ms;
}

void Thumbnails::put(int node, GLuint texture, uint64_t key, uint64_t live, double now, double ms) {
    Entry& e = entries_[node];
    if (e.fbo) gl_.DeleteFramebuffers(1, &e.fbo);
    if (e.texture) retired_.push_back(e.texture);
    e.fbo = 0;
    e.texture = texture;
    e.key = key;
    e.live = live;
    e.drawn = now;
    e.ms = ms;
}

void Thumbnails::collect() {
    if (!collecting_.empty()) gl_.DeleteTextures(static_cast<GLsizei>(collecting_.size()), collecting_.data());
    collecting_.swap(retired_);
    retired_.clear();
}

void Thumbnails::keep(const std::set<int>& nodes) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (nodes.count(it->first)) {
            ++it;
            continue;
        }
        release(it->second);
        it = entries_.erase(it);
    }
}

void Thumbnails::clear() {
    for (auto& [node, e] : entries_) release(e);
    entries_.clear();
}

void Thumbnails::release(Entry& e) {
    if (e.fbo) gl_.DeleteFramebuffers(1, &e.fbo);
    if (e.texture) gl_.DeleteTextures(1, &e.texture);
    e.fbo = e.texture = 0;
}

}  // namespace pg::editor
