// How much a force acts at face f of component a -- PyroSolver::maskAt: 1
// everywhere (mask 0); else the heat and fuel (1), or the smoke (2), of the
// cells either side, at most 1. Wants the fields' buffer S.
float amountAt(int mask, int i, int j, int k) {
    const int at = find(0, sizeOf(0), i, j, k);
    if (at < 0) return 0.0;  // not stored: still, empty air
    if (mask == 1) {
        precise float r = S[kTemperature * p.plane + uint(at)] + S[kFuel * p.plane + uint(at)];
        return r;
    }
    return S[kDensity * p.plane + uint(at)];
}

float maskAt(int mask, int a, ivec3 f) {
    if (mask == 0) return 1.0;
    const int along = f[a];
    const int n = a == 0 ? p.nx : a == 1 ? p.ny : p.nz;
    precise float m = 0.0;
    if (along > 0) {
        ivec3 b = f;
        b[a] -= 1;
        m = m + amountAt(mask, b.x, b.y, b.z);
    }
    if (along < n) m = m + amountAt(mask, f.x, f.y, f.z);
    return mn(m, 1.0);
}
