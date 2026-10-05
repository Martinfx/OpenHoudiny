#include "pg/render/Textures.h"

#include "pg/io/MaterialX.h"
#include "pg/io/Picture.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Scene.h"
#include "pg/usd/Shade.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

namespace pg::render {
namespace {

namespace fs = std::filesystem;

/// What a picture of a set is, as its name says.
enum class Role { None, Color, Height, Roughness, Normal, Opacity, Other };

Role roleOf(const std::string& token) {
    static const std::map<std::string, Role> roles = {
        {"diff", Role::Color},         {"diffuse", Role::Color},     {"color", Role::Color},
        {"colour", Role::Color},       {"col", Role::Color},         {"albedo", Role::Color},
        {"basecolor", Role::Color},    {"base", Role::Color},        {"co", Role::Color},
        {"disp", Role::Height},        {"displacement", Role::Height}, {"height", Role::Height},
        {"bump", Role::Height},        {"di", Role::Height},         {"dis", Role::Height},
        {"rough", Role::Roughness},    {"roughness", Role::Roughness},
        {"nor", Role::Normal},         {"normal", Role::Normal},     {"nrm", Role::Normal},
        {"normalgl", Role::Normal},    {"normaldx", Role::Normal},   {"norm", Role::Normal},
        {"gl", Role::Other},           {"dx", Role::Other},          {"opengl", Role::Other},
        {"directx", Role::Other},      {"ao", Role::Other},          {"ambientocclusion", Role::Other},
        {"occlusion", Role::Other},    {"arm", Role::Other},         {"metal", Role::Other},
        {"metallic", Role::Other},     {"metalness", Role::Other},   {"spec", Role::Other},
        {"specular", Role::Other},     {"opacity", Role::Opacity},   {"alpha", Role::Opacity},
        {"mask", Role::Opacity},         {"gloss", Role::Other},       {"glossiness", Role::Other},
    };
    const auto it = roles.find(token);
    return it == roles.end() ? Role::None : it->second;
}

/// A picture's name taken apart: what it is, and the rest of the name -- the
/// same for every picture of one set.
struct Name {
    Role role = Role::None;
    std::string key;
    bool directX = false;  // a normal map of DirectX's: _nor_dx_, _NormalDX
};
Name nameOf(const fs::path& file) {
    std::string stem = file.stem().string();
    for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<std::string> tokens;
    std::string token;
    for (const char c : stem + "_") {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            token += c;
        } else if (!token.empty()) {
            tokens.push_back(token);
            token.clear();
        }
    }
    Name name;
    for (const std::string& t : tokens) {
        const Role r = roleOf(t);
        if (r != Role::None) {
            if (name.role == Role::None || name.role == Role::Other) name.role = r;
            if (t == "dx" || t == "normaldx" || t == "directx") name.directX = true;
            continue;
        }
        name.key += t + "_";
    }
    return name;
}

bool isPicture(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".exr";
}

/// The average colour of a picture, linear: every few pixels of it, each
/// as much as its alpha says it is there. `cut`, if given: whether it has
/// pixels not all there -- an alpha of its own.
Vec3 meanOf(const std::string& path, bool* cut = nullptr) {
    if (cut) *cut = false;
    io::Picture picture;
    std::string error;
    if (!io::readPicture(path, picture, error) || picture.empty()) return Vec3(0.5f, 0.5f, 0.5f);
    const int step = std::max(1, std::max(picture.width, picture.height) / 256);
    double sum[3] = {0.0, 0.0, 0.0}, n = 0.0;
    for (int y = 0; y < picture.height; y += step) {
        for (int x = 0; x < picture.width; x += step) {
            const float* p = picture.pixel(x, y);
            for (int c = 0; c < 3; ++c) sum[c] += p[3] * (picture.linear ? p[c] : io::srgbToLinear(p[c]));
            n += p[3];
            if (cut && p[3] < 0.5f) *cut = true;
        }
    }
    if (n <= 0.0) return Vec3(0.5f, 0.5f, 0.5f);
    return Vec3(static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n));
}

