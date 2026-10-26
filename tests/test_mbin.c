/* tests/test_mbin.c — mbin (WebAssembly binary üretici) birim ve entegrasyon testleri.
 * LEB128 sınır değerlerini, bölüm (section) üretimini, wasm-validate doğrulamasını
 * ve wasm-interp --run-all-exports ile çalıştırma sonuçlarını denetler.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "test.h"
#include "mbin/mbin.h"
#include "mbin/mbin_ops.h"

static void ensure_test_out_dir(void) {
    mkdir("build", 0755);
    mkdir("build/test_out", 0755);
}

static b32 write_wasm_file(const char* path, const u8* data, u32 len) {
    ensure_test_out_dir();
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;
    size_t nw = fwrite(data, 1, len, fp);
    fclose(fp);
    return nw == (size_t)len;
}

static b32 validate_wasm_file(const char* path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wasm-validate --enable-all %s", path);
    int rc = system(cmd);
    return rc == 0;
}

static b32 run_wasm_interp(const char* path, char* out_buf, size_t out_cap) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wasm-interp --enable-all --run-all-exports %s 2>&1", path);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return FALSE;
    size_t total = 0;
    out_buf[0] = '\0';
    while (total + 1 < out_cap) {
        size_t nr = fread(out_buf + total, 1, out_cap - 1 - total, pipe);
        if (nr == 0) break;
        total += nr;
    }
    out_buf[total] = '\0';
    int rc = pclose(pipe);
    return rc == 0;
}

TEST(mbin_leb128_boundaries) {
    u8 mem[256];
    MbBuf b;

    /* ULEB128 sınır değerleri: 0, 127, 128, 624485, U32_MAX, U64_MAX */
    const u64 uleb_cases[] = {
        0ull,
        1ull,
        127ull,
        128ull,
        255ull,
        16383ull,
        16384ull,
        624485ull,
        0xFFFFFFFFull,
        0xFFFFFFFFFFFFFFFFull
    };

    for (usize i = 0; i < ARRAY_LEN(uleb_cases); i++) {
        mb_buf_init(&b, mem, sizeof(mem));
        mb_uleb(&b, uleb_cases[i]);
        u32 nread = 0;
        u64 decoded = mb_decode_uleb(b.p, b.len, &nread);
        CHECK(nread == b.len);
        CHECK(decoded == uleb_cases[i]);
    }

    /* Bilinen kanonik ULEB128 byte dizileri */
    mb_buf_init(&b, mem, sizeof(mem));
    mb_uleb(&b, 0ull);
    CHECK(b.len == 1 && b.p[0] == 0x00u);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_uleb(&b, 127ull);
    CHECK(b.len == 1 && b.p[0] == 0x7Fu);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_uleb(&b, 128ull);
    CHECK(b.len == 2 && b.p[0] == 0x80u && b.p[1] == 0x01u);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_uleb(&b, 624485ull);
    CHECK(b.len == 3 && b.p[0] == 0xE5u && b.p[1] == 0x8Eu && b.p[2] == 0x26u);

    /* SLEB128 sınır değerleri: 0, 63, 64, 127, 128, -1, -64, -65, -128, -129, i64 min/max */
    const i64 i64_min = (i64)0x8000000000000000ull;
    const i64 i64_max = (i64)0x7FFFFFFFFFFFFFFFull;
    const i64 sleb_cases[] = {
        0ll,
        1ll,
        63ll,
        64ll,
        127ll,
        128ll,
        624485ll,
        -1ll,
        -2ll,
        -64ll,
        -65ll,
        -127ll,
        -128ll,
        -129ll,
        -123456ll,
        (i64)I32_MIN,
        (i64)I32_MAX,
        i64_min,
        i64_max
    };

    for (usize i = 0; i < ARRAY_LEN(sleb_cases); i++) {
        mb_buf_init(&b, mem, sizeof(mem));
        mb_sleb(&b, sleb_cases[i]);
        u32 nread = 0;
        i64 decoded = mb_decode_sleb(b.p, b.len, &nread);
        CHECK(nread == b.len);
        CHECK(decoded == sleb_cases[i]);
    }

    /* Bilinen kanonik SLEB128 byte dizileri */
    mb_buf_init(&b, mem, sizeof(mem));
    mb_sleb(&b, -1ll);
    CHECK(b.len == 1 && b.p[0] == 0x7Fu);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_sleb(&b, -64ll);
    CHECK(b.len == 1 && b.p[0] == 0x40u);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_sleb(&b, -65ll);
    CHECK(b.len == 2 && b.p[0] == 0xBFu && b.p[1] == 0x7Fu);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_sleb(&b, i64_min);
    CHECK(b.len == 10);

    mb_buf_init(&b, mem, sizeof(mem));
    mb_sleb(&b, i64_max);
    CHECK(b.len == 10);
}

