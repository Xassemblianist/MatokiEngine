/* bench/bench_kernels.c — Elle yazılmış WAT çekirdekleriyle yarışan C (clang -O2 -msimd128) referansları.
 * Her fonksiyon __restrict işaretçilerle yazılmıştır; böylece clang otomatik vektörleştiricisi
 * (Loop/SLP Vectorizer) takma ad (aliasing) engeli olmadan en iyi kodu üretebilir.
 * Host ve wasm hedeflerinde birebir aynı IEEE-754 / tamsayı semantiğini korur.
 */
#include "bench/bench_kernels.h"
#include "core/math.h"

static inline i32 bench_trunc_sat_f32(f32 x) {
#if defined(__wasm__)
    return (i32)x;
#else
    if (x != x) return 0;
    if (x >= 2147483648.0f) return I32_MAX;
    if (x < -2147483648.0f) return I32_MIN;
    return (i32)x;
#endif
}

static inline u32 bench_clamp01_to_u8(f32 v) {
    f32 c = (0.0f < v) ? v : 0.0f;
    c = (c < 1.0f) ? c : 1.0f;
    return (u32)bench_trunc_sat_f32(c * 255.0f + 0.5f);
}

void bench_c_fill_u32(u32* __restrict dst, u32 val, u32 count) {
    for (u32 i = 0; i < count; i++) {
        dst[i] = val;
    }
}

void bench_c_blend_alpha_rgba8(u32* __restrict dst, const u32* __restrict src, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 s = src[i];
        u32 d = dst[i];
        u32 sa = (s >> 24u) & 0xFFu;
        u32 inv_a = 255u - sa;

        u32 sr = s & 0xFFu;
        u32 sg = (s >> 8u) & 0xFFu;
        u32 sb = (s >> 16u) & 0xFFu;

        u32 dr = d & 0xFFu;
        u32 dg = (d >> 8u) & 0xFFu;
        u32 db = (d >> 16u) & 0xFFu;
        u32 da = (d >> 24u) & 0xFFu;

        u32 rr = (sr * sa + dr * inv_a + 128u) >> 8u;
        u32 rg = (sg * sa + dg * inv_a + 128u) >> 8u;
        u32 rb = (sb * sa + db * inv_a + 128u) >> 8u;
        u32 ra = (255u * sa + da * inv_a + 128u) >> 8u;

        dst[i] = rr | (rg << 8u) | (rb << 16u) | (ra << 24u);
    }
}

void bench_c_rgba_f32_to_u8(u32* __restrict dst,
                            const f32* __restrict r,
                            const f32* __restrict g,
                            const f32* __restrict b,
                            const f32* __restrict a,
                            u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 ir = bench_clamp01_to_u8(r[i]);
        u32 ig = bench_clamp01_to_u8(g[i]);
        u32 ib = bench_clamp01_to_u8(b[i]);
        u32 ia = bench_clamp01_to_u8(a[i]);
        dst[i] = ir | (ig << 8u) | (ib << 16u) | (ia << 24u);
    }
}

void bench_c_edge_block_8x8(const i32* __restrict coeffs, i32 bx, i32 by, u8* __restrict out_masks) {
    i32 a0 = coeffs[0], b0 = coeffs[1], c0 = coeffs[2];
    i32 a1 = coeffs[3], b1 = coeffs[4], c1 = coeffs[5];
    i32 a2 = coeffs[6], b2 = coeffs[7], c2 = coeffs[8];

    i32 row_e0 = a0 * bx + b0 * by + c0;
    i32 row_e1 = a1 * bx + b1 * by + c1;
    i32 row_e2 = a2 * bx + b2 * by + c2;

    for (u32 ry = 0; ry < 4u; ry++) {
        i32 qe0 = row_e0;
        i32 qe1 = row_e1;
        i32 qe2 = row_e2;
        for (u32 rx = 0; rx < 4u; rx++) {
            i32 l0 = (qe0)           | (qe1)           | (qe2);
            i32 l1 = (qe0 + a0)      | (qe1 + a1)      | (qe2 + a2);
            i32 l2 = (qe0 + b0)      | (qe1 + b1)      | (qe2 + b2);
            i32 l3 = (qe0 + a0 + b0) | (qe1 + a1 + b1) | (qe2 + a2 + b2);
            u32 m = ((u32)(l0 >= 0) << 0u) |
                    ((u32)(l1 >= 0) << 1u) |
                    ((u32)(l2 >= 0) << 2u) |
                    ((u32)(l3 >= 0) << 3u);
            out_masks[ry * 4u + rx] = (u8)m;
            qe0 += 2 * a0;
            qe1 += 2 * a1;
            qe2 += 2 * a2;
        }
        row_e0 += 2 * b0;
        row_e1 += 2 * b1;
        row_e2 += 2 * b2;
    }
}

