/* img/noise.c — Deterministik gürültü (noise) algoritmaları implementasyonu.
 * Tamsayı karıştırma (xxhash32 / murmur3 türevi) ile tablosuz gradyan/değer üretimi.
 * Perlin Quintic (6t^5 - 15t^4 + 10t^3) sönümleme eğrisi kullanır.
 * Döşenebilir (tileable) varyantlar pozitif modulo (imod) ile tam periyodik sınır sağlar.
 */
#include "img/noise.h"

static inline i32 imod(i32 x, i32 p) {
    if (p <= 0) return x;
    i32 r = x % p;
    return r < 0 ? r + p : r;
}

static inline f32 fade(f32 t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

u32 noise_hash1(u32 x, u32 seed) {
    u32 h = x ^ seed;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

u32 noise_hash2(i32 x, i32 y, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= (u32)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h = h * 5u + 0xE6546B64u;
    h ^= (u32)y * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

u32 noise_hash3(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= (u32)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= (u32)y * 0xC2B2AE35u;
    h = (h << 15) | (h >> 17);
    h ^= (u32)z * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

static inline f32 hash_to_01(u32 h) {
    return (f32)(h >> 8) * (1.0f / 16777216.0f);
}

/* --- Value Noise --- */

f32 noise_value2(f32 x, f32 y, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    f32 tx = x - fx;
    f32 ty = y - fy;
    f32 u = fade(tx);
    f32 v = fade(ty);

    f32 v00 = hash_to_01(noise_hash2(ix,     iy,     seed));
    f32 v10 = hash_to_01(noise_hash2(ix + 1, iy,     seed));
    f32 v01 = hash_to_01(noise_hash2(ix,     iy + 1, seed));
    f32 v11 = hash_to_01(noise_hash2(ix + 1, iy + 1, seed));

    return m_lerp(m_lerp(v00, v10, u), m_lerp(v01, v11, u), v);
}

f32 noise_value2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    f32 tx = x - fx;
    f32 ty = y - fy;
    f32 u = fade(tx);
    f32 v = fade(ty);

    i32 x0 = imod(ix,     period_x);
    i32 x1 = imod(ix + 1, period_x);
    i32 y0 = imod(iy,     period_y);
    i32 y1 = imod(iy + 1, period_y);

    f32 v00 = hash_to_01(noise_hash2(x0, y0, seed));
    f32 v10 = hash_to_01(noise_hash2(x1, y0, seed));
    f32 v01 = hash_to_01(noise_hash2(x0, y1, seed));
    f32 v11 = hash_to_01(noise_hash2(x1, y1, seed));

    return m_lerp(m_lerp(v00, v10, u), m_lerp(v01, v11, u), v);
}

f32 noise_value3(f32 x, f32 y, f32 z, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    f32 fz = m_floor(z);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    i32 iz = (i32)fz;
    f32 u = fade(x - fx);
    f32 v = fade(y - fy);
    f32 w = fade(z - fz);

    f32 v000 = hash_to_01(noise_hash3(ix,     iy,     iz,     seed));
    f32 v100 = hash_to_01(noise_hash3(ix + 1, iy,     iz,     seed));
    f32 v010 = hash_to_01(noise_hash3(ix,     iy + 1, iz,     seed));
    f32 v110 = hash_to_01(noise_hash3(ix + 1, iy + 1, iz,     seed));
    f32 v001 = hash_to_01(noise_hash3(ix,     iy,     iz + 1, seed));
    f32 v101 = hash_to_01(noise_hash3(ix + 1, iy,     iz + 1, seed));
    f32 v011 = hash_to_01(noise_hash3(ix,     iy + 1, iz + 1, seed));
    f32 v111 = hash_to_01(noise_hash3(ix + 1, iy + 1, iz + 1, seed));

    f32 z0 = m_lerp(m_lerp(v000, v100, u), m_lerp(v010, v110, u), v);
    f32 z1 = m_lerp(m_lerp(v001, v101, u), m_lerp(v011, v111, u), v);
    return m_lerp(z0, z1, w);
}

/* --- Gradient (Perlin) Noise --- */

static inline f32 grad2(u32 hash, f32 dx, f32 dy) {
    /* 8 yön: 4 eksen + 4 köşegen */
    switch (hash & 7u) {
        case 0: return  dx + dy;
        case 1: return -dx + dy;
        case 2: return  dx - dy;
        case 3: return -dx - dy;
        case 4: return  dx * 1.41421356f;
        case 5: return -dx * 1.41421356f;
        case 6: return  dy * 1.41421356f;
        default: return -dy * 1.41421356f;
    }
}

static inline f32 grad3(u32 hash, f32 x, f32 y, f32 z) {
    u32 h = hash & 15u;
    f32 u = h < 8u ? x : y;
    f32 v = h < 4u ? y : (h == 12u || h == 14u ? x : z);
    return ((h & 1u) == 0 ? u : -u) + ((h & 2u) == 0 ? v : -v);
}

f32 noise_perlin2(f32 x, f32 y, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    f32 dx = x - fx;
    f32 dy = y - fy;
    f32 u = fade(dx);
    f32 v = fade(dy);

    f32 n00 = grad2(noise_hash2(ix,     iy,     seed), dx,        dy);
    f32 n10 = grad2(noise_hash2(ix + 1, iy,     seed), dx - 1.0f, dy);
    f32 n01 = grad2(noise_hash2(ix,     iy + 1, seed), dx,        dy - 1.0f);
    f32 n11 = grad2(noise_hash2(ix + 1, iy + 1, seed), dx - 1.0f, dy - 1.0f);

    /* Ölçekleme: [-1, 1] aralığına oturtmak için ~0.7071 */
    f32 res = m_lerp(m_lerp(n00, n10, u), m_lerp(n01, n11, u), v) * 0.70710678f;
    return m_clamp(res, -1.0f, 1.0f);
}

f32 noise_perlin2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    f32 dx = x - fx;
    f32 dy = y - fy;
    f32 u = fade(dx);
    f32 v = fade(dy);

    i32 x0 = imod(ix,     period_x);
    i32 x1 = imod(ix + 1, period_x);
    i32 y0 = imod(iy,     period_y);
    i32 y1 = imod(iy + 1, period_y);

    f32 n00 = grad2(noise_hash2(x0, y0, seed), dx,        dy);
    f32 n10 = grad2(noise_hash2(x1, y0, seed), dx - 1.0f, dy);
    f32 n01 = grad2(noise_hash2(x0, y1, seed), dx,        dy - 1.0f);
    f32 n11 = grad2(noise_hash2(x1, y1, seed), dx - 1.0f, dy - 1.0f);

    f32 res = m_lerp(m_lerp(n00, n10, u), m_lerp(n01, n11, u), v) * 0.70710678f;
    return m_clamp(res, -1.0f, 1.0f);
}

f32 noise_perlin3(f32 x, f32 y, f32 z, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    f32 fz = m_floor(z);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    i32 iz = (i32)fz;
    f32 dx = x - fx;
    f32 dy = y - fy;
    f32 dz = z - fz;
    f32 u = fade(dx);
    f32 v = fade(dy);
    f32 w = fade(dz);

    f32 n000 = grad3(noise_hash3(ix,     iy,     iz,     seed), dx,        dy,        dz);
    f32 n100 = grad3(noise_hash3(ix + 1, iy,     iz,     seed), dx - 1.0f, dy,        dz);
    f32 n010 = grad3(noise_hash3(ix,     iy + 1, iz,     seed), dx,        dy - 1.0f, dz);
    f32 n110 = grad3(noise_hash3(ix + 1, iy + 1, iz,     seed), dx - 1.0f, dy - 1.0f, dz);
    f32 n001 = grad3(noise_hash3(ix,     iy,     iz + 1, seed), dx,        dy,        dz - 1.0f);
    f32 n101 = grad3(noise_hash3(ix + 1, iy,     iz + 1, seed), dx - 1.0f, dy,        dz - 1.0f);
    f32 n011 = grad3(noise_hash3(ix,     iy + 1, iz + 1, seed), dx,        dy - 1.0f, dz - 1.0f);
    f32 n111 = grad3(noise_hash3(ix + 1, iy + 1, iz + 1, seed), dx - 1.0f, dy - 1.0f, dz - 1.0f);

    f32 z0 = m_lerp(m_lerp(n000, n100, u), m_lerp(n010, n110, u), v);
    f32 z1 = m_lerp(m_lerp(n001, n101, u), m_lerp(n011, n111, u), v);
    return m_clamp(m_lerp(z0, z1, w) * 0.9649214f, -1.0f, 1.0f);
}

f32 noise_perlin3_tile(f32 x, f32 y, f32 z, i32 px, i32 py, i32 pz, u32 seed) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    f32 fz = m_floor(z);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;
    i32 iz = (i32)fz;
    f32 dx = x - fx;
    f32 dy = y - fy;
    f32 dz = z - fz;
    f32 u = fade(dx);
    f32 v = fade(dy);
    f32 w = fade(dz);

    i32 x0 = imod(ix,     px), x1 = imod(ix + 1, px);
    i32 y0 = imod(iy,     py), y1 = imod(iy + 1, py);
    i32 z0 = imod(iz,     pz), z1 = imod(iz + 1, pz);

    f32 n000 = grad3(noise_hash3(x0, y0, z0, seed), dx,        dy,        dz);
    f32 n100 = grad3(noise_hash3(x1, y0, z0, seed), dx - 1.0f, dy,        dz);
    f32 n010 = grad3(noise_hash3(x0, y1, z0, seed), dx,        dy - 1.0f, dz);
    f32 n110 = grad3(noise_hash3(x1, y1, z0, seed), dx - 1.0f, dy - 1.0f, dz);
    f32 n001 = grad3(noise_hash3(x0, y0, z1, seed), dx,        dy,        dz - 1.0f);
    f32 n101 = grad3(noise_hash3(x1, y0, z1, seed), dx - 1.0f, dy,        dz - 1.0f);
    f32 n011 = grad3(noise_hash3(x0, y1, z1, seed), dx,        dy - 1.0f, dz - 1.0f);
    f32 n111 = grad3(noise_hash3(x1, y1, z1, seed), dx - 1.0f, dy - 1.0f, dz - 1.0f);

    f32 rz0 = m_lerp(m_lerp(n000, n100, u), m_lerp(n010, n110, u), v);
    f32 rz1 = m_lerp(m_lerp(n001, n101, u), m_lerp(n011, n111, u), v);
    return m_clamp(m_lerp(rz0, rz1, w) * 0.9649214f, -1.0f, 1.0f);
}

/* --- 2D Simplex Noise --- */

f32 noise_simplex2(f32 x, f32 y, u32 seed) {
    const f32 F2 = 0.36602540378f; /* 0.5 * (sqrt(3) - 1) */
    const f32 G2 = 0.21132486540f; /* (3 - sqrt(3)) / 6 */

    f32 s = (x + y) * F2;
    f32 fx = m_floor(x + s);
    f32 fy = m_floor(y + s);
    i32 i = (i32)fx;
    i32 j = (i32)fy;

    f32 t = (f32)(i + j) * G2;
    f32 x0 = x - ((f32)i - t);
    f32 y0 = y - ((f32)j - t);

    i32 i1 = (x0 > y0) ? 1 : 0;
    i32 j1 = (x0 > y0) ? 0 : 1;

    f32 x1 = x0 - (f32)i1 + G2;
    f32 y1 = y0 - (f32)j1 + G2;
    f32 x2 = x0 - 1.0f + 2.0f * G2;
    f32 y2 = y0 - 1.0f + 2.0f * G2;

    f32 n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;

    f32 t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * grad2(noise_hash2(i, j, seed), x0, y0);
    }

    f32 t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * grad2(noise_hash2(i + i1, j + j1, seed), x1, y1);
    }

    f32 t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * grad2(noise_hash2(i + 1, j + 1, seed), x2, y2);
    }

    return m_clamp(45.23065f * (n0 + n1 + n2), -1.0f, 1.0f);
}

