/* jit/jit_selftest.c — Tarayıcıda JIT köprüsü (mbin + masm + call_indirect) öz-testi.
 * 1) mbin ile (i32, i32) -> i32 toplama modülü üretir, jit_load ile yükler ve doğrular.
 * 2) masm ile WAT metninden 128-bit SIMD + paylaşılan bellek kullanan bir fonksiyon
 *    derler, jit_load ile yükler, fonksiyon işaretçisiyle çağırır ve doğrular.
 * Ayrıca doğrudan C çağrısı vs call_indirect JIT çağrı maliyeti ölçüm fonksiyonlarını içerir.
 */
#include "jit/jit.h"
#include "masm/masm.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"
#include "plat/platform.h"
#include "core/arena.h"
#include "core/log.h"

typedef i32 (*JitAddFn)(i32 a, i32 b);
typedef i32 (*JitSimdFn)(f32* buf, f32 scale);

static u8 g_selftest_arena_mem[1048576];
static JitAddFn g_bench_jit_add = NULL;

__attribute__((noinline)) static i32 direct_add_fn(i32 a, i32 b) {
    /* Derleyicinin döngüyü tamamen sabit katlamasını önlemek için volatile olmayan
     * ama noinline bir fonksiyon */
    return a + (b ^ 1);
}

EXPORT(engine_jit_selftest) i32 engine_jit_selftest(void) {
    /* 1. mbin ile (i32, i32) -> i32 toplama modülü üret */
    u8 work_mem[4096];
    u8 wasm_mem[1024];
    MbModule m;
    MbBuf out;

    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, wasm_mem, sizeof(wasm_mem));

    const u8 params[2] = { MB_VAL_I32, MB_VAL_I32 };
    const u8 results[1] = { MB_VAL_I32 };
    u32 t_add = mb_type(&m, params, 2, results, 1);

    MbFunc* f = mb_func_begin(&m, t_add);
    mb_local_get(f, 0);
    mb_local_get(f, 1);
    mb_i32_add(f);
    mb_func_end(f);

    mb_export_func(&m, "add", f->func_idx);
    mb_module_finish(&m, &out);

    JitAddFn fn_add = (JitAddFn)jit_load(out.p, out.len, "add");
    if (!fn_add) {
        LOG("[jit_selftest] FAIL: mbin add module failed to load");
        return 1;
    }
    if (fn_add(17, 25) != 42 || fn_add(-100, 300) != 200) {
        LOG("[jit_selftest] FAIL: mbin add returned wrong value");
        return 2;
    }

    /* Benchmark için aynı imzalı (a + (b ^ 1)) JIT fonksiyonunu da hazırla */
    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, wasm_mem, sizeof(wasm_mem));
    t_add = mb_type(&m, params, 2, results, 1);
    f = mb_func_begin(&m, t_add);
    mb_local_get(f, 0);
    mb_local_get(f, 1);
    mb_i32_const(f, 1);
    mb_i32_xor(f);
    mb_i32_add(f);
    mb_func_end(f);
    mb_export_func(&m, "bench_add", f->func_idx);
    mb_module_finish(&m, &out);
    g_bench_jit_add = (JitAddFn)jit_load(out.p, out.len, "bench_add");

    /* 2. masm ile metinden SIMD + paylaşılan bellek fonksiyonu derle ve yükle */
    static const char* simd_wat =
        "(module\n"
#if defined(PLAT_SHARED_MEMORY) && PLAT_SHARED_MEMORY
        "  (import \"env\" \"memory\" (memory 1 16384 shared))\n"
#else
        "  (import \"env\" \"memory\" (memory 1 16384))\n"
#endif
        "  (func $simd_dot (export \"simd_dot\") (param $ptr i32) (param $scale f32) (result i32)\n"
        "    (local $v v128)\n"
        "    (local.set $v\n"
        "      (f32x4.add\n"
        "        (f32x4.mul\n"
        "          (v128.load (local.get $ptr))\n"
        "          (f32x4.splat (local.get $scale)))\n"
        "        (v128.const f32x4 10.0 20.0 30.0 40.0)))\n"
        "    (v128.store offset=16 (local.get $ptr) (local.get $v))\n"
        "    (i32.trunc_sat_f32_s\n"
        "      (f32.add\n"
        "        (f32x4.extract_lane 0 (local.get $v))\n"
        "        (f32x4.extract_lane 3 (local.get $v))))\n"
        "  )\n"
        ")\n";

    Arena arena;
    arena_init(&arena, g_selftest_arena_mem, sizeof(g_selftest_arena_mem));
    MasmResult mr = masm_assemble_cstr(&arena, simd_wat);
    if (!mr.ok) {
        LOG("[jit_selftest] FAIL: masm_assemble failed: %s", mr.msg);
        return 3;
    }

    JitSimdFn fn_simd = (JitSimdFn)jit_load(mr.wasm, mr.len, "simd_dot");
    if (!fn_simd) {
        LOG("[jit_selftest] FAIL: masm SIMD module failed to load");
        return 4;
    }

    /* Paylaşılan bellekte (16-bayt hizalı) giriş ve çıkış dizisi */
    _Alignas(16) f32 io_buf[8] = { 1.5f, 2.5f, 3.5f, 4.5f, 0.0f, 0.0f, 0.0f, 0.0f };
    /* Beklenen:
     * v = (1.5, 2.5, 3.5, 4.5) * 2.0 + (10, 20, 30, 40) = (13, 25, 37, 49)
     * dönüş = lane0 + lane3 = 13 + 49 = 62
     */
    i32 simd_res = fn_simd(io_buf, 2.0f);
    if (simd_res != 62) {
        LOG("[jit_selftest] FAIL: fn_simd returned %d, expected 62", simd_res);
        return 5;
    }
    if (io_buf[4] != 13.0f || io_buf[5] != 25.0f || io_buf[6] != 37.0f || io_buf[7] != 49.0f) {
        LOG("[jit_selftest] FAIL: fn_simd shared memory store mismatch");
        return 6;
    }

    LOG("[jit_selftest] JIT OK (mbin add=42, masm simd_dot=62)");
    return 0;
}

EXPORT(engine_jit_bench_direct) i32 engine_jit_bench_direct(u32 iterations) {
    i32 acc = 0;
    for (u32 i = 0; i < iterations; i++) {
        acc = direct_add_fn(acc, (i32)i);
    }
    return acc;
}

EXPORT(engine_jit_bench_indirect) i32 engine_jit_bench_indirect(u32 iterations) {
    JitAddFn fn = g_bench_jit_add;
    if (!fn) return -1;
    i32 acc = 0;
    for (u32 i = 0; i < iterations; i++) {
        acc = fn(acc, (i32)i);
    }
    return acc;
}