void bench_c_bilinear_gather(const Texture* __restrict tex,
                             const f32* __restrict u,
                             const f32* __restrict v,
                             f32* __restrict out_rgba,
                             u32 quad_count) {
    const u32* px = tex->px;
    f32 wf = (f32)(i32)tex->w;
    f32 hf = (f32)(i32)tex->h;
    i32 w_mask = (i32)tex->w_mask;
    i32 h_mask = (i32)tex->h_mask;
    u32 log2_w = tex->log2_w;
    const f32 inv255 = 1.0f / 255.0f;

    u32 total_lanes = quad_count * 4u;
    f32* out_r = out_rgba + 0u * total_lanes;
    f32* out_g = out_rgba + 1u * total_lanes;
    f32* out_b = out_rgba + 2u * total_lanes;
    f32* out_a = out_rgba + 3u * total_lanes;

    for (u32 i = 0; i < total_lanes; i++) {
        f32 fu = u[i] * wf - 0.5f;
        f32 fv = v[i] * hf - 0.5f;
        f32 x0f = m_floor(fu);
        f32 y0f = m_floor(fv);
        f32 ax = fu - x0f;
        f32 ay = fv - y0f;

        i32 x0i = bench_trunc_sat_f32(x0f);
        i32 y0i = bench_trunc_sat_f32(y0f);
        u32 x0 = (u32)(x0i & w_mask);
        u32 x1 = (u32)((x0i + 1) & w_mask);
        u32 r0 = (u32)(y0i & h_mask) << log2_w;
        u32 r1 = (u32)((y0i + 1) & h_mask) << log2_w;

        u32 p00 = px[r0 + x0];
        u32 p10 = px[r0 + x1];
        u32 p01 = px[r1 + x0];
        u32 p11 = px[r1 + x1];

        f32 r00 = (f32)(p00 & 0xFFu) * inv255;
        f32 r10 = (f32)(p10 & 0xFFu) * inv255;
        f32 r01 = (f32)(p01 & 0xFFu) * inv255;
        f32 r11 = (f32)(p11 & 0xFFu) * inv255;
        f32 r_top = r00 + (r10 - r00) * ax;
        f32 r_bot = r01 + (r11 - r01) * ax;
        out_r[i] = r_top + (r_bot - r_top) * ay;

        f32 g00 = (f32)((p00 >> 8u) & 0xFFu) * inv255;
        f32 g10 = (f32)((p10 >> 8u) & 0xFFu) * inv255;
        f32 g01 = (f32)((p01 >> 8u) & 0xFFu) * inv255;
        f32 g11 = (f32)((p11 >> 8u) & 0xFFu) * inv255;
        f32 g_top = g00 + (g10 - g00) * ax;
        f32 g_bot = g01 + (g11 - g01) * ax;
        out_g[i] = g_top + (g_bot - g_top) * ay;

        f32 b00 = (f32)((p00 >> 16u) & 0xFFu) * inv255;
        f32 b10 = (f32)((p10 >> 16u) & 0xFFu) * inv255;
        f32 b01 = (f32)((p01 >> 16u) & 0xFFu) * inv255;
        f32 b11 = (f32)((p11 >> 16u) & 0xFFu) * inv255;
        f32 b_top = b00 + (b10 - b00) * ax;
        f32 b_bot = b01 + (b11 - b01) * ax;
        out_b[i] = b_top + (b_bot - b_top) * ay;

        f32 a00 = (f32)((p00 >> 24u) & 0xFFu) * inv255;
        f32 a10 = (f32)((p10 >> 24u) & 0xFFu) * inv255;
        f32 a01 = (f32)((p01 >> 24u) & 0xFFu) * inv255;
        f32 a11 = (f32)((p11 >> 24u) & 0xFFu) * inv255;
        f32 a_top = a00 + (a10 - a00) * ax;
        f32 a_bot = a01 + (a11 - a01) * ax;
        out_a[i] = a_top + (a_bot - a_top) * ay;
    }
}

