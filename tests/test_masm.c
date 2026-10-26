/* tests/test_masm.c — masm (WAT assembler) ve mdis (disassembler) kapsamlı testleri.
 * 1) Altın test: tests/wat/ altındaki .wat dosyaları (25+) için masm çıktısının
 *    wat2wasm çıktısı ile byte-byte birebir aynı olduğunu doğrular.
 * 2) Gidiş-dönüş (round-trip): masm(mdis(x)) == x özelliğini tüm test dosyaları ve
 *    build/test_out/ altındaki .wasm modülleri üzerinde denetler.
 * 3) Hata durumları: 15+ bozuk WAT girdisinde doğru satır:sütun bilgisini doğrular.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "test.h"
#include "masm/masm.h"
#include "masm/mdis.h"

static u8 g_test_arena_mem[8 * 1024 * 1024];

static b32 read_whole_file(const char* path, u8** out_buf, u32* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return FALSE;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return FALSE;
    }
    u8* buf = (u8*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return FALSE;
    }
    size_t nr = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[nr] = 0;
    *out_buf = buf;
    *out_len = (u32)nr;
    return TRUE;
}

static b32 write_whole_file(const char* path, const u8* buf, u32 len) {
    mkdir("build", 0755);
    mkdir("build/test_out", 0755);
    FILE* fp = fopen(path, "wb");
    if (!fp) return FALSE;
    size_t nw = fwrite(buf, 1, len, fp);
    fclose(fp);
    return nw == (size_t)len;
}

static int cmp_str_ptr(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

TEST(masm_golden_wat2wasm_and_roundtrip) {
    DIR* dir = opendir("tests/wat");
    CHECK(dir != NULL);
    if (!dir) return;

    char* files[128];
    u32 file_count = 0;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".wat") == 0) {
            if (file_count < 128) {
                char path[512];
                snprintf(path, sizeof(path), "tests/wat/%s", ent->d_name);
                size_t plen = strlen(path);
                files[file_count] = (char*)malloc(plen + 1);
                memcpy(files[file_count], path, plen + 1);
                file_count++;
            }
        }
    }
    closedir(dir);

    qsort(files, file_count, sizeof(char*), cmp_str_ptr);
    CHECK(file_count >= 31);

    Arena arena;
    arena_init(&arena, g_test_arena_mem, sizeof(g_test_arena_mem));

    for (u32 i = 0; i < file_count; i++) {
        const char* wat_path = files[i];
        u8* wat_src = NULL;
        u32 wat_len = 0;
        CHECK(read_whole_file(wat_path, &wat_src, &wat_len));
        if (!wat_src) continue;

        arena_reset(&arena);
        MasmResult mr = masm_assemble(&arena, (const char*)wat_src, wat_len);
        if (!mr.ok) {
            fprintf(stderr, "[masm golden] %s failed: %s\n", wat_path, mr.msg);
        }
        CHECK(mr.ok);

        if (mr.ok) {
            char ref_wasm_path[512];
            char our_wasm_path[512];
            snprintf(ref_wasm_path, sizeof(ref_wasm_path), "build/test_out/ref_%u.wasm", i);
            snprintf(our_wasm_path, sizeof(our_wasm_path), "build/test_out/our_%u.wasm", i);

            CHECK(write_whole_file(our_wasm_path, mr.wasm, mr.len));

            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "wat2wasm %s -o %s", wat_path, ref_wasm_path);
            int rc = system(cmd);
            CHECK(rc == 0);

            snprintf(cmd, sizeof(cmd), "wasm-validate %s", our_wasm_path);
            CHECK(system(cmd) == 0);

            u8* ref_wasm = NULL;
            u32 ref_len = 0;
            CHECK(read_whole_file(ref_wasm_path, &ref_wasm, &ref_len));
            if (ref_wasm) {
                b32 exact_match = (mr.len == ref_len) && (memcmp(mr.wasm, ref_wasm, ref_len) == 0);
                if (!exact_match) {
                    fprintf(stderr, "[masm golden] byte mismatch on %s (our=%u, ref=%u)\n",
                            wat_path, mr.len, ref_len);
                }
                CHECK(exact_match);
                free(ref_wasm);
            }
            remove(ref_wasm_path);
            remove(our_wasm_path);

            /* Gidiş-dönüş (Round-trip): masm(mdis(x)) == x */
            MdisResult dr = mdis_disassemble(&arena, mr.wasm, mr.len);
            if (!dr.ok) {
                fprintf(stderr, "[mdis roundtrip] %s disassemble failed: %s\n", wat_path, dr.msg);
            }
            CHECK(dr.ok);
            if (dr.ok) {
                MasmResult rt = masm_assemble(&arena, dr.wat, dr.len);
                if (!rt.ok) {
                    fprintf(stderr, "[mdis roundtrip] %s re-assemble failed: %s\nWAT:\n%s\n",
                            wat_path, rt.msg, dr.wat);
                }
                CHECK(rt.ok);
                if (rt.ok) {
                    b32 rt_match = (rt.len == mr.len) && (memcmp(rt.wasm, mr.wasm, mr.len) == 0);
                    if (!rt_match) {
                        fprintf(stderr, "[mdis roundtrip] binary mismatch on %s (orig=%u, rt=%u)\n",
                                wat_path, mr.len, rt.len);
                    }
                    CHECK(rt_match);
                }
            }
        }

        free(wat_src);
        free(files[i]);
    }
}

