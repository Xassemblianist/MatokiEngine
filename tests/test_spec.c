/* tests/test_spec.c — Çalışma anı özelleştirme (F0_B / ARCH_V2 H1) birim, diferansiyel ve benchmark testleri.
 * 1) mbin ile üretilen 4 çekirdek türünün wasm-validate + mdis/masm gidiş-dönüş ve sabit/ofset gömme doğrulaması.
 * 2) SpecCache isabet/ıska ve arketip düzeni / sabit değiştiğinde yeniden derleme testi.
 * 3) Host'ta EcsQuery (genel C) vs C-SoA (3 geçiş ve birleşik) bit-bit doğruluk + derleme süresi ölçümü.
 * 4) Node/V8 üzerinde gerçek JIT derleme + 100k varlık bit-bit doğrulama ve hızlanma ölçümü.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "spec/spec.h"
#include "masm/masm.h"
#include "masm/mdis.h"
#include "plat/platform.h"
#include "core/arena.h"
#include "core/rng.h"

static u8 g_spec_test_mem[MB(36)];

static void ensure_spec_test_dir(void) {
    mkdir("build", 0755);
    mkdir("build/test_spec", 0755);
}

static inline u32 f32_bits(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof(u32));
    return u;
}

/* 1. Wasm ikili üretimi, wasm-validate, mdis/masm gidiş-dönüş ve gömülü SoA ofset/sabit denetimi */
TEST(spec_codegen_and_disassembly) {
    ensure_spec_test_dir();

    Arena arena;
    arena_init(&arena, g_spec_test_mem, MB(4));

    SpecArchLayout layout;
    spec_arch_layout_init(&layout, 0x3u, 100000u, 0u);
    /* 100000 zaten 16'nın katı: col_bytes = 400000 */
    CHECK(layout.cap == 100000u);
    CHECK(layout.off_p[0] == 0u);
    CHECK(layout.off_p[1] == 400000u);
    CHECK(layout.off_p[2] == 800000u);
    CHECK(layout.off_v[0] == 1200000u);
    CHECK(layout.off_v[1] == 1600000u);
    CHECK(layout.off_v[2] == 2000000u);

    SpecParams params = spec_default_params();
    u8 wasm_buf[8192];

    for (u32 k = 0u; k < (u32)SPEC_KERNEL_COUNT; k++) {
        SpecKernelKind kind = (SpecKernelKind)k;
        u32 len = spec_codegen_kernel(kind, &layout, &params,
                                      TRUE, FALSE, &arena, wasm_buf, sizeof(wasm_buf));
        CHECK(len > 64u);

        char path[128];
        snprintf(path, sizeof(path), "build/test_spec/kernel_%u.wasm", k);
        FILE* fp = fopen(path, "wb");
        CHECK(fp != NULL);
        if (fp) {
            fwrite(wasm_buf, 1, len, fp);
            fclose(fp);
        }

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "wasm-validate --enable-all %s", path);
        CHECK(system(cmd) == 0);

        MdisResult dr = mdis_disassemble(&arena, wasm_buf, len);
        CHECK(dr.ok);
        if (dr.ok) {
            /* Gömülü SoA sütun ofsetleri (400000, 1200000, 1600000, 2000000) ve 4x unroll (+16, +32, +48) kontrolü */
            CHECK(strstr(dr.wat, "offset=1600000") != NULL);
            CHECK(strstr(dr.wat, "offset=1600016") != NULL);
            CHECK(strstr(dr.wat, "offset=1600032") != NULL);
            CHECK(strstr(dr.wat, "offset=1600048") != NULL);

            if (kind == SPEC_KERNEL_BOUNCE || kind == SPEC_KERNEL_FUSED) {
                CHECK(strstr(dr.wat, "f32x4.pmax") != NULL);
                CHECK(strstr(dr.wat, "f32x4.pmin") != NULL);
                CHECK(strstr(dr.wat, "v128.bitselect") != NULL);
            }

            /* mdis -> masm byte-byte gidiş-dönüş doğrulaması */
            MasmResult rt = masm_assemble(&arena, dr.wat, dr.len);
            CHECK(rt.ok);
            CHECK(rt.len == len);
            if (rt.ok && rt.len == len) {
                CHECK(memcmp(rt.wasm, wasm_buf, len) == 0);
            }
        }
    }

    /* Hizasız (16'nın katı olmayan) sütun ofseti reddedilmeli */
    SpecArchLayout bad_layout = layout;
    bad_layout.off_p[1] = 400004u;
    CHECK(spec_codegen_kernel(SPEC_KERNEL_MOVE, &bad_layout, &params,
                              TRUE, FALSE, &arena, wasm_buf, sizeof(wasm_buf)) == 0u);
}