f32 noise_simplex2_tile(f32 x, f32 y, f32 period_x, f32 period_y, u32 seed) {
    /* 2D periyodik uzayı iki dik 3D silindirik/harmonik bileşenle harmanlayarak
     * hem izotropik hem de tam döşenebilir (tileable) C2 sürekli gürültü üretir. */
    if (period_x <= 0.0f || period_y <= 0.0f) {
        return noise_simplex2(x, y, seed);
    }
    f32 ax = (x / period_x) * M_TAU;
    f32 ay = (y / period_y) * M_TAU;
    f32 rx = period_x / M_TAU;
    f32 ry = period_y / M_TAU;

    f32 cx = m_cos(ax) * rx;
    f32 sx = m_sin(ax) * rx;
    f32 cy = m_cos(ay) * ry;
    f32 sy = m_sin(ay) * ry;

    f32 a = noise_simplex2(cx + cy * 0.5f, sx + sy * 0.5f, seed);
    f32 b = noise_simplex2(cy - sx * 0.5f, sy + cx * 0.5f, seed ^ 0xA5A55A5Au);
    return m_clamp((a + b) * 0.70710678f, -1.0f, 1.0f);
}

/* --- 2D Worley (Cellular) Noise --- */

f32 noise_worley2(f32 x, f32 y, u32 seed, f32* out_f2) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;

    f32 d1 = 100.0f;
    f32 d2 = 100.0f;

    for (i32 dy = -1; dy <= 1; dy++) {
        for (i32 dx = -1; dx <= 1; dx++) {
            i32 cx = ix + dx;
            i32 cy = iy + dy;
            u32 h = noise_hash2(cx, cy, seed);
            f32 px = (f32)cx + (f32)(h & 0xFFFFu) * (1.0f / 65536.0f);
            f32 py = (f32)cy + (f32)(h >> 16)     * (1.0f / 65536.0f);
            f32 vx = px - x;
            f32 vy = py - y;
            f32 dist2 = vx * vx + vy * vy;
            if (dist2 < d1) {
                d2 = d1;
                d1 = dist2;
            } else if (dist2 < d2) {
                d2 = dist2;
            }
        }
    }

    f32 f1 = m_min(m_sqrt(d1), 1.0f);
    if (out_f2) {
        *out_f2 = m_min(m_sqrt(d2), 1.0f);
    }
    return f1;
}

