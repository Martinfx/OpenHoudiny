#include "pg/sim/Prepared.h"

#include "pg/core/Instances.h"
#include "pg/core/Lod.h"
#include "pg/core/Parallel.h"
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

GeometryPreparerThread::GeometryPreparerThread() { thread_ = std::thread([this] { loop(); }); }

GeometryPreparerThread::~GeometryPreparerThread() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void GeometryPreparerThread::want(std::vector<GeometryPtr> wanted) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        wanted_ = std::move(wanted);
    }
    wake_.notify_all();
}

std::shared_ptr<const PreparedGeometry> GeometryPreparerThread::madeLocked(const GeometryPtr& geometry) const {
    for (const auto& made : made_) {
        if (made->geometry == geometry) return made;
    }
    return nullptr;
}

int GeometryPreparerThread::nextLocked() const {
    for (size_t i = 0; i < wanted_.size(); ++i) {
        if (wanted_[i] && !madeLocked(wanted_[i])) return static_cast<int>(i);
    }
    return -1;
}

std::shared_ptr<const PreparedGeometry> GeometryPreparerThread::find(const GeometryPtr& geometry) const {
    if (!geometry) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    return madeLocked(geometry);
}

void GeometryPreparerThread::wait() {
    std::unique_lock<std::mutex> lock(mu_);
    done_.wait(lock, [&] { return stop_ || (!working_ && nextLocked() < 0); });
}

void GeometryPreparerThread::loop() {
    for (;;) {
        GeometryPtr next;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || nextLocked() >= 0; });
            if (stop_) return;
            next = wanted_[static_cast<size_t>(nextLocked())];
            working_ = true;
        }
        // Each on its own: what one made is not the start of the next.
        std::shared_ptr<const PreparedGeometry> made = GeometryPreparer().prepare(next);
        {
            std::lock_guard<std::mutex> lock(mu_);
            made_.push_back(std::move(made));
            for (auto it = made_.begin(); made_.size() > kKept && it != made_.end();) {
                const bool wanted = std::find(wanted_.begin(), wanted_.end(), (*it)->geometry) != wanted_.end();
                it = wanted ? std::next(it) : made_.erase(it);
            }
            working_ = false;
        }
        done_.notify_all();
    }
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

bool hasVolumes(const Frame& frame, unsigned layers) {
    return ((layers & kGasVolume) && !frame.fields.empty()) || ((layers & kWaterVolume) && !frame.water.empty()) ||
           ((layers & kRainVolume) && !frame.rain.empty());
}

std::shared_ptr<const PreparedVolumes> prepareVolumes(const Frame& frame, size_t texels, unsigned layers) {
    auto out = std::make_shared<PreparedVolumes>();
    out->texels = texels;
    out->layers = layers;
    // The coarsest grid needed to fit: `domain` 2, 4 or 8 times as coarse,
    // while that divides its cells -- the factor.
    auto coarse = [&](const Domain& domain, Domain& grid, auto tooBig) {
        grid = domain;
        int factor = 1;
        while (tooBig(grid) && factor < 8 && domain.cells[0] % (2 * factor) == 0 && domain.cells[1] % (2 * factor) == 0 &&
               domain.cells[2] % (2 * factor) == 0) {
            factor *= 2;
            for (int a = 0; a < 3; ++a) grid.cells[a] = domain.cells[a] / factor;
            grid.voxel = domain.voxel * static_cast<float>(factor);
        }
        return factor;
    };

    // The gas: smoke, temperature, flame -- and the steam, 0 without any.
    if ((layers & kGasVolume) && !frame.fields.empty()) {
        Domain grid;
        const int factor = coarse(frame.domain, grid, [&](const Domain& g) { return g.cellCount() > texels; });
        std::vector<uint16_t> scratch, steamScratch;
        const std::vector<uint16_t>* three = &scratch;
        const std::vector<uint16_t>* vapour = &steamScratch;
        if (factor > 1) {
            frame.coarseFields(factor, scratch);
            frame.coarseSteam(factor, steamScratch);
        } else {
            three = &frame.denseFields(scratch);
            vapour = &frame.denseSteam(steamScratch);
        }
        const size_t cells = grid.cellCount();
        if (three->size() == 3 * cells && grid.cells[0] > 0) {
            const bool steamy = vapour->size() == cells;
            out->gasGrid = grid;
            out->gas.assign(4 * cells, 0);
            pg::parallelFor(cells, 65536, [&](size_t begin, size_t end) {
                for (size_t c = begin; c < end; ++c) {
                    out->gas[4 * c] = (*three)[3 * c];
                    out->gas[4 * c + 1] = (*three)[3 * c + 1];
                    out->gas[4 * c + 2] = (*three)[3 * c + 2];
                    if (steamy) out->gas[4 * c + 3] = (*vapour)[c];
                }
            });
        }
    }

    // The water: at most `texels` cells, 2048 a side.
    const WaterFrame& water = frame.water;
    if ((layers & kWaterVolume) && !water.empty() && water.fits() && water.domain.cells[0] > 0) {
        const int factor = coarse(water.domain, out->waterGrid, [&](const Domain& g) {
            return g.cellCount() > texels || std::max({g.cells[0], g.cells[1], g.cells[2]}) > 2048;
        });
        if (factor > 1) {
            water.coarseCells(factor, out->water);
        } else {
            std::vector<uint8_t> scratch;
            const std::vector<uint8_t>& cells = water.denseCells(scratch);
            if (&cells == &scratch) out->water = std::move(scratch);
            else out->water = cells;
        }
    }

    // The rain: a streak for each drop and droplet; the floor wet where the
    // drops are.
    const RainFrame& rain = frame.rain;
    if ((layers & kRainVolume) && (!rain.drops.empty() || !rain.droplets.empty())) {
        out->wetMin[0] = out->wetMin[1] = 1e30f;
        out->wetMax[0] = out->wetMax[1] = -1e30f;
        for (size_t i = 0; i + 5 < rain.drops.size(); i += 6) {
            out->wetMin[0] = std::min(out->wetMin[0], rain.drops[i]);
            out->wetMax[0] = std::max(out->wetMax[0], rain.drops[i]);
            out->wetMin[1] = std::min(out->wetMin[1], rain.drops[i + 2]);
            out->wetMax[1] = std::max(out->wetMax[1], rain.drops[i + 2]);
        }
        // Six corners a streak: two triangles from its tail to its head.
        static const float corners[6][2] = {{0, -1}, {1, -1}, {1, 1}, {0, -1}, {1, 1}, {0, 1}};
        std::vector<float>& v = out->rain;
        v.reserve((rain.drops.size() + rain.droplets.size()) * 9);
        for (int kind = 0; kind < 2; ++kind) {
            const std::vector<float>& from = kind == 0 ? rain.drops : rain.droplets;
            for (size_t i = 0; i + 5 < from.size(); i += 6) {
                for (const auto& c : corners) {
                    v.insert(v.end(), {from[i], from[i + 1], from[i + 2], from[i + 3], from[i + 4], from[i + 5], c[0], c[1],
                                       static_cast<float>(kind)});
                }
            }
        }
    }
    return out;
}

