/* tools/host/tool_matc.c — Matoki (.mtk) komut satırı derleyicisi.
 * Kullanım: build/host/matc girdi.mtk [...] [-o cikti.wasm] [--ir] [--ir-opt] [--layout]
 * Birden fazla .mtk dosyasını aynı anda işleyebilir.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/types.h"
#include "core/arena.h"
#include "matoki/matoki.h"
#include "matoki/ir.h"

static u8 g_arena_mem[MB(16)];
static char g_ir_text[65536];
static u8 g_wasm_out[65536];

static char* read_file_bytes(const char* path, u32* out_len) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = (u32)rd;
    return buf;
}

static void print_layout(const char* path, const MtkLayout* layout) {
    printf("=== Layout: %s (uniform_bytes=%u, varyings=%u, textures=%u) ===\n",
           path, layout->uniform_bytes, layout->varying_count, layout->texture_count);
    for (u32 i = 0; i < layout->sym_count; i++) {
        const MtkSymbol* s = &layout->syms[i];
        const char* kind_str = (s->kind == MTK_SYM_UNIFORM) ? "uniform" :
                               (s->kind == MTK_SYM_VARYING) ? "varying" : "texture";
        printf("  %-8s %-16s type=%-10s offset/idx=%u comps=%u\n",
               kind_str, s->name, mtk_type_name((MtkType)s->type),
               s->offset_or_index, s->components);
    }
}

int main(int argc, char** argv) {
    const char* inputs[64];
    u32 input_count = 0;
    const char* out_path = NULL;
    b32 show_ir     = FALSE;
    b32 show_ir_opt = FALSE;
    b32 show_layout = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "matc: '-o' sonrasinda dosya yolu bekleniyordu\n");
                return 1;
            }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--ir") == 0) {
            show_ir = TRUE;
        } else if (strcmp(argv[i], "--ir-opt") == 0) {
            show_ir_opt = TRUE;
        } else if (strcmp(argv[i], "--layout") == 0) {
            show_layout = TRUE;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "matc: bilinmeyen secenek '%s'\n", argv[i]);
            return 1;
        } else {
            if (input_count < 64) {
                inputs[input_count++] = argv[i];
            }
        }
    }

    if (input_count == 0) {
        fprintf(stderr, "Kullanim: %s girdi.mtk [-o cikti.wasm] [--ir] [--ir-opt] [--layout]\n", argv[0]);
        return 1;
    }

    Arena arena;
    arena_init(&arena, g_arena_mem, sizeof(g_arena_mem));

    for (u32 idx = 0; idx < input_count; idx++) {
        const char* in_path = inputs[idx];
        u32 src_len = 0;
        char* src = read_file_bytes(in_path, &src_len);
        if (!src) {
            fprintf(stderr, "matc: dosya okunamadi: %s\n", in_path);
            return 1;
        }

        arena_reset(&arena);
        IrFunc ir;
        MtkLayout layout;
        MtkDiag diag;

        if (!mtk_compile_ir(src, src_len, in_path, &arena, &ir, &layout, &diag)) {
            fprintf(stderr, "%s", diag.text);
            free(src);
            return 1;
        }

        if (show_layout) {
            print_layout(in_path, &layout);
        }

        if (show_ir) {
            if (input_count > 1) {
                printf("; === %s (ham IR: %u komut) ===\n", in_path, ir.count);
            }
            ir_print(&ir, g_ir_text, sizeof(g_ir_text));
            printf("%s", g_ir_text);
        }

        if (show_ir_opt) {
            u32 before = ir.count;
            ir_optimize(&ir);
            if (input_count > 1) {
                printf("; === %s (opt IR: %u -> %u komut) ===\n", in_path, before, ir.count);
            }
            ir_print(&ir, g_ir_text, sizeof(g_ir_text));
            printf("%s", g_ir_text);
        }

        if (out_path || (!show_ir && !show_ir_opt && !show_layout)) {
            arena_reset(&arena);
            u32 wasm_len = mtk_compile_wasm(src, src_len, in_path, NULL, &arena,
                                            g_wasm_out, sizeof(g_wasm_out), &layout, &diag);
            if (wasm_len == 0) {
                fprintf(stderr, "%s", diag.text);
                free(src);
                return 1;
            }
            if (out_path) {
                FILE* out_fp = fopen(out_path, "wb");
                if (!out_fp) {
                    fprintf(stderr, "matc: cikti dosyasi yazilamadi: %s\n", out_path);
                    free(src);
                    return 1;
                }
                fwrite(g_wasm_out, 1, wasm_len, out_fp);
                fclose(out_fp);
            }
        }

        free(src);
    }

    return 0;
}