f32 noise_worley2_tile(f32 x, f32 y, i32 period_x, i32 period_y, u32 seed, f32* out_f2) {
    f32 fx = m_floor(x);
    f32 fy = m_floor(y);
    i32 ix = (i32)fx;
    i32 iy = (i32)fy;

    f32 d1 = 100.0f;
    f32 d2 = 100.0f;

    for (i32 dy = -1; dy <= 1; dy++) {
        for (i32 dx = -1; dx <= 1; dx++) {
            i32 cx = ix + dx;
            i32 cy = iy + dy;
            i32 wx = imod(cx, period_x);
            i32 wy = imod(cy, period_y);
            u32 h = noise_hash2(wx, wy, seed);
            f32 px = (f32)cx + (f32)(h & 0xFFFFu) * (1.0f / 65536.0f);
            f32 py = (f32)cy + (f32)(h >> 16)     * (1.0f / 65536.0f);
            f32 vx = px - x;
            f32 vy = py - y;
            f32 dist2 = vx * vx + vy * vy;
            if (dist2 < d1) {
                d2 = d1;
                d1 = dist2;
            } else if (dist2 < d2) {
                d2 = dist2;
            }
        }
    }

    f32 f1 = m_min(m_sqrt(d1), 1.0f);
    if (out_f2) {
        *out_f2 = m_min(m_sqrt(d2), 1.0f);
    }
    return f1;
}

