#include "pg/render/Denoise.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <vector>

#ifdef PG_HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace pg::render {

#ifdef PG_HAVE_OIDN
namespace {

/// OIDN's CPU device -- made at first need, its weights loaded, kept while
/// the program runs -- and the filters of the last size, kept with the
/// pictures they read and write: a filter is committed once for a size,
/// not for every picture.
struct Oidn {
    oidn::DeviceRef device;
    bool ok = false;
    std::mutex mutex;  // one picture at a time
    int width = 0, height = 0;
    oidn::FilterRef albedo, normal, color;
    std::vector<float> a, n, in, out;  // the albedo and the normal cleaned in place; the light in and out
};

Oidn& oidnState() {
    static Oidn* const made = [] {
        auto* o = new Oidn();
        o->device = oidn::newDevice(oidn::DeviceType::CPU);
        o->device.commit();
        const char* why = nullptr;
        o->ok = o->device.getError(why) == oidn::Error::None;
        if (!o->ok) {
            std::fprintf(stderr, "Open Image Denoise: no device (%s): our own filter takes the noise out\n", why ? why : "");
        }
        return o;
    }();
    return *made;
}

}  // namespace
#endif

bool oidnAvailable() {
#ifdef PG_HAVE_OIDN
    return oidnState().ok;
#else
    return false;
#endif
}

std::string oidnVersion() {
#ifdef PG_HAVE_OIDN
    return std::string("Open Image Denoise ") + OIDN_VERSION_STRING;
#else
    return {};
#endif
}

Denoiser defaultDenoiser() {
    static const Denoiser denoiser = [] {
        const char* asked = std::getenv("PG_DENOISER");
        if (asked && std::string_view(asked) == "own") return Denoiser::Own;
        return oidnAvailable() ? Denoiser::Oidn : Denoiser::Own;
    }();
    return denoiser;
}

std::string denoiserName(Denoiser denoiser) {
    return denoiser == Denoiser::Oidn && oidnAvailable() ? oidnVersion() : std::string("own filter");
}

bool oidnDenoise(const Image& beauty, const Image& albedo, const Image& normal, Image& out, std::string& error) {
#ifdef PG_HAVE_OIDN
    const int w = beauty.width, h = beauty.height;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (n == 0 || beauty.channels != 3 || beauty.pixels.size() < 3 * n || albedo.pixels.size() < 3 * n ||
        normal.pixels.size() < 3 * n) {
        error = "the light, the albedo and the normal are not pictures of one size";
        return false;
    }
    Oidn& o = oidnState();
    if (!o.ok) {
        error = "no Open Image Denoise device";
        return false;
    }
    std::lock_guard<std::mutex> lock(o.mutex);
    if (o.width != w || o.height != h) {
        // Filters for this size: the albedo and the normal cleaned of their
        // own noise, in place, then the light guided by them.
        o.a.assign(3 * n, 0.0f);
        o.n.assign(3 * n, 0.0f);
        o.in.assign(3 * n, 0.0f);
        o.out.assign(3 * n, 0.0f);
        const size_t sw = static_cast<size_t>(w), sh = static_cast<size_t>(h);
        o.albedo = o.device.newFilter("RT");
        o.albedo.setImage("albedo", o.a.data(), oidn::Format::Float3, sw, sh);
        o.albedo.setImage("output", o.a.data(), oidn::Format::Float3, sw, sh);
        o.albedo.set("quality", oidn::Quality::High);
        o.albedo.commit();
        o.normal = o.device.newFilter("RT");
        o.normal.setImage("normal", o.n.data(), oidn::Format::Float3, sw, sh);
        o.normal.setImage("output", o.n.data(), oidn::Format::Float3, sw, sh);
        o.normal.set("quality", oidn::Quality::High);
        o.normal.commit();
        o.color = o.device.newFilter("RT");
        o.color.setImage("color", o.in.data(), oidn::Format::Float3, sw, sh);
        o.color.setImage("albedo", o.a.data(), oidn::Format::Float3, sw, sh);
        o.color.setImage("normal", o.n.data(), oidn::Format::Float3, sw, sh);
        o.color.setImage("output", o.out.data(), oidn::Format::Float3, sw, sh);
        o.color.set("hdr", true);        // linear light, brighter than 1 where it is
        o.color.set("cleanAux", true);   // the guides cleaned just before
        o.color.set("quality", oidn::Quality::High);
        o.color.commit();
        o.width = w;
        o.height = h;
    }
    std::copy(albedo.pixels.begin(), albedo.pixels.begin() + static_cast<std::ptrdiff_t>(3 * n), o.a.begin());
    std::copy(normal.pixels.begin(), normal.pixels.begin() + static_cast<std::ptrdiff_t>(3 * n), o.n.begin());
    std::copy(beauty.pixels.begin(), beauty.pixels.begin() + static_cast<std::ptrdiff_t>(3 * n), o.in.begin());
    o.albedo.execute();
    o.normal.execute();
    o.color.execute();
    const char* why = nullptr;
    if (o.device.getError(why) != oidn::Error::None) {
        error = std::string("Open Image Denoise: ") + (why ? why : "failed");
        o.width = o.height = 0;  // made again next time
        return false;
    }
    out.width = w;
    out.height = h;
    out.channels = 3;
    out.pixels = o.out;
    return true;
#else
    (void)beauty, (void)albedo, (void)normal, (void)out;
    error = "built without Open Image Denoise";
    return false;
#endif
}

}  // namespace pg::render
