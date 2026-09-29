#include "pg/sim/State.h"

namespace pg::sim {

void StateWriter::grid(const Grid& g) {
    pod(static_cast<int32_t>(g.nx()));
    pod(static_cast<int32_t>(g.ny()));
    pod(static_cast<int32_t>(g.nz()));
    list(g.values());
}

void StateWriter::tiles(const Tiles& t) {
    pod(static_cast<int32_t>(t.nx()));
    pod(static_cast<int32_t>(t.ny()));
    pod(static_cast<int32_t>(t.nz()));
    pod(static_cast<int32_t>(t.axis()));
    list(t.states());
}

bool StateReader::grid(Grid& g) {
    int32_t n[3] = {0, 0, 0};
    std::vector<float> values;
    if (!pod(n[0]) || !pod(n[1]) || !pod(n[2]) || !list(values)) return fail();
    if (n[0] < 0 || n[1] < 0 || n[2] < 0 ||
        values.size() != static_cast<size_t>(n[0]) * static_cast<size_t>(n[1]) * static_cast<size_t>(n[2])) {
        return fail();
    }
    g = Grid(n[0], n[1], n[2]);
    if (!values.empty()) std::memcpy(g.data(), values.data(), values.size() * sizeof(float));
    return true;
}

bool StateReader::values(SparseGrid& g) {
    std::vector<float> values;
    if (!list(values) || values.size() != g.size()) return fail();
    if (!values.empty()) std::memcpy(g.data(), values.data(), values.size() * sizeof(float));
    return true;
}

bool StateReader::tiles(std::shared_ptr<const Tiles>& t) {
    int32_t n[3] = {0, 0, 0}, axis = -1;
    std::vector<uint8_t> state;
    if (!pod(n[0]) || !pod(n[1]) || !pod(n[2]) || !pod(axis) || !list(state)) return fail();
    if (n[0] < 1 || n[1] < 1 || n[2] < 1 || axis < -1 || axis > 2) return fail();
    const size_t count = static_cast<size_t>((n[0] + Tiles::kSide - 1) / Tiles::kSide) *
                         static_cast<size_t>((n[1] + Tiles::kSide - 1) / Tiles::kSide) *
                         static_cast<size_t>((n[2] + Tiles::kSide - 1) / Tiles::kSide);
    if (state.size() != count) return fail();
    for (const uint8_t s : state) {
        if (s > Tiles::FirstLayer) return fail();
    }
    t = std::make_shared<const Tiles>(n[0], n[1], n[2], std::move(state), axis);
    return true;
}

}  // namespace pg::sim