/* --- fBm & Domain Warp --- */

f32 noise_fbm2(f32 x, f32 y, i32 octaves, f32 lacunarity, f32 gain, u32 seed) {
    f32 sum = 0.0f;
    f32 amp = 1.0f;
    f32 freq = 1.0f;
    f32 norm = 0.0f;

    for (i32 i = 0; i < octaves; i++) {
        sum += amp * noise_perlin2(x * freq, y * freq, seed + (u32)i * 1013u);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? (sum / norm) : 0.0f;
}

f32 noise_fbm2_tile(f32 x, f32 y, i32 period_x, i32 period_y,
                    i32 octaves, f32 lacunarity, f32 gain, u32 seed) {
    f32 sum = 0.0f;
    f32 amp = 1.0f;
    f32 freq = 1.0f;
    f32 norm = 0.0f;

    for (i32 i = 0; i < octaves; i++) {
        i32 px = (i32)(period_x * freq);
        i32 py = (i32)(period_y * freq);
        if (px < 1) px = 1;
        if (py < 1) py = 1;
        sum += amp * noise_perlin2_tile(x * freq, y * freq, px, py, seed + (u32)i * 1013u);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? (sum / norm) : 0.0f;
}

f32 noise_warp2(f32 x, f32 y, f32 strength, i32 octaves, u32 seed) {
    f32 qx = noise_fbm2(x, y, octaves, 2.0f, 0.5f, seed + 17u);
    f32 qy = noise_fbm2(x + 5.2f, y + 1.3f, octaves, 2.0f, 0.5f, seed + 53u);
    return noise_fbm2(x + strength * qx, y + strength * qy, octaves, 2.0f, 0.5f, seed + 97u);
}

f32 noise_warp2_tile(f32 x, f32 y, i32 period_x, i32 period_y,
                     f32 strength, i32 octaves, u32 seed) {
    f32 qx = noise_fbm2_tile(x, y, period_x, period_y, octaves, 2.0f, 0.5f, seed + 17u);
    f32 qy = noise_fbm2_tile(x, y, period_x, period_y, octaves, 2.0f, 0.5f, seed + 53u);
    return noise_fbm2_tile(x + strength * qx, y + strength * qy,
                           period_x, period_y, octaves, 2.0f, 0.5f, seed + 97u);
}
