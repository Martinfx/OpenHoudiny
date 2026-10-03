#pragma once
//
// The pictures in the nodes of a network (CanvasNode::thumbnail): a texture
// of each node's own, kWidth x kHeight, taken from a picture a renderer drew
// at twice that and averaged down -- edges without steps.
//
// Each remembers what it shows, as two keys: what the node is (its
// parameters, what it was cooked from) and what it is at the frame on
// screen (a simulation's frame). A picture is drawn again as soon as the
// first changes; for the second -- a simulation that plays -- no more often
// than its drawing allows: a picture that took long waits long, so the
// pictures never take more than a sliver of the window's time.
//
#include "pg/gl/Gl.h"

#include <cstdint>
#include <map>
#include <set>

namespace pg::editor {

class Thumbnails {
public:
    static constexpr int kWidth = 320, kHeight = 200;

    explicit Thumbnails(const gl::Api& gl) : gl_(gl) {}
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    /// The texture of `node`'s picture, 0 while it has none.
    gl::GLuint texture(int node) const;
    /// Whether `node`'s picture is to be drawn, and how urgently: 0 not,
    /// 1 it has none, 2 it shows another `key`, 3 another `live` and it is
    /// time -- `now`, seconds.
    int stale(int node, uint64_t key, uint64_t live, double now) const;
    /// When `node`'s picture was drawn; 0 if it has none.
    double drawnAt(int node) const;
    /// Takes the picture in `source` -- a texture of 2 kWidth x 2 kHeight,
    /// bottom row first, as a renderer drew it -- as `node`'s, showing `key`
    /// and `live`; drawing it took `ms`.
    void take(int node, gl::GLuint source, uint64_t key, uint64_t live, double now, double ms);
    /// Keeps the pictures of `nodes` alone.
    void keep(const std::set<int>& nodes);
    void clear();

private:
    struct Entry {
        gl::GLuint texture = 0, fbo = 0;
        uint64_t key = 0, live = 0;
        double drawn = 0.0, ms = 0.0;  ///< when, seconds; how long it took
    };
    void release(Entry& e);

    const gl::Api& gl_;
    gl::GLuint readFbo_ = 0;  ///< the source, attached to be read
    std::map<int, Entry> entries_;
};

/// A key made of `value`'s bytes, folded into `key` (FNV-1a).
uint64_t mixKey(uint64_t key, const void* value, size_t bytes);
template <typename T>
uint64_t mixKey(uint64_t key, const T& value) {
    return mixKey(key, &value, sizeof value);
}

}  // namespace pg::editor