/* 2. Varyant önbelleği (SpecCache) ve düzen/sabit değişiminde yeniden özelleştirme */
TEST(spec_cache_and_respecialization) {
    Arena arena;
    arena_init(&arena, g_spec_test_mem, MB(4));

    SpecCache cache;
    spec_cache_init(&cache, &arena);

    SpecArchLayout lay_a, lay_b;
    spec_arch_layout_init(&lay_a, 0x3u, 50000u, 0u);
    spec_arch_layout_init(&lay_b, 0x7u, 30000u, 1u);

    SpecParams p1 = spec_default_params();
    SpecKey ka1 = spec_make_key(SPEC_KERNEL_FUSED, &lay_a, &p1);
    const SpecVariant* v1 = spec_cache_get(&cache, &ka1, &arena);
    CHECK(v1 != NULL);
    CHECK(v1->wasm_len > 0u);
    CHECK(cache.count == 1u);
    CHECK(cache.hits == 0u);
    CHECK(cache.misses == 1u);

    /* Aynı anahtarla ikinci sorgu: önbellekten (hit) dönmeli */
    const SpecVariant* v1_hit = spec_cache_get(&cache, &ka1, &arena);
    CHECK(v1_hit == v1);
    CHECK(cache.hits == 1u);
    CHECK(cache.misses == 1u);

    /* Farklı arketip düzeni (lay_b): yeni modül üretilmeli ve farklı baytlara sahip olmalı */
    SpecKey kb1 = spec_make_key(SPEC_KERNEL_FUSED, &lay_b, &p1);
    const SpecVariant* v2 = spec_cache_get(&cache, &kb1, &arena);
    CHECK(v2 != NULL);
    CHECK(v2 != v1);
    CHECK(cache.count == 2u);
    CHECK(cache.misses == 2u);
    CHECK(v2->wasm_len != v1->wasm_len || memcmp(v2->wasm, v1->wasm, v1->wasm_len) != 0);

    /* Farklı sabit (dt değişimi): yeni modül üretilmeli */
    SpecParams p2 = p1;
    p2.dt = 0.025f;
    SpecKey ka2 = spec_make_key(SPEC_KERNEL_FUSED, &lay_a, &p2);
    const SpecVariant* v3 = spec_cache_get(&cache, &ka2, &arena);
    CHECK(v3 != NULL);
    CHECK(v3 != v1);
    CHECK(cache.count == 3u);
    CHECK(cache.misses == 3u);
    CHECK(memcmp(v3->wasm, v1->wasm, v1->wasm_len) != 0);
}