TEST(masm_roundtrip_test_out_modules) {
    mkdir("build", 0755);
    mkdir("build/test_out", 0755);
    DIR* dir = opendir("build/test_out");
    CHECK(dir != NULL);
    if (!dir) return;

    Arena arena;
    arena_init(&arena, g_test_arena_mem, sizeof(g_test_arena_mem));

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 5 || strcmp(ent->d_name + nlen - 5, ".wasm") != 0) continue;
        /* Geçici altın test çıktılarını atla, diğer hatların (mbin / matoki) ürettiği .wasm'ları test et */
        if (strncmp(ent->d_name, "ref_", 4) == 0 || strncmp(ent->d_name, "our_", 4) == 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "build/test_out/%s", ent->d_name);

        u8* wasm_bytes = NULL;
        u32 wasm_len = 0;
        CHECK(read_whole_file(path, &wasm_bytes, &wasm_len));
        if (!wasm_bytes) continue;

        arena_reset(&arena);
        MdisResult dr = mdis_disassemble(&arena, wasm_bytes, wasm_len);
        if (!dr.ok) {
            fprintf(stderr, "[roundtrip test_out] %s mdis failed: %s\n", path, dr.msg);
        }
        CHECK(dr.ok);
        if (dr.ok) {
            MasmResult rt = masm_assemble(&arena, dr.wat, dr.len);
            if (!rt.ok) {
                fprintf(stderr, "[roundtrip test_out] %s masm failed: %s\n", path, rt.msg);
            }
            CHECK(rt.ok);
            if (rt.ok) {
                CHECK(rt.len == wasm_len);
                CHECK(memcmp(rt.wasm, wasm_bytes, wasm_len) == 0);
            }
        }
        free(wasm_bytes);
    }
    closedir(dir);
}

typedef struct {
    const char* name;
    const char* src;
    u32         exp_line;
    u32         exp_col;
} MasmErrCase;

