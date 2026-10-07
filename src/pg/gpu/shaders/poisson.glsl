// The pressure's multigrid on the device (pg/sim/Poisson.h, PyroGpu.cpp):
// each level a sparse grid of tiles of 8 x 8 x 8 cells, as sparse.glsl has
// them. Every kernel is PoissonSolver's arithmetic operation for operation --
// a product by 1 / the diagonal, which the CPU worked out, never a division
// -- so the solve is the CPU's to the bit.
//
//   T (binding 0)  the tables: each level's tiles, and with solids the faces'
//   F (binding 1)  the values: 1/0 .. 1/12 first, then each level's p, b, r,
//                  and with solids its diagonal, 1 / it, its faces
//   B (binding 2)  bits: each level's cells that count, 32 a word; the finest
//                  level's open faces with solids, a byte a cell

layout(std430, binding = 0) readonly buffer Tables { int T[]; };
layout(std430, binding = 1) buffer Values { float F[]; };
layout(std430, binding = 2) readonly buffer Bits { uint B[]; };

layout(push_constant) uniform Push {
    int nx, ny, nz;    // the level's cells
    uint slots;        // its table in T
    uint stored;       // its stored tiles in T
    uint count;        // ... how many
    uint faceSlots[3]; // with solids, below the finest: the tables of its faces
    uint p, b, r;      // where its grids start in F
    uint diag, inv;    // with solids: its diagonal, and 1 / it
    uint faces[3];     // with solids, below the finest: its faces
    uint on;           // its cells that count, in B
    uint open;         // with solids, the finest: its open faces, in B
    int mode;          // 0 no solids; 1 the finest with solids (open bits); 2 below it (faces)
    int colour;        // relax: the cells (i + j + k) & 1 == colour
    float omega;       // relax: 1, or over-relaxed on the coarsest level
    float h2, invH2;   // h * h, 1 / (h * h)
    uint closed;       // the walls: bit s for side s (-x, +x, -y, +y, -z, +z)
    int onx, ony, onz; // the other level -- restrict: the finer, prolong: the coarser
    uint oslots;       // its table in T
    uint other;        // the grid read there: the finer's r, the coarser's p
} p;

const int kSide = 8;

/// Where cell (i, j, k) of a grid of n cells whose table starts at `slots`
/// is: -1 when its tile is not stored.
int find(uint slots, ivec3 n, int i, int j, int k) {
    const ivec3 t = (n + (kSide - 1)) / kSide;
    const uint tile = uint(i >> 3) + uint(t.x) * (uint(j >> 3) + uint(t.y) * uint(k >> 3));
    const int s = T[slots + tile];
    if (s < 0) return -1;
    return s * 512 + (i & 7) + kSide * ((j & 7) + kSide * (k & 7));
}

/// SparseGrid::at of the grid at `base` in F: 0 where not stored.
float at(uint slots, ivec3 n, uint base, int i, int j, int k) {
    const int c = find(slots, n, i, j, k);
    return c < 0 ? 0.0 : F[base + uint(c)];
}

ivec3 level() { return ivec3(p.nx, p.ny, p.nz); }

/// Does stored cell c of the level count?
bool counts(uint c) { return ((B[p.on + (c >> 5)] >> (c & 31u)) & 1u) != 0u; }

/// The stored tile the work group does: its slot and first cell; false past
/// the last.
bool tileOf(out uint slot, out ivec3 corner) {
    slot = gl_WorkGroupID.x + gl_NumWorkGroups.x * gl_WorkGroupID.y;
    if (slot >= p.count) return false;
    const uint tile = uint(T[p.stored + slot]);
    const ivec3 tiles = (level() + (kSide - 1)) / kSide;
    corner = ivec3(int(tile % uint(tiles.x)), int((tile / uint(tiles.x)) % uint(tiles.y)),
                   int(tile / uint(tiles.x * tiles.y))) * kSide;
    return true;
}

uint indexOf(uint slot, ivec3 cell) {
    return slot * 512u + uint((cell.x & 7) + kSide * ((cell.y & 7) + kSide * (cell.z & 7)));
}

bool closedSide(int side) { return ((p.closed >> uint(side)) & 1u) != 0u; }

