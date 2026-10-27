/* img/jpeg_wasm.c — JPEG 8x8 sabit noktalı LLM IDCT ve YCbCr -> RGBA8 dönüşümünün wasm SIMD sürümü.
 * i32x4 kelebek (butterfly) ve 4x4 matris devriği (transpose) ile 4 sütun / 4 satırı paralel işler.
 * Tüm işlemler skaler sürümle aynı 32-bit tamsayı aritmetiğini izler; çıktı bit-bit aynıdır.
 */
#include "img/jpeg.h"
#include "raster/framebuffer.h"

#if defined(__wasm_simd128__)
#include <wasm_simd128.h>

static const u8 JPEG_UNZIGZAG_SIMD[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* 4x4 i32x4 matris devriği (transpose) */
static inline __attribute__((always_inline)) void simd_transpose_4x4(
    v128_t r0, v128_t r1, v128_t r2, v128_t r3,
    v128_t* c0, v128_t* c1, v128_t* c2, v128_t* c3) {
    v128_t t0 = wasm_i32x4_shuffle(r0, r1, 0, 4, 1, 5);
    v128_t t1 = wasm_i32x4_shuffle(r0, r1, 2, 6, 3, 7);
    v128_t t2 = wasm_i32x4_shuffle(r2, r3, 0, 4, 1, 5);
    v128_t t3 = wasm_i32x4_shuffle(r2, r3, 2, 6, 3, 7);
    *c0 = wasm_i32x4_shuffle(t0, t2, 0, 1, 4, 5);
    *c1 = wasm_i32x4_shuffle(t0, t2, 2, 3, 6, 7);
    *c2 = wasm_i32x4_shuffle(t1, t3, 0, 1, 4, 5);
    *c3 = wasm_i32x4_shuffle(t1, t3, 2, 3, 6, 7);
}

/* 8 noktalı 1B LLM IDCT kelebeği (4 paralel şerit) */
static inline __attribute__((always_inline)) void simd_idct_1d(
    v128_t s0, v128_t s1, v128_t s2, v128_t s3,
    v128_t s4, v128_t s5, v128_t s6, v128_t s7,
    v128_t bias, u32 shift,
    v128_t* o0, v128_t* o1, v128_t* o2, v128_t* o3,
    v128_t* o4, v128_t* o5, v128_t* o6, v128_t* o7) {
    v128_t p2 = s2;
    v128_t p3 = s6;
    v128_t p1 = wasm_i32x4_mul(wasm_i32x4_add(p2, p3), wasm_i32x4_const_splat(4433));
    v128_t t2 = wasm_i32x4_add(p1, wasm_i32x4_mul(p3, wasm_i32x4_const_splat(-15137)));
    v128_t t3 = wasm_i32x4_add(p1, wasm_i32x4_mul(p2, wasm_i32x4_const_splat(6270)));

    p2 = s0;
    p3 = s4;
    v128_t t0 = wasm_i32x4_shl(wasm_i32x4_add(p2, p3), 13);
    v128_t t1 = wasm_i32x4_shl(wasm_i32x4_sub(p2, p3), 13);

    v128_t x0 = wasm_i32x4_add(wasm_i32x4_add(t0, t3), bias);
    v128_t x3 = wasm_i32x4_add(wasm_i32x4_sub(t0, t3), bias);
    v128_t x1 = wasm_i32x4_add(wasm_i32x4_add(t1, t2), bias);
    v128_t x2 = wasm_i32x4_add(wasm_i32x4_sub(t1, t2), bias);

    t0 = s7;
    t1 = s5;
    t2 = s3;
    t3 = s1;

    p3 = wasm_i32x4_add(t0, t2);
    v128_t p4 = wasm_i32x4_add(t1, t3);
    p1 = wasm_i32x4_add(t0, t3);
    p2 = wasm_i32x4_add(t1, t2);
    v128_t p5 = wasm_i32x4_mul(wasm_i32x4_add(p3, p4), wasm_i32x4_const_splat(9633));

    t0 = wasm_i32x4_mul(t0, wasm_i32x4_const_splat(2446));
    t1 = wasm_i32x4_mul(t1, wasm_i32x4_const_splat(16819));
    t2 = wasm_i32x4_mul(t2, wasm_i32x4_const_splat(25172));
    t3 = wasm_i32x4_mul(t3, wasm_i32x4_const_splat(12299));

    p1 = wasm_i32x4_add(p5, wasm_i32x4_mul(p1, wasm_i32x4_const_splat(-7373)));
    p2 = wasm_i32x4_add(p5, wasm_i32x4_mul(p2, wasm_i32x4_const_splat(-20995)));
    p3 = wasm_i32x4_mul(p3, wasm_i32x4_const_splat(-16069));
    p4 = wasm_i32x4_mul(p4, wasm_i32x4_const_splat(-3196));

    t0 = wasm_i32x4_add(t0, wasm_i32x4_add(p1, p3));
    t1 = wasm_i32x4_add(t1, wasm_i32x4_add(p2, p4));
    t2 = wasm_i32x4_add(t2, wasm_i32x4_add(p2, p3));
    t3 = wasm_i32x4_add(t3, wasm_i32x4_add(p1, p4));

    *o0 = wasm_i32x4_shr(wasm_i32x4_add(x0, t3), shift);
    *o7 = wasm_i32x4_shr(wasm_i32x4_sub(x0, t3), shift);
    *o1 = wasm_i32x4_shr(wasm_i32x4_add(x1, t2), shift);
    *o6 = wasm_i32x4_shr(wasm_i32x4_sub(x1, t2), shift);
    *o2 = wasm_i32x4_shr(wasm_i32x4_add(x2, t1), shift);
    *o5 = wasm_i32x4_shr(wasm_i32x4_sub(x2, t1), shift);
    *o3 = wasm_i32x4_shr(wasm_i32x4_add(x3, t0), shift);
    *o4 = wasm_i32x4_shr(wasm_i32x4_sub(x3, t0), shift);
}

void jpeg_idct_block(const i16* block_zz, const u16* quant_zz, u8* out, u32 stride) {
    _Alignas(16) i32 d[64];
    for (u32 i = 0; i < 64; i++) d[i] = 0;
    for (u32 k = 0; k < 64; k++) {
        i32 c = (i32)block_zz[k] * (i32)quant_zz[k];
        d[JPEG_UNZIGZAG_SIMD[k]] = CLAMP(c, -8192, 8192);
    }

    _Alignas(16) i32 tmp[64];
    const v128_t pass1_bias = wasm_i32x4_const_splat(1024);
    const v128_t c_min = wasm_i32x4_const_splat(-8192);
    const v128_t c_max = wasm_i32x4_const_splat(8192);

    /* 1. Geçiş: Sütunlar 0..3 ve 4..7 */
    for (u32 half = 0; half < 2; half++) {
        u32 off = half * 4u;
        v128_t s0 = wasm_v128_load(d + 0 * 8 + off);
        v128_t s1 = wasm_v128_load(d + 1 * 8 + off);
        v128_t s2 = wasm_v128_load(d + 2 * 8 + off);
        v128_t s3 = wasm_v128_load(d + 3 * 8 + off);
        v128_t s4 = wasm_v128_load(d + 4 * 8 + off);
        v128_t s5 = wasm_v128_load(d + 5 * 8 + off);
        v128_t s6 = wasm_v128_load(d + 6 * 8 + off);
        v128_t s7 = wasm_v128_load(d + 7 * 8 + off);

        v128_t o0, o1, o2, o3, o4, o5, o6, o7;
        simd_idct_1d(s0, s1, s2, s3, s4, s5, s6, s7, pass1_bias, 11,
                     &o0, &o1, &o2, &o3, &o4, &o5, &o6, &o7);

        wasm_v128_store(tmp + 0 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o0, c_max), c_min));
        wasm_v128_store(tmp + 1 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o1, c_max), c_min));
        wasm_v128_store(tmp + 2 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o2, c_max), c_min));
        wasm_v128_store(tmp + 3 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o3, c_max), c_min));
        wasm_v128_store(tmp + 4 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o4, c_max), c_min));
        wasm_v128_store(tmp + 5 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o5, c_max), c_min));
        wasm_v128_store(tmp + 6 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o6, c_max), c_min));
        wasm_v128_store(tmp + 7 * 8 + off, wasm_i32x4_max(wasm_i32x4_min(o7, c_max), c_min));
    }

    /* 2. Geçiş: Satırlar 0..3 ve 4..7 (4x4 devrik ile) */
    const v128_t pass2_bias = wasm_i32x4_const_splat(131072 + (128 << 18));
    const v128_t zero = wasm_i32x4_const_splat(0);
    const v128_t c255 = wasm_i32x4_const_splat(255);

    for (u32 half = 0; half < 2; half++) {
        u32 rbase = half * 4u;
        const i32* r0 = tmp + (rbase + 0u) * 8u;
        const i32* r1 = tmp + (rbase + 1u) * 8u;
        const i32* r2 = tmp + (rbase + 2u) * 8u;
        const i32* r3 = tmp + (rbase + 3u) * 8u;

        v128_t s0, s1, s2, s3, s4, s5, s6, s7;
        simd_transpose_4x4(wasm_v128_load(r0 + 0), wasm_v128_load(r1 + 0),
                           wasm_v128_load(r2 + 0), wasm_v128_load(r3 + 0),
                           &s0, &s1, &s2, &s3);
        simd_transpose_4x4(wasm_v128_load(r0 + 4), wasm_v128_load(r1 + 4),
                           wasm_v128_load(r2 + 4), wasm_v128_load(r3 + 4),
                           &s4, &s5, &s6, &s7);

        v128_t o0, o1, o2, o3, o4, o5, o6, o7;
        simd_idct_1d(s0, s1, s2, s3, s4, s5, s6, s7, pass2_bias, 18,
                     &o0, &o1, &o2, &o3, &o4, &o5, &o6, &o7);

        o0 = wasm_i32x4_max(wasm_i32x4_min(o0, c255), zero);
        o1 = wasm_i32x4_max(wasm_i32x4_min(o1, c255), zero);
        o2 = wasm_i32x4_max(wasm_i32x4_min(o2, c255), zero);
        o3 = wasm_i32x4_max(wasm_i32x4_min(o3, c255), zero);
        o4 = wasm_i32x4_max(wasm_i32x4_min(o4, c255), zero);
        o5 = wasm_i32x4_max(wasm_i32x4_min(o5, c255), zero);
        o6 = wasm_i32x4_max(wasm_i32x4_min(o6, c255), zero);
        o7 = wasm_i32x4_max(wasm_i32x4_min(o7, c255), zero);

        v128_t row0_lo, row1_lo, row2_lo, row3_lo;
        v128_t row0_hi, row1_hi, row2_hi, row3_hi;
        simd_transpose_4x4(o0, o1, o2, o3, &row0_lo, &row1_lo, &row2_lo, &row3_lo);
        simd_transpose_4x4(o4, o5, o6, o7, &row0_hi, &row1_hi, &row2_hi, &row3_hi);

        _Alignas(16) i32 out_rows[4][8];
        wasm_v128_store(out_rows[0] + 0, row0_lo); wasm_v128_store(out_rows[0] + 4, row0_hi);
        wasm_v128_store(out_rows[1] + 0, row1_lo); wasm_v128_store(out_rows[1] + 4, row1_hi);
        wasm_v128_store(out_rows[2] + 0, row2_lo); wasm_v128_store(out_rows[2] + 4, row2_hi);
        wasm_v128_store(out_rows[3] + 0, row3_lo); wasm_v128_store(out_rows[3] + 4, row3_hi);

        for (u32 r = 0; r < 4; r++) {
            u8* dst = out + (rbase + r) * stride;
            for (u32 c = 0; c < 8; c++) {
                dst[c] = (u8)out_rows[r][c];
            }
        }
    }
}

