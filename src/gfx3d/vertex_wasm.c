/* gfx3d/vertex_wasm.c — g3d_transform_positions'ın wasm SIMD uygulaması: 4 köşe/iterasyon.
 * Normal SIMD derlemesinde toplama sırası skaler sürümle aynıdır (bit-bit aynı sonuç);
 * relaxed SIMD derlemesinde (-mrelaxed-simd) simd_madd ile 3 adet f32x4.relaxed_madd kullanır.
 */
#include "gfx3d/vertex.h"
#include "core/simd.h"

void g3d_transform_positions(const Mat4* mt, const f32* px, const f32* py, const f32* pz, u32 n,
                             f32* ox, f32* oy, f32* oz, f32* ow) {
    const f32* m = mt->m;
    v128_t c[16];
    for (u32 k = 0; k < 16; k++) c[k] = wasm_f32x4_splat(m[k]);
    u32 i = 0;
    for (; i + 4 <= n; i += 4) {
        v128_t x = wasm_v128_load(px + i), y = wasm_v128_load(py + i), z = wasm_v128_load(pz + i);
        for (u32 r = 0; r < 4; r++) {
#if SIMD_RELAXED
            v128_t v = simd_madd(c[8 + r], z,
                           simd_madd(c[4 + r], y,
                               simd_madd(c[r], x, c[12 + r])));
#else
            v128_t v = wasm_f32x4_add(
                           simd_madd(c[8 + r], z,
                               simd_madd(c[4 + r], y, wasm_f32x4_mul(c[r], x))),
                           c[12 + r]);
#endif
            f32* dst = (r == 0) ? ox : (r == 1) ? oy : (r == 2) ? oz : ow;
            wasm_v128_store(dst + i, v);
        }
    }
    for (; i < n; i++) {
        f32 x = px[i], y = py[i], z = pz[i];
        ox[i] = m[0] * x + m[4] * y + m[ 8] * z + m[12];
        oy[i] = m[1] * x + m[5] * y + m[ 9] * z + m[13];
        oz[i] = m[2] * x + m[6] * y + m[10] * z + m[14];
        ow[i] = m[3] * x + m[7] * y + m[11] * z + m[15];
    }
}
