// The neighbours of a cell along each axis, kept inside the grid, and the
// distance between them -- PyroSolver::addVorticity's around(): p.f[0] is
// 1 / h, p.f[1] 1 / (2 h), as the CPU worked them out.
struct Around {
    ivec3 minus[3], plus[3];
    float scale[3];
};

Around around(ivec3 c) {
    Around r;
    const ivec3 lo = max(c - 1, ivec3(0)), hi = min(c + 1, ivec3(p.nx, p.ny, p.nz) - 1);
    for (int b = 0; b < 3; ++b) {
        r.minus[b] = c;
        r.plus[b] = c;
        r.minus[b][b] = lo[b];
        r.plus[b][b] = hi[b];
        r.scale[b] = hi[b] - lo[b] == 2 ? p.f[1] : p.f[0];
    }
    return r;
}