typedef f32 bench_v4f __attribute__((vector_size(16), aligned(4)));

f32 bench_c_sum_f32(const f32* __restrict a, u32 count) {
    if (count == 0u) return 0.0f;

    bench_v4f acc0 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc1 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc2 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc3 = { 0.0f, 0.0f, 0.0f, 0.0f };

    u32 i = 0u;
    u32 end16 = count & ~15u;
    for (; i < end16; i += 16u) {
        acc0 += *(const bench_v4f*)(a + i +  0u);
        acc1 += *(const bench_v4f*)(a + i +  4u);
        acc2 += *(const bench_v4f*)(a + i +  8u);
        acc3 += *(const bench_v4f*)(a + i + 12u);
    }

    acc0 = (acc0 + acc1) + (acc2 + acc3);

    u32 end4 = count & ~3u;
    for (; i < end4; i += 4u) {
        acc0 += *(const bench_v4f*)(a + i);
    }

    f32 sum = (acc0[0] + acc0[2]) + (acc0[1] + acc0[3]);
    for (; i < count; i++) {
        sum += a[i];
    }
    return sum;
}

f32 bench_c_dot_f32(const f32* __restrict a, const f32* __restrict b, u32 count) {
    if (count == 0u) return 0.0f;

    bench_v4f acc0 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc1 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc2 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bench_v4f acc3 = { 0.0f, 0.0f, 0.0f, 0.0f };

    u32 i = 0u;
    u32 end16 = count & ~15u;
    for (; i < end16; i += 16u) {
        acc0 += (*(const bench_v4f*)(a + i +  0u)) * (*(const bench_v4f*)(b + i +  0u));
        acc1 += (*(const bench_v4f*)(a + i +  4u)) * (*(const bench_v4f*)(b + i +  4u));
        acc2 += (*(const bench_v4f*)(a + i +  8u)) * (*(const bench_v4f*)(b + i +  8u));
        acc3 += (*(const bench_v4f*)(a + i + 12u)) * (*(const bench_v4f*)(b + i + 12u));
    }

    acc0 = (acc0 + acc1) + (acc2 + acc3);

    u32 end4 = count & ~3u;
    for (; i < end4; i += 4u) {
        acc0 += (*(const bench_v4f*)(a + i)) * (*(const bench_v4f*)(b + i));
    }

    f32 sum = (acc0[0] + acc0[2]) + (acc0[1] + acc0[3]);
    for (; i < count; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

void bench_c_mandelbrot_row(u32* __restrict out_iter, u32 width, f32 x0, f32 y0, f32 dx, u32 max_iter) {
    for (u32 i = 0; i < width; i++) {
        f32 cx = x0 + (f32)i * dx;
        f32 cy = y0;
        f32 zx = 0.0f;
        f32 zy = 0.0f;
        u32 iter = 0;
        while (iter < max_iter) {
            f32 zx2 = zx * zx;
            f32 zy2 = zy * zy;
            if (zx2 + zy2 > 4.0f) break;
            f32 nzy = (zx + zx) * zy + cy;
            zx = zx2 - zy2 + cx;
            zy = nzy;
            iter++;
        }
        out_iter[i] = iter;
    }
}
