#pragma once
//
// A dense 3D grid of floats -- the field type of the simulation.
//
// Cell-centred: cell (i, j, k) covers [i, i+1) x [j, j+1) x [k, k+1) in cell
// units and its value sits at the centre, (i + 0.5, j + 0.5, k + 0.5).
// Positions passed to sample() are in the same cell units, so the grid spans
// [0, nx] x [0, ny] x [0, nz].
//
// Dense, not sparse: a prototype of what production keeps in OpenVDB, where
// only the cells near smoke are stored. The interface is the part that stays.
//
#include <cstddef>
#include <vector>

namespace pg::sim {

class Grid {
public:
    Grid() = default;
    Grid(int nx, int ny, int nz, float value = 0.0f);

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    size_t size() const { return data_.size(); }

    size_t index(int i, int j, int k) const {
        return static_cast<size_t>(i) +
               static_cast<size_t>(nx_) * (static_cast<size_t>(j) + static_cast<size_t>(ny_) * static_cast<size_t>(k));
    }
    float& at(int i, int j, int k) { return data_[index(i, j, k)]; }
    float at(int i, int j, int k) const { return data_[index(i, j, k)]; }
    /// at(), to write -- as SparseGrid has it.
    float& ref(int i, int j, int k) { return data_[index(i, j, k)]; }
    /// The nearest cell inside the grid: a boundary that repeats itself.
    float clamped(int i, int j, int k) const;

    /// Trilinear interpolation at a position in cell units. Outside the grid
    /// the border repeats -- or, with zeroOutside, the value is 0.
    float sample(float x, float y, float z, bool zeroOutside = false) const;
    /// sample(), also giving the smallest and largest of the eight values it
    /// interpolated -- the bounds a MacCormack limiter clamps to.
    float sample(float x, float y, float z, bool zeroOutside, float& lo, float& hi) const;

    void fill(float value);
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }
    const std::vector<float>& values() const { return data_; }

    float max() const;
    double sum() const;

private:
    int nx_ = 0, ny_ = 0, nz_ = 0;
    std::vector<float> data_;
};

}  // namespace pg::sim