/* 3. Host doğruluk testi (EcsQuery vs C-SoA 3-geçiş vs C-SoA birleşik) + host derleme süresi */
TEST(spec_host_ecs_vs_c_soa_and_compile_speed) {
    Arena arena;
    arena_init(&arena, g_spec_test_mem, sizeof(g_spec_test_mem));

    SpecParams params = spec_default_params();
    static const u32 k_counts[] = { 0u, 1u, 3u, 4u, 7u, 15u, 16u, 17u, 19u, 131u };

    for (u32 c = 0u; c < ARRAY_LEN(k_counts); c++) {
        usize mark = arena_mark(&arena);
        u32 n = k_counts[c];

        EcsWorld w;
        ecs_world_init(&w, &arena, n + 16u);
        EcsCompId cid_pos = ecs_register(&w, "Pos", sizeof(SpecPos), _Alignof(SpecPos), NULL);
        EcsCompId cid_vel = ecs_register(&w, "Vel", sizeof(SpecVel), _Alignof(SpecVel), NULL);
        EcsCompId cid_tag = ecs_register(&w, "Tag", sizeof(u32),     _Alignof(u32),     NULL);

        Rng rng;
        rng_seed(&rng, 0x12345678ULL + c);
        for (u32 i = 0u; i < n; i++) {
            EcsEntity e = ecs_spawn(&w);
            SpecPos p = {
                (rng_f32(&rng) * 2.0f - 1.0f) * 70.0f,
                (rng_f32(&rng) * 2.0f - 1.0f) * 40.0f,
                (rng_f32(&rng) * 2.0f - 1.0f) * 70.0f
            };
            SpecVel v = {
                (rng_f32(&rng) * 2.0f - 1.0f) * 30.0f,
                (rng_f32(&rng) * 2.0f - 1.0f) * 30.0f,
                (rng_f32(&rng) * 2.0f - 1.0f) * 30.0f
            };
            if (i == 0u) { p.x = -0.0f; v.vx = 0.0f; }
            ecs_set(&w, e, cid_pos, &p);
            ecs_set(&w, e, cid_vel, &v);
            if (i & 1u) {
                u32 t = i;
                ecs_set(&w, e, cid_tag, &t);
            }
        }

        SpecWorld sw3, swf;
        spec_world_build(&sw3, &w, cid_pos, cid_vel, &arena);
        spec_world_build(&swf, &w, cid_pos, cid_vel, &arena);

        for (u32 step = 0u; step < 4u; step++) {
            spec_ecs_step_all(&w, cid_pos, cid_vel, &params);
            spec_c_soa_step_all(&sw3, &params);
            spec_c_soa_step_fused(&swf, &params);
        }

        EcsWorld w3, wf;
        ecs_world_init(&w3, &arena, n + 16u);
        ecs_world_init(&wf, &arena, n + 16u);
        /* sw3 ve swf'yi EcsWorld'deki değerlerle doğrudan karşılaştır */
        for (u32 a = 0u; a < sw3.arch_count; a++) {
            const SpecArchetype* a3 = &sw3.archetypes[a];
            const SpecArchetype* af = &swf.archetypes[a];
            CHECK(a3->layout.count == af->layout.count);
            CHECK(memcmp(a3->data, af->data, a3->layout.total_bytes) == 0);

            const f32* px = (const f32*)(a3->data + a3->layout.off_p[0]);
            const f32* py = (const f32*)(a3->data + a3->layout.off_p[1]);
            const f32* pz = (const f32*)(a3->data + a3->layout.off_p[2]);
            const f32* vx = (const f32*)(a3->data + a3->layout.off_v[0]);
            const f32* vy = (const f32*)(a3->data + a3->layout.off_v[1]);
            const f32* vz = (const f32*)(a3->data + a3->layout.off_v[2]);

            for (u32 i = 0u; i < a3->layout.count; i++) {
                const SpecPos* ep = (const SpecPos*)ecs_get_const(&w, a3->entities[i], cid_pos);
                const SpecVel* ev = (const SpecVel*)ecs_get_const(&w, a3->entities[i], cid_vel);
                CHECK(ep != NULL && ev != NULL);
                if (ep && ev) {
                    CHECK(f32_bits(ep->x)  == f32_bits(px[i]));
                    CHECK(f32_bits(ep->y)  == f32_bits(py[i]));
                    CHECK(f32_bits(ep->z)  == f32_bits(pz[i]));
                    CHECK(f32_bits(ev->vx) == f32_bits(vx[i]));
                    CHECK(f32_bits(ev->vy) == f32_bits(vy[i]));
                    CHECK(f32_bits(ev->vz) == f32_bits(vz[i]));
                }
            }
        }
        (void)w3; (void)wf;
        arena_pop_to(&arena, mark);
    }

    /* Host codegen hızı ölçümü (3 arketip x 4 çekirdek = 12 wasm modülü üretimi) */
    SpecArchLayout lays[3];
    spec_arch_layout_init(&lays[0], 0x3u,  50000u, 0u);
    spec_arch_layout_init(&lays[1], 0x7u,  30000u, 1u);
    spec_arch_layout_init(&lays[2], 0xFu,  20000u, 2u);
    u8 out_wasm[8192];
    u32 total_bytes = 0u;
    f64 t0 = plat_time_ms();
    for (u32 it = 0u; it < 100u; it++) {
        total_bytes = 0u;
        for (u32 a = 0u; a < 3u; a++) {
            for (u32 k = 0u; k < 4u; k++) {
                total_bytes += spec_codegen_kernel((SpecKernelKind)k, &lays[a], &params,
                                                   TRUE, FALSE, &arena, out_wasm, sizeof(out_wasm));
            }
        }
    }
    f64 cg_12_ms = (plat_time_ms() - t0) / 100.0;
    printf("  [bench F0_B host codegen] 12 wasm modulu (3 arketip x 4 cekirdek, %u B): %.4f ms (%.4f ms/modul)\n",
           total_bytes, cg_12_ms, cg_12_ms / 12.0);
    CHECK(total_bytes > 4000u);
}

