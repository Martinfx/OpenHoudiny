#include "pg/sim/Prepared.h"

#include "pg/core/Instances.h"
#include "pg/core/Lod.h"
#include "pg/io/Picture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace pg::sim {
namespace {

/// Topologies are numbered across all preparers: a renderer handed one
/// prepared by another does not take it for a mesh of its own.
std::atomic<uint64_t> topologies{0};

/// A picture file's pixels, `size` square -- each the average of the
/// file's under it, read between them -- as shown (sRGB) or as they are.
/// Empty when it cannot be read.
std::vector<float> squarePicture(const std::string& file, int size) {
    io::Picture picture;
    std::string error;
    if (file.empty() || !io::readPicture(file, picture, error) || picture.empty()) return {};
    std::vector<float> out(static_cast<size_t>(size) * size * 4);
    // A few samples a pixel: enough for a picture no more than 4 times as large.
    const int taps = std::clamp((std::max(picture.width, picture.height) + size - 1) / size, 1, 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int j = 0; j < taps; ++j) {
                for (int i = 0; i < taps; ++i) {
                    const int px = std::min(static_cast<int>((x + (i + 0.5f) / taps) * picture.width / size), picture.width - 1);
                    const int py = std::min(static_cast<int>((y + (j + 0.5f) / taps) * picture.height / size), picture.height - 1);
                    const float* p = picture.pixel(px, py);
                    for (int c = 0; c < 4; ++c) sum[c] += c < 3 && picture.linear ? io::linearToSrgb(p[c]) : p[c];
                }
            }
            float* o = &out[(static_cast<size_t>(y) * size + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = sum[c] / static_cast<float>(taps * taps);
        }
    }
    return out;
}

uint8_t byteOf(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

std::shared_ptr<const PictureBytes> pictureBytes(const DisplayPicture& picture, int size) {
    const size_t pixels = static_cast<size_t>(size) * size;
    // Its colour, the alpha its own or its grey's; its normal map, the
    // green as OpenGL has it -- or flat.
    const std::vector<float> color = squarePicture(picture.color, size);
    if (color.empty()) return nullptr;
    std::vector<float> alpha;
    if (!picture.alpha.empty() && !(picture.alphaChannel && picture.alpha == picture.color)) {
        alpha = squarePicture(picture.alpha, size);
    }
    const std::vector<float> normal = squarePicture(picture.normal, size);
    auto out = std::make_shared<PictureBytes>();
    out->color.resize(pixels * 4);
    out->normal.resize(pixels * 4);
    std::vector<uint8_t>& rgba = out->color;
    std::vector<uint8_t>& nrm = out->normal;
    for (size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = byteOf(color[i * 4 + c]);
        float a = 1.0f;
        if (!picture.alpha.empty()) {
            a = alpha.empty() ? color[i * 4 + 3] : (alpha[i * 4] + alpha[i * 4 + 1] + alpha[i * 4 + 2]) / 3.0f;
        }
        rgba[i * 4 + 3] = byteOf(a);
        if (normal.empty()) {
            nrm[i * 4] = nrm[i * 4 + 1] = 128;
            nrm[i * 4 + 2] = 255;
        } else {
            nrm[i * 4] = byteOf(normal[i * 4]);
            nrm[i * 4 + 1] = byteOf(picture.normalDirectX ? 1.0f - normal[i * 4 + 1] : normal[i * 4 + 1]);
            nrm[i * 4 + 2] = byteOf(normal[i * 4 + 2]);
        }
        nrm[i * 4 + 3] = 255;
    }
    return out;
}

std::shared_ptr<const PictureBytes> PreparedGeometry::bytesOf(const DisplayPicture& picture) const {
    for (const auto& [p, bytes] : pictures) {
        if (p == picture) return bytes;
    }
    return nullptr;
}

bool hasFoliage(const Geometry& geo) {
    const AttributeArray* through = geo.primitives().find("translucency");
    if (!through || through->type() != AttrType::Float) return false;
    const auto lets = through->read<float>();
    return std::any_of(lets.begin(), lets.end(), [](float t) { return t > 0.0f; });
}

std::shared_ptr<const PreparedGeometry> GeometryPreparer::prepare(const GeometryPtr& geometry) {
    if (!geometry) return nullptr;
    if (last_ && last_->geometry == geometry) return last_;
    auto out = std::make_shared<PreparedGeometry>();
    out->geometry = geometry;

    // What stands on its points. Each prototype as it was prepared before;
    // a new one indexed -- what stands on its own points copied first, a
    // plant thinned for far away.
    out->instances = instancesOf(*geometry);
    decltype(prototypes_) kept;
    for (size_t k = 0; k < out->instances.prototypes.size(); ++k) {
        const GeometryPtr& prototype = out->instances.prototypes[k];
        auto& [held, meshes] = kept[prototype.get()];
        if (meshes.empty()) {
            const auto was = prototypes_.find(prototype.get());
            if (was != prototypes_.end() && was->second.first == prototype) {
                meshes = was->second.second;
            } else {
                const int levels = hasFoliage(*prototype) ? static_cast<int>(kDetailLevels) : 1;
                const GeometryPtr whole = prototype->prototypeCount() > 0 ? GeometryPtr(unpackInstances(*prototype)) : prototype;
                for (int level = 0; level < levels; ++level) {
                    const GeometryPtr made =
                        level == 0 ? whole : std::make_shared<Geometry>(plantDetail(*whole, kDetailKeep[static_cast<size_t>(level)]));
                    auto mesh = std::make_shared<DisplayMesh>();
                    DisplayMesher().make(made, *mesh);
                    meshes.push_back(std::move(mesh));
                }
            }
            held = prototype;
        }
        const int levels = static_cast<int>(meshes.size());
        for (int level = 0; level < levels; ++level) out->prototypes.push_back({k, level, levels, meshes[static_cast<size_t>(level)]});
    }
    prototypes_ = std::move(kept);

    // Its polygons: the last mesh moved, when only the points did.
    DisplayMesh mesh;
    if (last_ && last_->mesh && mesher_.sameMaking(*geometry)) mesh = *last_->mesh;
    const bool moved = mesher_.make(geometry, mesh) == DisplayMesher::Made::Moved;
    out->topology = moved ? last_->topology : ++topologies;
    out->mesh = std::make_shared<const DisplayMesh>(std::move(mesh));
    out->rest = mesher_.hasRest();
    // The rest; none, when it had none and only the points moved.
    if (moved && !mesher_.hasRest()) {
        out->display.lo = out->mesh->lo;
        out->display.hi = out->mesh->hi;
    } else {
        out->display = displayOf(*geometry, 400000, false);
    }

    // The pictures the meshes lay on: read once, while they are laid.
    auto read = [&](const DisplayMesh& m) {
        for (const DisplayPicture& p : m.pictures) {
            if (out->bytesOf(p)) continue;
            std::shared_ptr<const PictureBytes> bytes = last_ ? last_->bytesOf(p) : nullptr;
            if (!bytes) bytes = pictureBytes(p);
            if (bytes) out->pictures.emplace_back(p, std::move(bytes));
        }
    };
    read(*out->mesh);
    for (const PreparedGeometry::Prototype& p : out->prototypes) read(*p.mesh);
    last_ = out;
    return out;
}

std::string bodiesKey(const Look& look) {
    char key[400];
    std::snprintf(key, sizeof key, "%d %g %g %g %g %g %g %g %g %g %d %g %g %g %d %g %g %g ", look.pieces ? 1 : 0,
                  look.piecesColor.x, look.piecesColor.y, look.piecesColor.z, look.piecesInside.x, look.piecesInside.y,
                  look.piecesInside.z, look.rebarColor.x, look.rebarColor.y, look.rebarColor.z, look.cloth ? 1 : 0,
                  look.clothColor.x, look.clothColor.y, look.clothColor.z, look.grains ? 1 : 0, look.grainColor.x,
                  look.grainColor.y, look.grainColor.z);
    return key + look.insideGroup;
}

bool drawsBodies(const Frame& frame, const Look& look) {
    return (look.pieces && !frame.rigid.empty()) || (look.cloth && !frame.cloth.empty()) ||
           (look.grains && !frame.grains.empty());
}

std::shared_ptr<const PreparedBodies> prepareBodies(const Frame& frame, const Look& look) {
    auto out = std::make_shared<PreparedBodies>();
    if (const std::shared_ptr<const Geometry> bodies = drawsBodies(frame, look) ? drawnBodies(frame, look) : nullptr) {
        out->display = displayOf(*bodies);
    }
    return out;
}

BodiesPreparer::BodiesPreparer() { thread_ = std::thread([this] { loop(); }); }

BodiesPreparer::~BodiesPreparer() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void BodiesPreparer::want(std::vector<Want> wanted) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        wanted_ = std::move(wanted);
        wantedKeys_.clear();
        for (const Want& w : wanted_) wantedKeys_.push_back(bodiesKey(w.look));
    }
    wake_.notify_all();
}