/// A folder of ours: color, height, roughness and texture.txt -- the
/// pictures of another folder, where its texture.txt says `pictures`.
TextureSet readFolder(const fs::path& folder) {
    TextureSet set;
    fs::path pictures = folder;
    bool hasMean = false;
    std::ifstream in(folder / "texture.txt");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string key;
        if (!(words >> key) || key[0] == '#') continue;
        if (key == "pictures") {
            std::string other;
            if (words >> other) pictures = (folder / other).lexically_normal();
        }
        if (key == "size") words >> set.size;
        if (key == "depth") words >> set.depth;
        if (key == "mean") hasMean = static_cast<bool>(words >> set.mean.x >> set.mean.y >> set.mean.z);
        if (key == "tint") {
            int tint = 1;
            if (words >> tint) set.tint = tint != 0;
        }
        if (key == "projection") {
            std::string how;
            if (words >> how) {
                set.alongFace = how == "face";
                set.onlyByUv = how == "uv";
            }
        }
        if (key == "normal") {
            std::string how;
            if (words >> how) set.normalDirectX = how == "dx" || how == "directx";
        }
        if (key == "alpha") {
            int alpha = 0;
            if (words >> alpha) set.alphaChannel = alpha != 0;
        }
    }
    auto picture = [&](const char* name) {
        for (const char* ext : {".jpg", ".png", ".jpeg", ".exr"}) {
            const fs::path p = pictures / (std::string(name) + ext);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) return p.string();
        }
        return std::string();
    };
    set.color = picture("color");
    set.height = picture("height");
    set.roughness = picture("roughness");
    set.normal = picture("normal");
    if (set.alphaChannel) {
        set.alpha = set.color;
    } else {
        set.alpha = picture("opacity");
    }
    set.size = std::max(set.size, 1e-3f);
    set.depth = std::max(set.depth, 0.0f);
    if (set.valid() && !hasMean) set.mean = meanOf(set.color);
    return set;
}

/// A set of someone else's, from one picture of it: those beside it whose
/// names differ only in what they are.
TextureSet readSiblings(const fs::path& file) {
    TextureSet set;
    const Name name = nameOf(file);
    std::error_code ec;
    if (name.role == Role::None || name.role == Role::Color) set.color = file.string();
    // In the order the folder lists them: the same set whatever it is.
    std::vector<fs::path> beside;
    for (const auto& entry : fs::directory_iterator(file.parent_path(), ec)) {
        if (entry.is_regular_file(ec) && isPicture(entry.path())) beside.push_back(entry.path());
    }
    std::sort(beside.begin(), beside.end());
    for (const fs::path& path : beside) {
        const Name other = nameOf(path);
        if (other.key != name.key) continue;
        if (other.role == Role::Color && set.color.empty()) set.color = path.string();
        if (other.role == Role::Height && set.height.empty()) set.height = path.string();
        if (other.role == Role::Roughness && set.roughness.empty()) set.roughness = path.string();
        if (other.role == Role::Opacity && set.alpha.empty()) set.alpha = path.string();
        // A normal map: OpenGL's rather than DirectX's, where there are both.
        if (other.role == Role::Normal && (set.normal.empty() || (set.normalDirectX && !other.directX))) {
            set.normal = path.string();
            set.normalDirectX = other.directX;
        }
    }
    if (!set.valid()) return set;
    set.size = 2.0f;
    set.depth = 0.01f * set.size;
    // A colour whose own alpha leaves some of it out: that cuts it out,
    // where there is no picture to say so.
    bool cut = false;
    set.mean = meanOf(set.color, &cut);
    if (set.alpha.empty() && cut) {
        set.alpha = set.color;
        set.alphaChannel = true;
    }
    set.tint = false;  // a photograph of someone else's: as it is
    return set;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The texture set of a material's surface (mtlx::surfaceOf): the pictures
/// behind it, each file given its path by `resolved`. Tinted by Cd where its
/// colour is tinted by a geometric property -- its mean what the material
/// evens the picture by; as many metres a picture as it lays them on from
/// three sides by, else 2.
TextureSet setOfSurface(const io::mtlx::Surface& surface, const std::function<std::string(const std::string&)>& resolved) {
    if (!surface.found || surface.color.empty()) return {};
    TextureSet set;
    set.color = resolved(surface.color);
    set.normal = resolved(surface.normal);
    set.normalDirectX = surface.normalDirectX;
    set.roughness = resolved(surface.roughnessFile);
    set.height = resolved(surface.height);
    set.alpha = resolved(surface.opacity);
    set.alphaChannel = !set.alpha.empty() && surface.opacityFromAlpha;
    set.size = surface.size > 0.0f ? surface.size : 2.0f;
    set.depth = surface.depth > 0.0f ? surface.depth : 0.01f * set.size;
    // Tinted, the picture over its mean -- as the material evens it, else
    // as it is.
    set.tint = surface.tinted;
    const Vec3 k = surface.scale;
    set.mean = set.tint && k.x > 0.0f && k.y > 0.0f && k.z > 0.0f ? Vec3(1.0f) / k : meanOf(set.color);
    return set;
}

/// A material of a MaterialX document (MaterialGraph.h writes them; so do
/// Poly Haven, ambientCG, Houdini...) -- the one named `name`, else its
/// first --, its files from the document's folder.
TextureSet readMaterialX(const fs::path& file, const std::string& name) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::vector<io::mtlx::Node> nodes;
    std::string error;
    if (!io::mtlx::parse(text.str(), nodes, error)) return {};
    return setOfSurface(io::mtlx::surfaceOf(nodes, name), [&](const std::string& f) {
        if (f.empty()) return std::string();
        const fs::path p(f);
        return (p.is_absolute() ? p : (file.parent_path() / p).lexically_normal()).string();
    });
}