/* 4. Node/V8 üzerinde gerçek JIT yükleme + 100k varlık bit-bit doğrulama ve hızlanma ölçümü */
TEST(spec_wasm_v8_jit_bit_exact_and_bench) {
    ensure_spec_test_dir();

    const char* runner_path = "build/test_spec/run_spec.mjs";
    FILE* fp = fopen(runner_path, "wb");
    CHECK(fp != NULL);
    if (!fp) return;

    const char* mjs_src =
        "import fs from 'node:fs';\n"
        "import { performance } from 'node:perf_hooks';\n"
        "import { createJitLoader } from '../../web/jit.js';\n"
        "const decoder = new TextDecoder('utf-8');\n"
        "const wasmBytes = fs.readFileSync('build/engine.wasm');\n"
        "const mod = new WebAssembly.Module(wasmBytes);\n"
        "let initialTable = 64;\n"
        "for (const imp of WebAssembly.Module.imports(mod)) {\n"
        "  if (imp.module === 'env' && imp.name === '__indirect_function_table' && imp.kind === 'table') {\n"
        "    const min = imp.type?.minimum ?? imp.type?.initial ?? 64;\n"
        "    initialTable = Math.max(initialTable, min);\n"
        "  }\n"
        "}\n"
        "const table = new WebAssembly.Table({ element: 'anyfunc', initial: initialTable });\n"
        "const memory = new WebAssembly.Memory({ initial: 4096, maximum: 16384, shared: true });\n"
        "function readCString(ptr, len) {\n"
        "  return decoder.decode(new Uint8Array(memory.buffer, ptr, len).slice());\n"
        "}\n"
        "const instance = new WebAssembly.Instance(mod, {\n"
        "  env: {\n"
        "    memory,\n"
        "    __indirect_function_table: table,\n"
        "    plat_log(ptr, len) { console.log('  ' + readCString(ptr, len)); },\n"
        "    plat_panic(ptr, len) { console.error('[PANIC] ' + readCString(ptr, len)); process.exit(1); },\n"
        "    plat_time_ms() { return performance.now(); },\n"
        "    plat_jit_load: createJitLoader(memory, table),\n"
        "    plat_download() {},\n"
        "  }\n"
        "});\n"
        "const rcVerify = instance.exports.engine_spec_verify();\n"
        "if (rcVerify !== 0) { console.error('engine_spec_verify FAIL:', rcVerify); process.exit(1); }\n"
        "const rcBench = instance.exports.engine_spec_bench_run(100000);\n"
        "if (rcBench !== 0) { console.error('engine_spec_bench_run FAIL:', rcBench); process.exit(1); }\n";

    fwrite(mjs_src, 1, strlen(mjs_src), fp);
    fclose(fp);

    int rc = system("node build/test_spec/run_spec.mjs");
    CHECK(rc == 0);
}