void jpeg_ycbcr_to_rgba_row(const u8* y_row, const u8* cb_row, const u8* cr_row, u32* dst, u32 w) {
    const v128_t c128    = wasm_i32x4_const_splat(128);
    const v128_t c8192   = wasm_i32x4_const_splat(8192);
    const v128_t c22970  = wasm_i32x4_const_splat(22970);
    const v128_t cm5638  = wasm_i32x4_const_splat(-5638);
    const v128_t cm11700 = wasm_i32x4_const_splat(-11700);
    const v128_t c29032  = wasm_i32x4_const_splat(29032);
    const v128_t c0      = wasm_i32x4_const_splat(0);
    const v128_t c255    = wasm_i32x4_const_splat(255);
    const v128_t calpha  = wasm_i32x4_const_splat((i32)0xFF000000u);

    u32 x = 0;
    for (; x + 4u <= w; x += 4u) {
        v128_t vy  = wasm_i32x4_make(y_row[x + 0],  y_row[x + 1],  y_row[x + 2],  y_row[x + 3]);
        v128_t vcb = wasm_i32x4_sub(wasm_i32x4_make(cb_row[x + 0], cb_row[x + 1], cb_row[x + 2], cb_row[x + 3]), c128);
        v128_t vcr = wasm_i32x4_sub(wasm_i32x4_make(cr_row[x + 0], cr_row[x + 1], cr_row[x + 2], cr_row[x + 3]), c128);

        v128_t vr = wasm_i32x4_add(vy, wasm_i32x4_shr(wasm_i32x4_add(wasm_i32x4_mul(vcr, c22970), c8192), 14));
        v128_t vg = wasm_i32x4_add(vy, wasm_i32x4_shr(wasm_i32x4_add(
                        wasm_i32x4_add(wasm_i32x4_mul(vcb, cm5638), wasm_i32x4_mul(vcr, cm11700)), c8192), 14));
        v128_t vb = wasm_i32x4_add(vy, wasm_i32x4_shr(wasm_i32x4_add(wasm_i32x4_mul(vcb, c29032), c8192), 14));

        vr = wasm_i32x4_max(wasm_i32x4_min(vr, c255), c0);
        vg = wasm_i32x4_max(wasm_i32x4_min(vg, c255), c0);
        vb = wasm_i32x4_max(wasm_i32x4_min(vb, c255), c0);

        v128_t rgba = wasm_v128_or(wasm_v128_or(vr, wasm_i32x4_shl(vg, 8)),
                                   wasm_v128_or(wasm_i32x4_shl(vb, 16), calpha));
        wasm_v128_store(dst + x, rgba);
    }

    if (x < w) {
        jpeg_ycbcr_to_rgba_row_scalar(y_row + x, cb_row + x, cr_row + x, dst + x, w - x);
    }
}

#endif /* defined(__wasm_simd128__) */