/// A Material of a USD stage -- its MaterialX or UsdPreviewSurface network
/// (usd/Shade.h) --, its files as USD resolves them, its lengths in metres.
TextureSet readUsdMaterial(const std::string& file, const std::string& path) {
    std::string error;
    const auto stage = usd::Stage::openCached(file, error);
    const usd::Stage::Prim* prim = stage ? stage->find(path) : nullptr;
    if (!prim || prim->type != "Material") return {};
    io::mtlx::Surface surface = usd::materialSurface(*stage, *prim, 0.0);
    // Its lengths in the stage's units: in metres, as the renderers take them.
    const float metres = static_cast<float>(stage->metersPerUnit());
    if (surface.size > 0.0f) surface.size *= metres;
    if (surface.depth > 0.0f) surface.depth *= metres;
    return setOfSurface(surface, [](const std::string& f) { return f; });
}

bool isUsd(const fs::path& file) {
    const std::string ext = lower(file.extension().string());
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

/// Sets as they were read, by what named them.
std::mutex setsMutex;
std::map<std::string, TextureSet> sets;

}  // namespace

TextureSet textureSet(const std::string& where) {
    if (where.empty()) return {};
    {
        std::lock_guard<std::mutex> lock(setsMutex);
        if (const auto it = sets.find(where); it != sets.end()) return it->second;
    }
    TextureSet set;
    std::error_code ec;
    const fs::path path(where);
    // A MaterialX document, or one material of it: materials.mtlx#bark; a
    // Material of a USD stage: shot.usda#/World/Materials/bark.
    const size_t hash = where.find('#');
    const bool named = hash != std::string::npos && lower(fs::path(where.substr(0, hash)).extension().string()) == ".mtlx";
    if (hash != std::string::npos && isUsd(fs::path(where.substr(0, hash)))) {
        set = readUsdMaterial(where.substr(0, hash), where.substr(hash + 1));
    } else if (named || lower(path.extension().string()) == ".mtlx") {
        const fs::path document = named ? fs::path(where.substr(0, hash)) : path;
        if (fs::is_regular_file(document, ec)) set = readMaterialX(document, named ? where.substr(hash + 1) : std::string());
    } else if (fs::is_directory(path, ec)) {
        set = fs::is_regular_file(path / "texture.txt", ec) ? readFolder(path) : TextureSet();
        if (!set.valid()) {
            // Someone else's pictures in a folder of their own: the colour's.
            for (const auto& entry : fs::directory_iterator(path, ec)) {
                if (entry.is_regular_file(ec) && isPicture(entry.path()) && nameOf(entry.path()).role == Role::Color) {
                    set = readSiblings(entry.path());
                    break;
                }
            }
        }
        if (!set.valid()) {
            // ... or a MaterialX document of them: the first, by name.
            std::vector<fs::path> documents;
            for (const auto& entry : fs::directory_iterator(path, ec)) {
                if (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".mtlx") documents.push_back(entry.path());
            }
            std::sort(documents.begin(), documents.end());
            if (!documents.empty()) set = readMaterialX(documents.front(), {});
        }
    } else if (fs::is_regular_file(path, ec) && isPicture(path)) {
        // One of ours by its folder, else someone else's.
        set = path.stem() == "color" && fs::is_regular_file(path.parent_path() / "texture.txt", ec)
                  ? readFolder(path.parent_path())
                  : readSiblings(path);
    }
    std::lock_guard<std::mutex> lock(setsMutex);
    sets[where] = set;
    return set;
}

std::string textureLibrary() {
    std::error_code ec;
    if (const char* env = std::getenv("PG_TEXTURES"); env && *env && fs::is_directory(env, ec)) return env;
#ifdef PG_TEXTURES_DIR
    if (fs::is_directory(PG_TEXTURES_DIR, ec)) return PG_TEXTURES_DIR;
#endif
    return {};
}

