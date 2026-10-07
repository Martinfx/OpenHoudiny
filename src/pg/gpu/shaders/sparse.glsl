// The sparse grids of the gas solver on the device (pg/sim/SparseGrid.h,
// pg/sim/PyroGpu.cpp): tiles of 8 x 8 x 8 cells, a table of the tiles of the
// whole box saying where each is stored (-1: not), the values of the stored
// ones 512 a tile. Four sets of tiles: the cells (set 0) and the faces along
// x, y and z (sets 1 to 3), one longer along their axis.
//
// Every lookup here is the CPU's, operation for operation -- min and max as
// std::min and std::max take them, no fused multiply-add (precise) -- so a
// kernel gives the CPU's result to the bit.
//
// Binding 0 holds the tables (T), as ints; the kernels bind their fields
// after it.

layout(std430, binding = 0) readonly buffer Tables { int T[]; };

layout(push_constant) uniform Push {
    int nx, ny, nz;     // cells
    uint slots[4];      // where each set's table starts in T
    uint stored;        // where the set gone over lists its stored tiles: the tile, the top bit for FirstLayer
    uint slotCount;     // ... and how many
    uint vel[3];        // where each component of the velocity starts in its buffer
    uint field;         // where the field carried starts in its buffer
    uint plane;         // values a field of the cells has: 512 a stored tile
    int axis;           // the velocity's component carried
    int floorClosed;    // the floor at y = 0 lets nothing through
    float cells;        // the step, in cells: dt / the cell's size
} p;

const int kSide = 8;

float mn(float a, float b) { return b < a ? b : a; }  // std::min
float mx(float a, float b) { return a < b ? b : a; }  // std::max

ivec3 sizeOf(int set) {
    ivec3 n = ivec3(p.nx, p.ny, p.nz);
    if (set > 0) n[set - 1] += 1;
    return n;
}

/// Where cell (i, j, k) of `set` is in a field of that set: -1 when its tile
/// is not stored.
int find(int set, ivec3 n, int i, int j, int k) {
    const ivec3 t = (n + (kSide - 1)) / kSide;
    const uint tile = uint(i >> 3) + uint(t.x) * (uint(j >> 3) + uint(t.y) * uint(k >> 3));
    const int s = T[p.slots[set] + tile];
    if (s < 0) return -1;
    return s * 512 + (i & 7) + kSide * ((j & 7) + kSide * (k & 7));
}

/// What a trilinear lookup at (x, y, z) reads, as SparseGrid::cornersAt:
/// the eight places, x fastest, and how far between them.
void cornersAt(int set, float x, float y, float z, out int at[8], out vec3 t) {
    const ivec3 n = sizeOf(set);
    const float fx = mx(0.0, mn(x - 0.5, float(n.x - 1)));
    const float fy = mx(0.0, mn(y - 0.5, float(n.y - 1)));
    const float fz = mx(0.0, mn(z - 0.5, float(n.z - 1)));
    const int i = int(fx), j = int(fy), k = int(fz);
    precise float tx = fx - float(i), ty = fy - float(j), tz = fz - float(k);
    t = vec3(tx, ty, tz);
    const int di = i + 1 < n.x ? 1 : 0, dj = j + 1 < n.y ? 1 : 0, dk = k + 1 < n.z ? 1 : 0;
    for (int q = 0; q < 8; ++q) {
        at[q] = find(set, n, i + ((q & 1) != 0 ? di : 0), j + ((q & 2) != 0 ? dj : 0), k + ((q & 4) != 0 ? dk : 0));
    }
}

/// Trilinear between eight values, as SparseGrid::lerp.
float trilinear(float c[8], vec3 t) {
    precise float x00 = c[0] + (c[1] - c[0]) * t.x, x10 = c[2] + (c[3] - c[2]) * t.x;
    precise float x01 = c[4] + (c[5] - c[4]) * t.x, x11 = c[6] + (c[7] - c[6]) * t.x;
    precise float y0 = x00 + (x10 - x00) * t.y, y1 = x01 + (x11 - x01) * t.y;
    precise float r = y0 + (y1 - y0) * t.z;
    return r;
}

/// Outside the box of `set`'s cells, as SparseGrid's outside().
bool outside(float x, float y, float z, int nx, int ny, int nz) {
    return x < 0.0 || y < 0.0 || z < 0.0 || x > float(nx) || y > float(ny) || z > float(nz);
}

/// The cell the thread does of the stored tile it is given, if it counts:
/// the tile's slot, and the cell. A work group is a tile, 8 x 8 x 4
/// threads, each doing two cells: z and z + 4 (`part`).
bool cellOf(int set, uint part, out uint slot, out ivec3 cell) {
    slot = gl_WorkGroupID.x + gl_NumWorkGroups.x * gl_WorkGroupID.y;
    if (slot >= p.slotCount) return false;
    const uint entry = uint(T[p.stored + slot]);
    const uint tile = entry & 0x7fffffffu;
    const ivec3 n = sizeOf(set);
    const ivec3 tiles = (n + (kSide - 1)) / kSide;
    const ivec3 corner = ivec3(int(tile % uint(tiles.x)), int((tile / uint(tiles.x)) % uint(tiles.y)),
                               int(tile / uint(tiles.x * tiles.y))) * kSide;
    ivec3 extent = min(ivec3(kSide), n - corner);
    if (set > 0 && (entry & 0x80000000u) != 0u) extent[set - 1] = min(extent[set - 1], 1);
    const ivec3 local = ivec3(gl_LocalInvocationID.xy, int(gl_LocalInvocationID.z + 4u * part));
    if (any(greaterThanEqual(local, extent))) return false;
    cell = corner + local;
    return true;
}

/// Where cell `local` of a stored tile is in its field.
uint indexOf(uint slot, ivec3 cell) {
    return slot * 512u + uint((cell.x & 7) + kSide * ((cell.y & 7) + kSide * (cell.z & 7)));
}
