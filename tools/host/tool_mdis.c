/* tools/host/tool_mdis.c — Komut satırı WebAssembly çözücüsü (disassembler).
 * Kullanım: mdis in.wasm [-o out.wat]
 * src/masm/mdis.c modülünü native olarak çalıştırır (tools/host altında libc serbest).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "masm/mdis.h"

int main(int argc, char** argv) {
    const char* in_path = NULL;
    const char* out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mdis: missing filename after -o\n");
                return 1;
            }
            out_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "mdis: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            in_path = argv[i];
        }
    }

    if (!in_path) {
        fprintf(stderr, "Usage: %s <in.wasm> [-o <out.wat>]\n", argv[0]);
        return 1;
    }

    FILE* fp = fopen(in_path, "rb");
    if (!fp) {
        fprintf(stderr, "mdis: cannot open input file '%s'\n", in_path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return 1;
    }

    u8* wasm = (u8*)malloc((size_t)sz + 1);
    if (!wasm) {
        fclose(fp);
        return 1;
    }
    size_t nread = fread(wasm, 1, (size_t)sz, fp);
    fclose(fp);

    size_t arena_cap = 4 * 1024 * 1024;
    void* arena_mem = malloc(arena_cap);
    Arena arena;
    arena_init(&arena, arena_mem, arena_cap);

    MdisResult res = mdis_disassemble(&arena, wasm, (u32)nread);
    if (!res.ok) {
        fprintf(stderr, "%s: %s\n", in_path, res.msg);
        free(arena_mem);
        free(wasm);
        return 1;
    }

    if (out_path) {
        FILE* out_fp = fopen(out_path, "wb");
        if (!out_fp) {
            fprintf(stderr, "mdis: cannot open output file '%s'\n", out_path);
            free(arena_mem);
            free(wasm);
            return 1;
        }
        fwrite(res.wat, 1, res.len, out_fp);
        fclose(out_fp);
    } else {
        fwrite(res.wat, 1, res.len, stdout);
    }

    free(arena_mem);
    free(wasm);
    return 0;
}