TEST(mbin_add_module) {
    u8 work_mem[16384];
    u8 out_mem[4096];
    MbModule m;
    MbBuf out;

    /* 1. Saf add(i32, i32) -> i32 modülü (boyut ve hex dökümü doğrulaması için) */
    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    const u8 add_params[2] = { MB_VAL_I32, MB_VAL_I32 };
    const u8 add_results[1] = { MB_VAL_I32 };
    u32 t_add = mb_type(&m, add_params, 2, add_results, 1);
    /* İmza tekilleştirme testi */
    u32 t_add_dup = mb_type(&m, add_params, 2, add_results, 1);
    CHECK(t_add == t_add_dup);
    CHECK(m.type_count == 1);

    MbFunc* f_pure = mb_func_begin(&m, t_add);
    mb_local_get(f_pure, 0);
    mb_local_get(f_pure, 1);
    mb_i32_add(f_pure);
    mb_func_end(f_pure);
    mb_export_func(&m, "add", f_pure->func_idx);

    u32 pure_len = mb_module_finish(&m, &out);
    CHECK(pure_len == 41);
    CHECK(write_wasm_file("build/test_out/add_pure.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/add_pure.wasm"));

    /* 2. wasm-interp --run-all-exports için parametresiz sarmalayıcı içeren add modülü */
    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    t_add = mb_type(&m, add_params, 2, add_results, 1);
    u32 t_run = mb_type(&m, NULL, 0, add_results, 1);

    MbFunc* f_add = mb_func_begin(&m, t_add);
    mb_local_get(f_add, 0);
    mb_local_get(f_add, 1);
    mb_i32_add(f_add);
    mb_func_end(f_add);

    MbFunc* f_test = mb_func_begin(&m, t_run);
    mb_i32_const(f_test, 2);
    mb_i32_const(f_test, 3);
    mb_call(f_test, f_add->func_idx);
    mb_func_end(f_test);

    mb_export_func(&m, "add", f_add->func_idx);
    mb_export_func(&m, "test_add", f_test->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/add.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/add.wasm"));

    char interp_out[512];
    CHECK(run_wasm_interp("build/test_out/add.wasm", interp_out, sizeof(interp_out)));
    CHECK(strstr(interp_out, "test_add() => i32:5") != NULL);
}

TEST(mbin_fact_loop_module) {
    u8 work_mem[16384];
    u8 out_mem[4096];
    MbModule m;
    MbBuf out;

    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    const u8 fact_params[1] = { MB_VAL_I32 };
    const u8 fact_results[1] = { MB_VAL_I64 };
    u32 t_fact = mb_type(&m, fact_params, 1, fact_results, 1);
    u32 t_run  = mb_type(&m, NULL, 0, fact_results, 1);

    /* fn fact(n: i32) -> i64:
     *   local acc: i64 = 1
     *   block $exit
     *     loop $top
     *       if n <= 1 -> br $exit
     *       acc = acc * i64.extend_i32_s(n)
     *       n = n - 1
     *       br $top
     *     end
     *   end
     *   return acc
     */
    MbFunc* f_fact = mb_func_begin(&m, t_fact);
    u32 loc_acc = mb_local(f_fact, MB_VAL_I64, 1);
    CHECK(loc_acc == 1);

    mb_i64_const(f_fact, 1);
    mb_local_set(f_fact, loc_acc);

    mb_block(f_fact, MB_BLOCK_VOID);
    mb_loop(f_fact, MB_BLOCK_VOID);

    mb_local_get(f_fact, 0);
    mb_i32_const(f_fact, 1);
    mb_i32_le_s(f_fact);
    mb_br_if(f_fact, 1); /* $exit bloğuna çık */

    mb_local_get(f_fact, loc_acc);
    mb_local_get(f_fact, 0);
    mb_i64_extend_i32_s(f_fact);
    mb_i64_mul(f_fact);
    mb_local_set(f_fact, loc_acc);

    mb_local_get(f_fact, 0);
    mb_i32_const(f_fact, 1);
    mb_i32_sub(f_fact);
    mb_local_set(f_fact, 0);

    mb_br(f_fact, 0); /* $top döngüsüne devam et */
    mb_end(f_fact);   /* end loop */
    mb_end(f_fact);   /* end block */

    mb_local_get(f_fact, loc_acc);
    mb_func_end(f_fact);

    /* test_fact() -> fact(10) = 3628800 */
    MbFunc* f_test = mb_func_begin(&m, t_run);
    mb_i32_const(f_test, 10);
    mb_call(f_test, f_fact->func_idx);
    mb_func_end(f_test);

    mb_export_func(&m, "fact", f_fact->func_idx);
    mb_export_func(&m, "test_fact", f_test->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/fact.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/fact.wasm"));

    char interp_out[512];
    CHECK(run_wasm_interp("build/test_out/fact.wasm", interp_out, sizeof(interp_out)));
    CHECK(strstr(interp_out, "test_fact() => i64:3628800") != NULL);
}

TEST(mbin_simd_module) {
    u8 work_mem[16384];
    u8 out_mem[4096];
    MbModule m;
    MbBuf out;

    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    const u8 f32_res[1] = { MB_VAL_F32 };
    const u8 i32_res[1] = { MB_VAL_I32 };
    u32 t_f32 = mb_type(&m, NULL, 0, f32_res, 1);
    u32 t_i32 = mb_type(&m, NULL, 0, i32_res, 1);

    /* 1. İki f32x4.const topla, f32x4.extract_lane 2 döndür:
     *    (1.0, 2.0, 3.5, 4.0) + (10.0, 20.0, 30.25, 40.0) -> lane 2 = 33.75
     */
    MbFunc* f_simd_add = mb_func_begin(&m, t_f32);
    mb_f32x4_const(f_simd_add, 1.0f, 2.0f, 3.5f, 4.0f);
    mb_f32x4_const(f_simd_add, 10.0f, 20.0f, 30.25f, 40.0f);
    mb_f32x4_add(f_simd_add);
    mb_f32x4_extract_lane(f_simd_add, 2);
    mb_func_end(f_simd_add);

    /* 2. Ek SIMD komutlarını (splat, mul, sub, div, min, max, sqrt, cmp, bitselect,
     *    shuffle, trunc_sat, convert, narrow, any_true, all_true) doğrulayan fonksiyon
     */
    MbFunc* f_simd_ops = mb_func_begin(&m, t_i32);
    u32 v_loc = mb_local(f_simd_ops, MB_VAL_V128, 2);
    mb_f32_const(f_simd_ops, 4.0f);
    mb_f32x4_splat(f_simd_ops);
    mb_f32x4_sqrt(f_simd_ops); /* (2, 2, 2, 2) */
    mb_local_tee(f_simd_ops, v_loc);
    mb_f32x4_const(f_simd_ops, 1.0f, 5.0f, 2.0f, 8.0f);
    mb_local_tee(f_simd_ops, v_loc + 1);
    mb_f32x4_lt(f_simd_ops); /* mask: (0, -1, 0, -1) */
    mb_local_get(f_simd_ops, v_loc + 1);
    mb_local_get(f_simd_ops, v_loc);
    mb_v128_bitselect(f_simd_ops); /* her lane'de max: (2, 5, 2, 8) */
    mb_i32x4_trunc_sat_f32x4_s(f_simd_ops);
    const u8 rev_lanes[16] = {
        12, 13, 14, 15,
        8,  9,  10, 11,
        4,  5,  6,  7,
        0,  1,  2,  3
    };
    mb_local_tee(f_simd_ops, v_loc);
    mb_local_get(f_simd_ops, v_loc);
    mb_i8x16_shuffle(f_simd_ops, rev_lanes); /* (8, 2, 5, 2) */
    mb_i32x4_extract_lane(f_simd_ops, 0);    /* 8 */
    mb_func_end(f_simd_ops);

    mb_export_func(&m, "test_simd_add", f_simd_add->func_idx);
    mb_export_func(&m, "test_simd_ops", f_simd_ops->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/simd.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/simd.wasm"));

    char interp_out[512];
    CHECK(run_wasm_interp("build/test_out/simd.wasm", interp_out, sizeof(interp_out)));
    CHECK(strstr(interp_out, "test_simd_add() => f32:33.750000") != NULL);
    CHECK(strstr(interp_out, "test_simd_ops() => i32:8") != NULL);
}

TEST(mbin_memory_import_and_rw) {
    u8 work_mem[16384];
    u8 out_mem[4096];
    MbModule m;
    MbBuf out;

    /* 1. env.memory ve env.__indirect_function_table import eden modül (JIT ABI şablonu) */
    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    const u8 i32_res[1] = { MB_VAL_I32 };
    u32 t_run = mb_type(&m, NULL, 0, i32_res, 1);

    mb_import_memory(&m, "env", "memory", 1, 16, FALSE);
    mb_import_table(&m, "env", "__indirect_function_table", 1);

    MbFunc* f_mem = mb_func_begin(&m, t_run);
    /* Adres 64'e i32 (123456) yaz ve geri oku */
    mb_i32_const(f_mem, 64);
    mb_i32_const(f_mem, 123456);
    mb_i32_store(f_mem, 2, 0);

    /* Adres 128'e v128 yaz, yükle ve lane 1'i adres 64 ile topla */
    mb_i32_const(f_mem, 128);
    mb_i32x4_const(f_mem, 10, 20, 30, 40);
    mb_v128_store(f_mem, 4, 0);

    mb_i32_const(f_mem, 64);
    mb_i32_load(f_mem, 2, 0);
    mb_i32_const(f_mem, 128);
    mb_v128_load(f_mem, 4, 0);
    mb_i32x4_extract_lane(f_mem, 1);
    mb_i32_add(f_mem);
    mb_func_end(f_mem);

    mb_export_func(&m, "test_mem_import", f_mem->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/mem_import.wasm", out.p, out.len));
    /* wasm-interp dışarıdan "env.memory" import'unu sağlamadığı için wasm-validate ile doğrulanır */
    CHECK(validate_wasm_file("build/test_out/mem_import.wasm"));

    /* 2. Dahili bellek (Section 5: Memory) ile aynı bellek yazma/okuma mantığını wasm-interp'te çalıştır */
    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    t_run = mb_type(&m, NULL, 0, i32_res, 1);
    mb_memory(&m, 1, 16, FALSE);

    MbFunc* f_mem_run = mb_func_begin(&m, t_run);
    mb_i32_const(f_mem_run, 64);
    mb_i32_const(f_mem_run, 100);
    mb_i32_store(f_mem_run, 2, 0);
    mb_i32_const(f_mem_run, 64);
    mb_i32_load(f_mem_run, 2, 0);
    mb_func_end(f_mem_run);

    mb_export_func(&m, "test_mem_rw", f_mem_run->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/mem_internal.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/mem_internal.wasm"));

    char interp_out[512];
    CHECK(run_wasm_interp("build/test_out/mem_internal.wasm", interp_out, sizeof(interp_out)));
    CHECK(strstr(interp_out, "test_mem_rw() => i32:100") != NULL);
}

TEST(mbin_all_opcodes_validate) {
    u8 work_mem[32768];
    u8 out_mem[8192];
    MbModule m;
    MbBuf out;

    mb_module_init(&m, work_mem, sizeof(work_mem));
    mb_buf_init(&out, out_mem, sizeof(out_mem));

    const u8 i32_res[1] = { MB_VAL_I32 };
    u32 t_fn = mb_type(&m, NULL, 0, i32_res, 1);

    mb_import_memory(&m, "env", "memory", 1, 16, TRUE);
    mb_import_table(&m, "env", "__indirect_function_table", 4);

    MbFunc* f = mb_func_begin(&m, t_fn);
    u32 l_i32 = mb_local(f, MB_VAL_I32, 1);
    u32 l_i64 = mb_local(f, MB_VAL_I64, 1);
    u32 l_f32 = mb_local(f, MB_VAL_F32, 1);
    u32 l_v128 = mb_local(f, MB_VAL_V128, 1);
    UNUSED(l_i64);
    UNUSED(l_f32);

    /* Skaler aritmetik, karşılaştırma, dönüşüm ve kontrol akışı */
    mb_nop(f);
    mb_i32_const(f, 10);
    mb_i32_const(f, 3);
    mb_i32_sub(f);
    mb_i32_const(f, 2);
    mb_i32_mul(f);
    mb_i32_const(f, 7);
    mb_i32_div_s(f);
    mb_i32_const(f, 5);
    mb_i32_or(f);
    mb_i32_const(f, 3);
    mb_i32_and(f);
    mb_i32_const(f, 1);
    mb_i32_xor(f);
    mb_i32_const(f, 1);
    mb_i32_shl(f);
    mb_i32_const(f, 1);
    mb_i32_shr_s(f);
    mb_i32_const(f, 1);
    mb_i32_shr_u(f);
    mb_local_set(f, l_i32);

    /* if / else / end */
    mb_local_get(f, l_i32);
    mb_if(f, MB_VAL_I32);
    mb_i32_const(f, 1);
    mb_else(f);
    mb_i32_const(f, 0);
    mb_end(f);
    mb_drop(f);

    /* Bellek alt-byte/halfword yükleme ve saklama */
    mb_i32_const(f, 0);
    mb_i32_const(f, 0xFF);
    mb_i32_store8(f, 0, 0);
    mb_i32_const(f, 0);
    mb_i32_load8_u(f, 0, 0);
    mb_drop(f);
    mb_i32_const(f, 0);
    mb_i32_const(f, 0x1234);
    mb_i32_store16(f, 1, 2);
    mb_i32_const(f, 0);
    mb_i32_load16_u(f, 1, 2);
    mb_drop(f);

    /* f32 ve doygun (saturating) dönüşüm */
    mb_f32_const(f, 12.75f);
    mb_f32_abs(f);
    mb_f32_floor(f);
    mb_i32_trunc_sat_f32_s(f);
    mb_drop(f);

    /* SIMD: v128 mantıksal, daraltma (narrow), karşılaştırma ve aritmetik */
    mb_i32_const(f, 3);
    mb_i32x4_splat(f);
    mb_local_tee(f, l_v128);
    mb_local_get(f, l_v128);
    mb_i32x4_add(f);
    mb_local_get(f, l_v128);
    mb_i32x4_sub(f);
    mb_local_get(f, l_v128);
    mb_i32x4_mul(f);
    mb_i32_const(f, 1);
    mb_i32x4_shl(f);
    mb_i32_const(f, 1);
    mb_i32x4_shr_s(f);
    mb_i32_const(f, 1);
    mb_i32x4_shr_u(f);
    mb_f32x4_convert_i32x4_s(f);
    mb_local_get(f, l_v128);
    mb_f32x4_convert_i32x4_s(f);
    mb_f32x4_sub(f);
    mb_f32x4_const(f, 1.0f, 2.0f, 3.0f, 4.0f);
    mb_f32x4_mul(f);
    mb_f32x4_const(f, 1.0f, 1.0f, 1.0f, 1.0f);
    mb_f32x4_div(f);
    mb_f32x4_const(f, 0.0f, 0.0f, 0.0f, 0.0f);
    mb_f32x4_max(f);
    mb_f32x4_const(f, 10.0f, 10.0f, 10.0f, 10.0f);
    mb_f32x4_min(f);
    mb_f32_const(f, 9.0f);
    mb_f32x4_replace_lane(f, 0);
    mb_local_tee(f, l_v128);
    mb_local_get(f, l_v128);
    mb_f32x4_eq(f);
    mb_local_get(f, l_v128);
    mb_local_get(f, l_v128);
    mb_f32x4_le(f);
    mb_v128_and(f);
    mb_local_get(f, l_v128);
    mb_local_get(f, l_v128);
    mb_f32x4_gt(f);
    mb_v128_or(f);
    mb_local_get(f, l_v128);
    mb_local_get(f, l_v128);
    mb_f32x4_ge(f);
    mb_v128_xor(f);
    mb_v128_not(f);
    mb_local_get(f, l_v128);
    mb_v128_andnot(f);
    mb_local_get(f, l_v128);
    mb_i16x8_narrow_i32x4_u(f);
    mb_local_get(f, l_v128);
    mb_i8x16_narrow_i16x8_u(f);
    mb_local_tee(f, l_v128);
    mb_v128_any_true(f);
    mb_drop(f);
    mb_local_get(f, l_v128);
    mb_i32x4_all_true(f);

    mb_func_end(f);
    mb_export_func(&m, "all_ops", f->func_idx);

    mb_module_finish(&m, &out);
    CHECK(write_wasm_file("build/test_out/all_ops.wasm", out.p, out.len));
    CHECK(validate_wasm_file("build/test_out/all_ops.wasm"));
}