FrameWorker::FrameWorker(size_t kept) : kept_(kept) { thread_ = std::thread([this] { loop(); }); }

FrameWorker::~FrameWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void FrameWorker::want(std::vector<Want> wanted) {
    std::deque<Made> gone;
    {
        std::lock_guard<std::mutex> lock(mu_);
        wanted_ = std::move(wanted);
        dropLocked(gone);
    }
    wake_.notify_all();
}

void FrameWorker::dropLocked(std::deque<Made>& gone) {
    // The wanted stay, or they would be made again and again.
    for (auto it = made_.begin(); made_.size() > kept_ && it != made_.end();) {
        if (wantedLocked(*it)) {
            ++it;
        } else {
            gone.push_back(std::move(*it));
            it = made_.erase(it);
        }
    }
}

const FrameWorker::Made* FrameWorker::madeLocked(const std::shared_ptr<const Frame>& frame, const std::string& key) const {
    for (const Made& m : made_) {
        if (m.key == key && m.frame.lock() == frame) return &m;
    }
    return nullptr;
}

int FrameWorker::nextLocked() const {
    for (size_t i = 0; i < wanted_.size(); ++i) {
        if (wanted_[i].frame && !madeLocked(wanted_[i].frame, wanted_[i].key)) return static_cast<int>(i);
    }
    return -1;
}

bool FrameWorker::wantedLocked(const Made& made) const {
    const std::shared_ptr<const Frame> frame = made.frame.lock();
    for (const Want& w : wanted_) {
        if (frame && w.frame == frame && w.key == made.key) return true;
    }
    return false;
}

std::shared_ptr<const void> FrameWorker::find(const std::shared_ptr<const Frame>& frame, const std::string& key) const {
    if (!frame) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    const Made* made = madeLocked(frame, key);
    return made ? made->thing : nullptr;
}

void FrameWorker::wait() {
    std::unique_lock<std::mutex> lock(mu_);
    done_.wait(lock, [&] { return stop_ || (!working_ && nextLocked() < 0); });
}

void FrameWorker::loop() {
    for (;;) {
        Want next;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || nextLocked() >= 0; });
            if (stop_) return;
            next = wanted_[static_cast<size_t>(nextLocked())];
            working_ = true;
        }
        std::shared_ptr<const void> thing = next.make();
        std::deque<Made> gone;
        {
            std::lock_guard<std::mutex> lock(mu_);
            made_.push_back({next.frame, std::move(next.key), std::move(thing)});
            dropLocked(gone);
            working_ = false;
        }
        done_.notify_all();
    }
}

void BodiesPreparer::want(std::vector<Want> wanted) {
    std::vector<FrameWorker::Want> work;
    work.reserve(wanted.size());
    for (Want& w : wanted) {
        std::string key = bodiesKey(w.look);
        auto make = [frame = w.frame, look = std::move(w.look)]() -> std::shared_ptr<const void> {
            return prepareBodies(*frame, look);
        };
        work.push_back({std::move(w.frame), std::move(key), std::move(make)});
    }
    worker_.want(std::move(work));
}

std::shared_ptr<const PreparedBodies> BodiesPreparer::find(const std::shared_ptr<const Frame>& frame, const Look& look) const {
    return std::static_pointer_cast<const PreparedBodies>(worker_.find(frame, bodiesKey(look)));
}

namespace {
std::string volumesKey(size_t texels, unsigned layers) { return std::to_string(texels) + " " + std::to_string(layers); }
}  // namespace

void VolumesPreparer::want(std::vector<Want> wanted) {
    std::vector<FrameWorker::Want> work;
    work.reserve(wanted.size());
    for (const Want& w : wanted) {
        auto make = [frame = w.frame, texels = w.texels, layers = w.layers]() -> std::shared_ptr<const void> {
            return prepareVolumes(*frame, texels, layers);
        };
        work.push_back({w.frame, volumesKey(w.texels, w.layers), std::move(make)});
    }
    worker_.want(std::move(work));
}

std::shared_ptr<const PreparedVolumes> VolumesPreparer::find(const std::shared_ptr<const Frame>& frame, size_t texels,
                                                             unsigned layers) const {
    return std::static_pointer_cast<const PreparedVolumes>(worker_.find(frame, volumesKey(texels, layers)));
}

}  // namespace pg::sim