TEST(masm_error_diagnostics_line_col) {
    static const MasmErrCase cases[] = {
        /* 1. Eksik '(' başlangıcı */
        { "missing_open_paren",
          "module)", 1, 1 },
        /* 2. Yanlış üst seviye anahtar sözcük */
        { "bad_module_kw",
          "(mod)", 1, 2 },
        /* 3. Kapatılmamış blok yorumu */
        { "unterminated_block_comment",
          "(module\n  (; açık yorum\n)", 2, 3 },
        /* 4. Kapatılmamış dizge sabiti */
        { "unterminated_string",
          "(module\n  (export \"acik (func 0))\n)", 2, 26 },
        /* 5. Geçersiz kaçış dizisi */
        { "invalid_string_escape",
          "(module\n  (export \"\\zz\" (func 0))\n)", 2, 12 },
        /* 6. Bilinmeyen modül alanı */
        { "unknown_module_field",
          "(module\n  (foobar 123)\n)", 2, 4 },
        /* 7. Tanımsız fonksiyonktan sonra gelen import */
        { "import_after_func",
          "(module\n  (func $f)\n  (import \"a\" \"b\" (func $g))\n)", 3, 4 },
        /* 8. (result) sonrasında (param) */
        { "param_after_result",
          "(module\n  (func (result i32) (param i32)\n    i32.const 0)\n)", 2, 23 },
        /* 9. Bilinmeyen değer tipi */
        { "invalid_valtype",
          "(module\n  (func (param i128))\n)", 2, 16 },
        /* 10. Mükerrer fonksiyon adı */
        { "duplicate_func_id",
          "(module\n  (func $dup)\n  (func $dup)\n)", 3, 9 },
        /* 11. Mükerrer yerel değişken adı */
        { "duplicate_local_id",
          "(module\n  (func (param $x i32) (local $x i32))\n)", 2, 31 },
        /* 12. Tanımsız yerel değişken ($y) */
        { "undefined_local",
          "(module\n  (func (param $x i32) (result i32)\n    local.get $y)\n)", 3, 15 },
        /* 13. Sınır dışı yerel değişken indeksi */
        { "local_index_out_of_range",
          "(module\n  (func (param i32) (result i32)\n    local.get 5)\n)", 3, 15 },
        /* 14. Tanımsız etiket ($missing) */
        { "undefined_branch_label",
          "(module\n  (func\n    block $b\n      br $missing\n    end)\n)", 4, 10 },
        /* 15. Eşleşmeyen end etiketi */
        { "mismatching_end_label",
          "(module\n  (func\n    block $a\n    end $b)\n)", 4, 5 },
        /* 16. Bilinmeyen komut (opcode) */
        { "unknown_opcode",
          "(module\n  (func\n    i32.magic_op)\n)", 3, 5 },
        /* 17. Kuvveti olmayan bellek hizalaması (align=3) */
        { "non_pow2_align",
          "(module\n  (memory 1)\n  (func (param i32) (result i32)\n    local.get 0\n    i32.load align=3)\n)", 5, 14 },
        /* 18. SIMD lane indeksi sınır dışı (>= 4) */
        { "simd_lane_out_of_bounds",
          "(module\n  (func (param v128) (result i32)\n    local.get 0\n    i32x4.extract_lane 4)\n)", 4, 24 },
        /* 19. Kapatılmamış kontrol bloğu */
        { "unclosed_block_at_func_end",
          "(module\n  (func\n    block $b\n      nop)\n)", 4, 10 },
        /* 20. Tanımsız fonksiyon çağrısı */
        { "undefined_func_call",
          "(module\n  (func\n    call $yok)\n)", 3, 10 },
        /* 21. br_table hedef etiketi eksik */
        { "br_table_missing_targets",
          "(module\n  (func (param i32)\n    local.get 0\n    br_table\n  )\n)", 5, 3 },
        /* 22. v128.load64_lane sınır dışı lane indeksi (>= 2) */
        { "simd_mem_lane_out_of_bounds",
          "(module\n  (memory 1)\n  (func (param i32) (param v128) (result v128)\n    local.get 0\n    local.get 1\n    v128.load64_lane 2)\n)", 6, 22 },
    };

    Arena arena;
    arena_init(&arena, g_test_arena_mem, sizeof(g_test_arena_mem));

    for (usize i = 0; i < ARRAY_LEN(cases); i++) {
        arena_reset(&arena);
        MasmResult r = masm_assemble_cstr(&arena, cases[i].src);
        CHECK(!r.ok);
        if (r.ok || r.line != cases[i].exp_line || r.col != cases[i].exp_col) {
            fprintf(stderr, "[err_case %s] expected %u:%u, got ok=%u %u:%u (%s)\n",
                    cases[i].name, cases[i].exp_line, cases[i].exp_col,
                    r.ok, r.line, r.col, r.msg);
        }
        CHECK(r.line == cases[i].exp_line);
        CHECK(r.col == cases[i].exp_col);
    }
}
