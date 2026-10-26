/* tests/test_kernels.c — C çekirdeklerinin host doğruluk testleri ve Node/V8 üzerinden
 * elle yazılmış WAT çekirdekleriyle bit-bit eşitlik doğrulaması.
 * 1) Host'ta bench_c_* fonksiyonlarının referans değerlerini ve sınır durumlarını denetler.
 * 2) Gömülü asm/ altındaki .wat kaynaklarının masm_assemble + mdis_disassemble gidiş-dönüşünü denetler.
 * 3) `node tools/host/run_kernels.mjs` ile çalışma anında derlenen WAT çekirdeklerinin
 *    C referanslarıyla bit-bit aynı çıktıyı ürettiğini doğrular.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "bench/bench_kernels.h"
#include "bench/asm_sources.h"
#include "masm/masm.h"
#include "masm/mdis.h"
#include "core/arena.h"
#include "core/rng.h"

static u8 g_kern_test_arena[4 * 1024 * 1024];

static u32 f32_to_bits(f32 x) {
    union { f32 f; u32 u; } u;
    u.f = x;
    return u.u;
}

TEST(kernels_c_host_correctness) {
    /* 1. fill_u32 */
    u32 buf[32];
    for (u32 i = 0; i < 32; i++) buf[i] = 0x11111111u;
    bench_c_fill_u32(buf, 0xCAFEBABEu, 19);
    for (u32 i = 0; i < 19; i++) CHECK(buf[i] == 0xCAFEBABEu);
    for (u32 i = 19; i < 32; i++) CHECK(buf[i] == 0x11111111u);

    /* 2. blend_alpha_rgba8: sa=0 -> dst korunur, sa=255 -> src (a=255) yazılır, sa=128 -> yarı yarıya */
    u32 dst[3] = { 0x80402010u, 0x80402010u, 0xFF0064C8u };
    u32 src[3] = { 0x00AABBCCu, 0xFF112233u, 0x80C86400u };
    bench_c_blend_alpha_rgba8(dst, src, 3);
    CHECK(dst[0] == 0x80402010u);
    CHECK(dst[1] == 0xFE112233u);
    /* sa=128, inv_a=127: r=(0*128 + 200*127 + 128)>>8 = 99 (0x63), g=100 (0x64), b=(200*128 + 0 + 128)>>8 = 100 (0x64), a=254 (0xFE) */
    CHECK(dst[2] == 0xFE646463u);

    /* 3. rgba_f32_to_u8: [0..1] ve taşma [-0.5, 1.5] */
    f32 r[4] = { 0.0f, 1.0f, -0.5f, 0.5f };
    f32 g[4] = { 1.0f, 0.0f,  1.5f, 0.25f };
    f32 b[4] = { 0.5f, 0.5f,  0.0f, 0.75f };
    f32 a[4] = { 1.0f, 1.0f,  1.0f, 1.0f };
    u32 out_u8[4] = { 0, 0, 0, 0 };
    bench_c_rgba_f32_to_u8(out_u8, r, g, b, a, 4);
    CHECK(out_u8[0] == 0xFF80FF00u);
    CHECK(out_u8[1] == 0xFF8000FFu);
    CHECK(out_u8[2] == 0xFF00FF00u);
    CHECK((out_u8[3] & 0xFFu) == 128u);

    /* 4. edge_block_8x8: tüm bloğu kapsayan üçgen -> 16 adet 0xF maskesi */
    i32 coeffs_all[9] = { 1, 0, 100,  0, 1, 100,  1, 1, 100 };
    u8 masks[16];
    bench_c_edge_block_8x8(coeffs_all, 0, 0, masks);
    for (u32 i = 0; i < 16; i++) CHECK(masks[i] == 0x0Fu);

    /* 5. bilinear_gather: 2x2 sabit renkli dokuda tam örnek */
    u32 tex_px[4] = { 0xFF204080u, 0xFF204080u, 0xFF204080u, 0xFF204080u };
    Texture tex;
    tex.px = tex_px;
    tex.w = 2; tex.h = 2;
    tex.w_mask = 1; tex.h_mask = 1;
    tex.log2_w = 1;
    tex.flags = TEX_FLAG_BILINEAR;
    tex._pad = 0;
    f32 u_arr[4] = { 0.25f, 0.75f, 0.25f, 0.75f };
    f32 v_arr[4] = { 0.25f, 0.25f, 0.75f, 0.75f };
    f32 out_rgba[16];
    bench_c_bilinear_gather(&tex, u_arr, v_arr, out_rgba, 1);
    for (u32 lane = 0; lane < 4; lane++) {
        CHECK(f32_to_bits(out_rgba[0 + lane])  == f32_to_bits(128.0f / 255.0f));
        CHECK(f32_to_bits(out_rgba[4 + lane])  == f32_to_bits(64.0f / 255.0f));
        CHECK(f32_to_bits(out_rgba[8 + lane])  == f32_to_bits(32.0f / 255.0f));
        CHECK(f32_to_bits(out_rgba[12 + lane]) == f32_to_bits(1.0f));
    }

    /* 6. sum_f32 ve dot_f32 */
    f32 vec_a[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 0.5f, 1.5f, 2.5f, 3.5f };
    f32 vec_b[8] = { 2.0f, 2.0f, 2.0f, 2.0f, 4.0f, 4.0f, 4.0f, 4.0f };
    CHECK(f32_to_bits(bench_c_sum_f32(vec_a, 8)) == f32_to_bits(18.0f));
    CHECK(f32_to_bits(bench_c_dot_f32(vec_a, vec_b, 8)) == f32_to_bits(52.0f));

    /* 7. mandelbrot_row: c = (0, 0) küme içinde (max_iter), c = (3, 0) 1 adımda çıkar */
    u32 iters[4] = { 0, 0, 0, 0 };
    bench_c_mandelbrot_row(iters, 2, 0.0f, 0.0f, 3.0f, 50);
    CHECK(iters[0] == 50u);
    CHECK(iters[1] == 1u);
}

TEST(kernels_embedded_wat_masm_and_node_bit_exact) {
    CHECK(g_embedded_asm_count >= 8u);

    Arena arena;
    arena_init(&arena, g_kern_test_arena, sizeof(g_kern_test_arena));

    for (u32 i = 0; i < g_embedded_asm_count; i++) {
        arena_reset(&arena);
        const EmbeddedAsm* emb = &g_embedded_asm[i];
        MasmResult mr = masm_assemble(&arena, emb->src, emb->len);
        CHECK(mr.ok);
        if (mr.ok) {
            MdisResult dr = mdis_disassemble(&arena, mr.wasm, mr.len);
            CHECK(dr.ok);
            if (dr.ok) {
                MasmResult rt = masm_assemble(&arena, dr.wat, dr.len);
                CHECK(rt.ok);
                CHECK(rt.len == mr.len);
                CHECK(memcmp(rt.wasm, mr.wasm, mr.len) == 0);
            }
        }
    }

    /* Node/V8 üzerinde gerçek JIT derleme + C vs WAT bit-bit doğrulaması */
    int rc = system("node tools/host/run_kernels.mjs");
    CHECK(rc == 0);
}