TextureSet presetTextureSet(const std::string& library, MaterialPreset preset) {
    if (library.empty() || preset == MaterialPreset::None) return {};
    const fs::path folder = fs::path(library) / std::string(materialName(preset));
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return {};
    return textureSet(folder.string());
}

TextureSet textureOf(const Material& m, const Settings& settings) {
    if (!settings.textures || m.kind != Material::Kind::Surface) return {};
    TextureSet set;
    if (!m.texture.empty()) {
        set = textureSet(m.texture);
    } else if (m.preset != MaterialPreset::None) {
        set = presetTextureSet(settings.textureFolder.empty() ? textureLibrary() : settings.textureFolder, m.preset);
    }
    if (set.onlyByUv && !m.byUv) return {};
    if (set.valid() && m.textureSize > 0.0f) {
        set.depth *= m.textureSize / set.size;
        set.size = m.textureSize;
    }
    if (m.textureTint >= 0) set.tint = m.textureTint > 0;
    return set;
}

Vec3 TexturePicture::at(float u, float v) const {
    if (width <= 0 || height <= 0) return Vec3(1.0f, 1.0f, 1.0f);
    // Rows from the top; v up the picture, as Cycles has it.
    const float x = (u - std::floor(u)) * static_cast<float>(width) - 0.5f;
    const float y = (1.0f - (v - std::floor(v))) * static_cast<float>(height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto wrap = [](int i, int n) { return ((i % n) + n) % n; };
    const int x0 = wrap(static_cast<int>(fx), width), x1 = wrap(static_cast<int>(fx) + 1, width);
    const int y0 = wrap(static_cast<int>(fy), height), y1 = wrap(static_cast<int>(fy) + 1, height);
    auto p = [&](int xx, int yy) { return pixels[static_cast<size_t>(yy) * static_cast<size_t>(width) + static_cast<size_t>(xx)]; };
    return (p(x0, y0) * (1.0f - tx) + p(x1, y0) * tx) * (1.0f - ty) + (p(x0, y1) * (1.0f - tx) + p(x1, y1) * tx) * ty;
}

Vec3 TexturePicture::onSurface(const Vec3& rest, const Vec3& face) const {
    const float k = 1.0f / std::max(size, 1e-3f);
    Vec3 c(0.0f);
    // Along the face, where it leans more than some 15 degrees from lying:
    // across it level -- its normal turned a quarter round Y -- and up it,
    // across both (as Cycles' alongFace).
    float along = 0.0f;
    if (alongFace) {
        const float lean = std::sqrt(face.x * face.x + face.z * face.z);
        const float t = std::clamp((lean - 0.24f) / 0.04f, 0.0f, 1.0f);
        along = t * t * (3.0f - 2.0f * t);
        if (along > 0.0f) {
            const Vec3 level = Vec3(face.z, 0.0f, -face.x) / std::max(lean, 1e-4f);
            const Vec3 up = glm::cross(face, level);
            c = at(glm::dot(rest, level) * k, glm::dot(rest, up) * k) * along;
            if (along >= 1.0f) return c;
        }
    }
    // As much from each side as the surface faces it, to the fourth power;
    // each side's picture moved, so that the three do not line up.
    Vec3 w(face.x * face.x, face.y * face.y, face.z * face.z);
    w = w * w;
    const float sum = w.x + w.y + w.z;
    if (sum <= 0.0f) return Vec3(1.0f, 1.0f, 1.0f);
    w = w * ((1.0f - along) / sum);
    if (w.x > 1e-3f) c = c + at(rest.z * k + 0.31f, rest.y * k + 0.17f) * w.x;
    if (w.y > 1e-3f) c = c + at(rest.x * k + 0.53f, rest.z * k + 0.71f) * w.y;
    if (w.z > 1e-3f) c = c + at(rest.x * k, rest.y * k) * w.z;
    return c;
}

Vec3 TexturePicture::shade(const Vec3& color, const Vec3& copyTint, const Vec3& rest, const Vec3& face, const Vec2* uv) const {
    const Vec3 picture = uv ? at(uv->x, uv->y) : onSurface(rest, face);
    const Vec3 c = tint ? color * picture / glm::max(mean, Vec3(1e-4f)) : picture * copyTint;
    return glm::min(c, Vec3(0.95f));
}

namespace {

/// The picture of `file` -- light, or (`values`) as it is -- no more than
/// 1024 across; null when it cannot be read.
std::shared_ptr<TexturePicture> loadPicture(const std::string& file, bool values, int coverage = -1) {
    io::Picture picture;
    std::string error;
    std::shared_ptr<TexturePicture> out;
    if (io::readPicture(file, picture, error) && !picture.empty()) {
        // No more than 1024 across: halved, four pixels averaged, until so.
        int w = picture.width, h = picture.height;
        std::vector<Vec3> px(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* p = picture.pixel(x, y);
                if (coverage >= 0) {
                    // How much there is: the alpha (3), or the grey (0).
                    const float a = coverage == 3 ? p[3] : (p[0] + p[1] + p[2]) * (1.0f / 3.0f);
                    px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = Vec3(a);
                    continue;
                }
                Vec3 c = picture.linear || values
                             ? Vec3(p[0], p[1], p[2])
                             : Vec3(io::srgbToLinear(p[0]), io::srgbToLinear(p[1]), io::srgbToLinear(p[2]));
                px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
            }
        }
        while (w > 1024 || h > 1024) {
            const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
            std::vector<Vec3> half(static_cast<size_t>(nw) * static_cast<size_t>(nh));
            for (int y = 0; y < nh; ++y) {
                for (int x = 0; x < nw; ++x) {
                    Vec3 s(0.0f);
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(2 * x + dx, w - 1), sy = std::min(2 * y + dy, h - 1);
                            s = s + px[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
                        }
                    }
                    half[static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)] = s * 0.25f;
                }
            }
            px = std::move(half);
            w = nw;
            h = nh;
        }
        out = std::make_shared<TexturePicture>();
        out->width = w;
        out->height = h;
        out->pixels = std::move(px);
    }
    return out;
}

}  // namespace

