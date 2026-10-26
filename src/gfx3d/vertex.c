/* gfx3d/vertex.c — g3d_transform_positions skaler yedeği (host ve SIMD'siz hedefler).
 * wasm SIMD derlemesinde gerçek uygulama vertex_wasm.c'dedir.
 */
#include "gfx3d/vertex.h"

#if !defined(__wasm_simd128__)
void g3d_transform_positions(const Mat4* mt, const f32* px, const f32* py, const f32* pz, u32 n,
                             f32* ox, f32* oy, f32* oz, f32* ow) {
    const f32* m = mt->m;
    for (u32 i = 0; i < n; i++) {
        f32 x = px[i], y = py[i], z = pz[i];
        ox[i] = m[0] * x + m[4] * y + m[ 8] * z + m[12];
        oy[i] = m[1] * x + m[5] * y + m[ 9] * z + m[13];
        oz[i] = m[2] * x + m[6] * y + m[10] * z + m[14];
        ow[i] = m[3] * x + m[7] * y + m[11] * z + m[15];
    }
}
#endif