/// Without solids: the neighbours inside the grid, and the diagonal -- 6, one
/// more for each open side of the box the cell touches, one less for each
/// wall (Poisson.cpp, neighbours()).
float neighbours(ivec3 c, out float diagonal) {
    const ivec3 n = level();
    precise float sum = 0.0;
    float d = 6.0;
    if (c.x > 0) sum = sum + at(p.slots, n, p.p, c.x - 1, c.y, c.z); else d += closedSide(0) ? -1.0 : 1.0;
    if (c.x < n.x - 1) sum = sum + at(p.slots, n, p.p, c.x + 1, c.y, c.z); else d += closedSide(1) ? -1.0 : 1.0;
    if (c.y > 0) sum = sum + at(p.slots, n, p.p, c.x, c.y - 1, c.z); else d += closedSide(2) ? -1.0 : 1.0;
    if (c.y < n.y - 1) sum = sum + at(p.slots, n, p.p, c.x, c.y + 1, c.z); else d += closedSide(3) ? -1.0 : 1.0;
    if (c.z > 0) sum = sum + at(p.slots, n, p.p, c.x, c.y, c.z - 1); else d += closedSide(4) ? -1.0 : 1.0;
    if (c.z < n.z - 1) sum = sum + at(p.slots, n, p.p, c.x, c.y, c.z + 1); else d += closedSide(5) ? -1.0 : 1.0;
    diagonal = d;
    return sum;
}

/// The finest level with solids: the neighbours behind open faces
/// (openSum()).
float openSum(ivec3 c, uint index) {
    const ivec3 n = level();
    const uint open = (B[p.open + (index >> 2)] >> ((index & 3u) * 8u)) & 0xffu;
    precise float sum = 0.0;
    if (c.x > 0 && (open & 1u) != 0u) sum = sum + at(p.slots, n, p.p, c.x - 1, c.y, c.z);
    if (c.x < n.x - 1 && (open & 2u) != 0u) sum = sum + at(p.slots, n, p.p, c.x + 1, c.y, c.z);
    if (c.y > 0 && (open & 4u) != 0u) sum = sum + at(p.slots, n, p.p, c.x, c.y - 1, c.z);
    if (c.y < n.y - 1 && (open & 8u) != 0u) sum = sum + at(p.slots, n, p.p, c.x, c.y + 1, c.z);
    if (c.z > 0 && (open & 16u) != 0u) sum = sum + at(p.slots, n, p.p, c.x, c.y, c.z - 1);
    if (c.z < n.z - 1 && (open & 32u) != 0u) sum = sum + at(p.slots, n, p.p, c.x, c.y, c.z + 1);
    return sum;
}

/// A face's coefficient below the finest level with solids: a[axis].at().
float face(int axis, int i, int j, int k) {
    ivec3 n = level();
    n[axis] += 1;
    return at(p.faceSlots[axis], n, p.faces[axis], i, j, k);
}

/// Below the finest level with solids: the neighbours, each weighted by the
/// face between (weightedSum()).
float weightedSum(ivec3 c) {
    const ivec3 n = level();
    precise float sum = 0.0;
    if (c.x > 0) sum = sum + face(0, c.x, c.y, c.z) * at(p.slots, n, p.p, c.x - 1, c.y, c.z);
    if (c.x < n.x - 1) sum = sum + face(0, c.x + 1, c.y, c.z) * at(p.slots, n, p.p, c.x + 1, c.y, c.z);
    if (c.y > 0) sum = sum + face(1, c.x, c.y, c.z) * at(p.slots, n, p.p, c.x, c.y - 1, c.z);
    if (c.y < n.y - 1) sum = sum + face(1, c.x, c.y + 1, c.z) * at(p.slots, n, p.p, c.x, c.y + 1, c.z);
    if (c.z > 0) sum = sum + face(2, c.x, c.y, c.z) * at(p.slots, n, p.p, c.x, c.y, c.z - 1);
    if (c.z < n.z - 1) sum = sum + face(2, c.x, c.y, c.z + 1) * at(p.slots, n, p.p, c.x, c.y, c.z + 1);
    return sum;
}
