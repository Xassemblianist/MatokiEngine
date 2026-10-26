/* bench/asm_sources.h — Gömülü elle yazılmış WAT çekirdek kaynakları (asm/ altındaki .wat).
 * tools/host/tool_wat2c.c (veya tools/embed_asm.py) tarafından üretilen
 * src/bench/asm_sources.c tablosuna erişim sağlar.
 */
#ifndef BENCH_ASM_SOURCES_H
#define BENCH_ASM_SOURCES_H

#include "core/types.h"

typedef struct {
    const char* name;        /* ör. "fill_u32.wat" */
    const char* export_name; /* ör. "fill_u32" */
    const char* src;         /* NUL sonlandırmalı WAT metni */
    u32         len;         /* NUL hariç bayt uzunluğu */
} EmbeddedAsm;

extern const EmbeddedAsm g_embedded_asm[];
extern const u32         g_embedded_asm_count;

const EmbeddedAsm* bench_find_asm(const char* export_name);

#endif /* BENCH_ASM_SOURCES_H */