std::shared_ptr<const TexturePicture> texturePicture(const TextureSet& set) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TexturePicture>> pictures;
    if (!set.valid()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    std::ostringstream key;
    key << set.color << '|' << set.size << '|' << set.tint << '|' << set.alongFace << '|' << set.mean.x << ' '
        << set.mean.y << ' ' << set.mean.z;
    if (const auto it = pictures.find(key.str()); it != pictures.end()) return it->second;
    std::shared_ptr<TexturePicture> out = loadPicture(set.color, false);
    if (out) {
        out->size = set.size;
        out->mean = set.mean;
        out->tint = set.tint;
        out->alongFace = set.alongFace;
    }
    pictures[key.str()] = out;
    return out;
}

std::shared_ptr<const TexturePicture> normalPicture(const TextureSet& set) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TexturePicture>> pictures;
    if (set.normal.empty()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = pictures.find(set.normal); it != pictures.end()) return it->second;
    std::shared_ptr<const TexturePicture> out = loadPicture(set.normal, true);
    pictures[set.normal] = out;
    return out;
}

std::shared_ptr<const TexturePicture> alphaPicture(const TextureSet& set) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TexturePicture>> pictures;
    if (set.alpha.empty()) return nullptr;
    const std::string key = set.alpha + (set.alphaChannel ? "|alpha" : "|grey");
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = pictures.find(key); it != pictures.end()) return it->second;
    std::shared_ptr<const TexturePicture> out = loadPicture(set.alpha, true, set.alphaChannel ? 3 : 0);
    pictures[key] = out;
    return out;
}

Vec3 bentNormal(const TexturePicture& map, bool directX, const Vec2& uv, const Vec3& n, const Vec3& tangent, float handed,
                float strength, const Vec3& face) {
    if (!(strength > 0.0f) || !(dot(tangent, tangent) > 0.0f)) return n;
    // The way the picture says the surface faces, its own x y z from 0..1;
    // tilted as strongly as asked -- weaker, its z toward 1, as Cycles has it.
    const Vec3 c = map.at(uv.x, uv.y);
    Vec3 m(2.0f * c.x - 1.0f, (2.0f * c.y - 1.0f) * (directX ? -1.0f : 1.0f), 2.0f * c.z - 1.0f);
    m.x *= strength;
    m.y *= strength;
    m.z = std::max(1.0f + (m.z - 1.0f) * std::min(strength, 1.0f), 1e-3f);
    // Its space: the tangent made across n again, and the way v goes.
    const Vec3 t = normalize(tangent - n * dot(n, tangent));
    const Vec3 b = cross(n, t) * handed;
    Vec3 bent = normalize(t * m.x + b * m.y + n * m.z);
    if (!std::isfinite(bent.x) || !std::isfinite(bent.y) || !std::isfinite(bent.z)) return n;
    // Not past the surface: a light that comes from behind it is none.
    const Vec3 f = dot(face, n) < 0.0f ? -face : face;
    const float above = dot(bent, f);
    if (above < 0.05f) bent = normalize(bent + f * (0.05f - above));
    return bent;
}

}  // namespace pg::render