const BodiesPreparer::Made* BodiesPreparer::madeLocked(const std::shared_ptr<const Frame>& frame, const std::string& key) const {
    for (const Made& m : made_) {
        if (m.key == key && m.frame.lock() == frame) return &m;
    }
    return nullptr;
}

int BodiesPreparer::nextLocked() const {
    for (size_t i = 0; i < wanted_.size(); ++i) {
        if (wanted_[i].frame && !madeLocked(wanted_[i].frame, wantedKeys_[i])) return static_cast<int>(i);
    }
    return -1;
}

bool BodiesPreparer::wantedLocked(const Made& made) const {
    const std::shared_ptr<const Frame> frame = made.frame.lock();
    for (size_t i = 0; i < wanted_.size(); ++i) {
        if (frame && wanted_[i].frame == frame && wantedKeys_[i] == made.key) return true;
    }
    return false;
}

std::shared_ptr<const PreparedBodies> BodiesPreparer::find(const std::shared_ptr<const Frame>& frame, const Look& look) const {
    if (!frame) return nullptr;
    const std::string key = bodiesKey(look);
    std::lock_guard<std::mutex> lock(mu_);
    const Made* made = madeLocked(frame, key);
    return made ? made->bodies : nullptr;
}

void BodiesPreparer::wait() {
    std::unique_lock<std::mutex> lock(mu_);
    done_.wait(lock, [&] { return stop_ || (!working_ && nextLocked() < 0); });
}

void BodiesPreparer::loop() {
    for (;;) {
        Want next;
        std::string key;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || nextLocked() >= 0; });
            if (stop_) return;
            const auto i = static_cast<size_t>(nextLocked());
            next = wanted_[i];
            key = wantedKeys_[i];
            working_ = true;
        }
        std::shared_ptr<const PreparedBodies> bodies = prepareBodies(*next.frame, next.look);
        {
            std::lock_guard<std::mutex> lock(mu_);
            made_.push_back({next.frame, std::move(key), std::move(bodies)});
            // Past kKept, the oldest no longer wanted go -- the wanted stay, or
            // they would be made again and again.
            for (auto it = made_.begin(); made_.size() > kKept && it != made_.end();) {
                it = wantedLocked(*it) ? std::next(it) : made_.erase(it);
            }
            working_ = false;
        }
        done_.notify_all();
    }
}

}  // namespace pg::sim
