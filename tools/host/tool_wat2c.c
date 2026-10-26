/* tools/host/tool_wat2c.c — asm/ altındaki .wat dosyalarını doğrulayıp src/bench/asm_sources.c'ye gömen araç.
 * Kullanım: ./build/host/wat2c [çıktı.c] [girdi1.wat girdi2.wat ...]
 * Argüman verilmezse varsayılan olarak asm/ altındaki .wat dosyalarını tarar, her birini masm_assemble
 * ile derleyerek sözdizimi/kodlama hatası olmadığını doğrular ve src/bench/asm_sources.c üretir.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "core/types.h"
#include "core/arena.h"
#include "masm/masm.h"
#include "masm/mdis.h"

static u8 g_tool_arena_mem[4 * 1024 * 1024];

static int cmp_str(const void* a, const void* b) {
    const char* sa = *(const char* const*)a;
    const char* sb = *(const char* const*)b;
    return strcmp(sa, sb);
}

static const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

int main(int argc, char** argv) {
    const char* out_path = "src/bench/asm_sources.c";
    char* files[64];
    u32 file_count = 0;
    b32 allocated_paths = FALSE;

    if (argc >= 3) {
        out_path = argv[1];
        for (int i = 2; i < argc && file_count < 64; i++) {
            files[file_count++] = argv[i];
        }
    } else {
        if (argc == 2) {
            out_path = argv[1];
        }
        DIR* dir = opendir("asm");
        if (!dir) {
            fprintf(stderr, "wat2c: 'asm/' dizini açılamadı\n");
            return 1;
        }
        allocated_paths = TRUE;
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcmp(ent->d_name + len - 4, ".wat") == 0 && file_count < 64) {
                char buf[512];
                snprintf(buf, sizeof(buf), "asm/%s", ent->d_name);
                files[file_count] = strdup(buf);
                file_count++;
            }
        }
        closedir(dir);
        qsort(files, file_count, sizeof(char*), cmp_str);
    }

    Arena arena;
    arena_init(&arena, g_tool_arena_mem, sizeof(g_tool_arena_mem));

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "wat2c: çıktı dosyası yazılamadı: %s\n", out_path);
        return 1;
    }

    fprintf(out,
        "/* bench/asm_sources.c — OTOMATİK ÜRETİLDİ (tools/host/tool_wat2c).\n"
        " * asm/ altındaki .wat dosyalarının gömülü WAT metinlerini ve arama yardımcısını içerir.\n"
        " * Çalışma anında masm_assemble ile WebAssembly ikilisine derlenir.\n"
        " */\n"
        "#include \"bench/asm_sources.h\"\n\n");

    u32 sizes[64];
    for (u32 i = 0; i < file_count; i++) {
        FILE* fp = fopen(files[i], "rb");
        if (!fp) {
            fprintf(stderr, "wat2c: dosya okunamadı: %s\n", files[i]);
            fclose(out);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz < 0) {
            fclose(fp);
            fclose(out);
            return 1;
        }
        u8* data = (u8*)malloc((size_t)sz + 1);
        size_t nr = fread(data, 1, (size_t)sz, fp);
        fclose(fp);
        data[nr] = 0;
        sizes[i] = (u32)nr;

        /* masm ve mdis ile doğrula */
        arena_reset(&arena);
        MasmResult mr = masm_assemble(&arena, (const char*)data, (u32)nr);
        if (!mr.ok) {
            fprintf(stderr, "wat2c: %s derleme hatası (%u:%u): %s\n",
                    files[i], mr.line, mr.col, mr.msg);
            free(data);
            fclose(out);
            return 1;
        }
        MdisResult dr = mdis_disassemble(&arena, mr.wasm, mr.len);
        if (!dr.ok) {
            fprintf(stderr, "wat2c: %s mdis hatası: %s\n", files[i], dr.msg);
            free(data);
            fclose(out);
            return 1;
        }

        fprintf(out, "static const char g_asm_%u[%u] = {", i, (u32)nr + 1u);
        for (size_t b = 0; b <= nr; b++) {
            if (b > 0) fputc(',', out);
            fprintf(out, "%u", (unsigned)data[b]);
        }
        fprintf(out, "};\n");
        free(data);
    }

    fprintf(out, "\nconst EmbeddedAsm g_embedded_asm[%u] = {\n", file_count > 0 ? file_count : 1u);
    for (u32 i = 0; i < file_count; i++) {
        const char* bname = base_name(files[i]);
        char exp_name[128];
        size_t blen = strlen(bname);
        size_t elen = (blen > 4 && strcmp(bname + blen - 4, ".wat") == 0) ? (blen - 4) : blen;
        if (elen >= sizeof(exp_name)) elen = sizeof(exp_name) - 1;
        memcpy(exp_name, bname, elen);
        exp_name[elen] = '\0';

        fprintf(out, "    { \"%s\", \"%s\", g_asm_%u, %uu },\n",
                bname, exp_name, i, sizes[i]);
    }
    if (file_count == 0) {
        fprintf(out, "    { \"\", \"\", \"\", 0u },\n");
    }
    fprintf(out, "};\n");
    fprintf(out, "const u32 g_embedded_asm_count = %uu;\n\n", file_count);

    fprintf(out,
        "static b32 str_eq(const char* a, const char* b) {\n"
        "    u32 i = 0;\n"
        "    while (a[i] != '\\0' && b[i] != '\\0') {\n"
        "        if (a[i] != b[i]) return FALSE;\n"
        "        i++;\n"
        "    }\n"
        "    return a[i] == b[i];\n"
        "}\n\n"
        "const EmbeddedAsm* bench_find_asm(const char* export_name) {\n"
        "    if (!export_name) return NULL;\n"
        "    for (u32 i = 0; i < g_embedded_asm_count; i++) {\n"
        "        if (str_eq(g_embedded_asm[i].export_name, export_name) ||\n"
        "            str_eq(g_embedded_asm[i].name, export_name)) {\n"
        "            return &g_embedded_asm[i];\n"
        "        }\n"
        "    }\n"
        "    return NULL;\n"
        "}\n");

    fclose(out);

    if (allocated_paths) {
        for (u32 i = 0; i < file_count; i++) {
            free(files[i]);
        }
    }
    return 0;
}
