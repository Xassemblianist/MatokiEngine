/* tools/host/tool_masm.c — Komut satırı WAT birleştiricisi (assembler).
 * Kullanım: masm in.wat -o out.wasm
 * src/masm/masm.c modülünü native olarak çalıştırır (tools/host altında libc serbest).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "masm/masm.h"

int main(int argc, char** argv) {
    const char* in_path = NULL;
    const char* out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "masm: missing filename after -o\n");
                return 1;
            }
            out_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "masm: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            in_path = argv[i];
        }
    }

    if (!in_path || !out_path) {
        fprintf(stderr, "Usage: %s <in.wat> -o <out.wasm>\n", argv[0]);
        return 1;
    }

    FILE* fp = fopen(in_path, "rb");
    if (!fp) {
        fprintf(stderr, "masm: cannot open input file '%s'\n", in_path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        fclose(fp);
        return 1;
    }

    char* src = (char*)malloc((size_t)sz + 1);
    if (!src) {
        fclose(fp);
        return 1;
    }
    size_t nread = fread(src, 1, (size_t)sz, fp);
    fclose(fp);
    src[nread] = '\0';

    size_t arena_cap = 4 * 1024 * 1024;
    void* arena_mem = malloc(arena_cap);
    Arena arena;
    arena_init(&arena, arena_mem, arena_cap);

    MasmResult res = masm_assemble(&arena, src, (u32)nread);
    if (!res.ok) {
        fprintf(stderr, "%s:%s\n", in_path, res.msg);
        free(arena_mem);
        free(src);
        return 1;
    }

    FILE* out_fp = fopen(out_path, "wb");
    if (!out_fp) {
        fprintf(stderr, "masm: cannot open output file '%s'\n", out_path);
        free(arena_mem);
        free(src);
        return 1;
    }
    size_t nwritten = fwrite(res.wasm, 1, res.len, out_fp);
    fclose(out_fp);

    free(arena_mem);
    free(src);
    return (nwritten == (size_t)res.len) ? 0 : 1;
}
