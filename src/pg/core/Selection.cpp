#include "pg/core/Selection.h"

#include <algorithm>
#include <charconv>

namespace pg {

namespace {

/// "12" or "3-40": the first and last numbers, inclusive. False when the
/// item is not numbers.
bool rangeOf(std::string_view item, size_t& first, size_t& last) {
    const size_t dash = item.find('-');
    auto number = [](std::string_view s, size_t& out) {
        if (s.empty()) return false;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        return r.ec == std::errc() && r.ptr == s.data() + s.size();
    };
    if (dash == std::string_view::npos) {
        if (!number(item, first)) return false;
        last = first;
        return true;
    }
    if (!number(item.substr(0, dash), first) || !number(item.substr(dash + 1), last)) return false;
    if (last < first) std::swap(first, last);
    return true;
}

}  // namespace

std::vector<uint8_t> pointsOfPrimitives(const Geometry& geo, std::span<const uint8_t> prims) {
    std::vector<uint8_t> points(geo.pointCount(), 0);
    for (size_t p = 0; p < geo.primitiveCount() && p < prims.size(); ++p) {
        if (!prims[p]) continue;
        for (const uint32_t q : geo.primitivePoints(p)) {
            if (q < points.size()) points[q] = 1;
        }
    }
    return points;
}

std::vector<uint8_t> primitivesOfPoints(const Geometry& geo, std::span<const uint8_t> points) {
    std::vector<uint8_t> prims(geo.primitiveCount(), 0);
    for (size_t p = 0; p < prims.size(); ++p) {
        const auto corners = geo.primitivePoints(p);
        if (corners.empty()) continue;
        bool all = true;
        for (const uint32_t q : corners) all = all && q < points.size() && points[q];
        prims[p] = all ? 1 : 0;
    }
    return prims;
}

std::vector<uint8_t> selectElements(const Geometry& geo, AttrClass cls, std::string_view pattern, bool* named) {
    const size_t n = cls == AttrClass::Primitive ? geo.primitiveCount() : geo.pointCount();
    std::vector<uint8_t> mask(n, 0);
    if (named) *named = false;
    size_t at = 0;
    while (at < pattern.size()) {
        // The next item: up to a space or a comma.
        while (at < pattern.size() && (pattern[at] == ' ' || pattern[at] == ',' || pattern[at] == '\t' ||
                                       pattern[at] == '\n')) {
            ++at;
        }
        size_t end = at;
        while (end < pattern.size() && pattern[end] != ' ' && pattern[end] != ',' && pattern[end] != '\t' &&
               pattern[end] != '\n') {
            ++end;
        }
        std::string_view item = pattern.substr(at, end - at);
        at = end;
        if (item.empty()) continue;
        uint8_t set = 1;
        if (item.front() == '^') {
            set = 0;
            item.remove_prefix(1);
            if (item.empty()) continue;
        }
        size_t first = 0, last = 0;
        if (item == "*") {
            std::fill(mask.begin(), mask.end(), set);
            if (named) *named = true;
        } else if (rangeOf(item, first, last)) {
            for (size_t i = first; i <= last && i < n; ++i) mask[i] = set;
            if (named) *named = true;
        } else if (const Group* g = geo.findGroup(std::string(item))) {
            if (named) *named = true;
            std::vector<uint8_t> members(g->classOf() == AttrClass::Primitive ? geo.primitiveCount() : geo.pointCount(), 0);
            for (size_t i = 0; i < members.size(); ++i) members[i] = g->contains(i) ? 1 : 0;
            if (g->classOf() != cls) {
                if (g->classOf() == AttrClass::Primitive && cls == AttrClass::Point) {
                    members = pointsOfPrimitives(geo, members);
                } else if (g->classOf() == AttrClass::Point && cls == AttrClass::Primitive) {
                    members = primitivesOfPoints(geo, members);
                } else {
                    continue;
                }
            }
            for (size_t i = 0; i < n && i < members.size(); ++i) {
                if (members[i]) mask[i] = set;
            }
        }
    }
    return mask;
}

std::string patternOf(std::span<const uint8_t> mask) {
    std::string out;
    size_t i = 0;
    while (i < mask.size()) {
        if (!mask[i]) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j + 1 < mask.size() && mask[j + 1]) ++j;
        if (!out.empty()) out += ' ';
        out += std::to_string(i);
        if (j > i) out += '-' + std::to_string(j);
        i = j + 1;
    }
    return out;
}

}  // namespace pg
