/* masm/masm.h — WebAssembly metin biçimi (WAT alt kümesi) birleştiricisi (assembler).
 * mbin üzerine inşa edilmiştir; libc veya malloc kullanmaz, Arena ile çalışır.
 * Hem native araçlarda (tools/host/tool_masm) hem de çalışma anında tarayıcıda
 * (engine.wasm içinde) doğrudan WAT metninden ikili WebAssembly modülü üretir.
 */
#ifndef MASM_MASM_H
#define MASM_MASM_H

#include "core/types.h"
#include "core/arena.h"

typedef struct {
    b32  ok;
    u8*  wasm;      /* Arena üzerinde ayrılmış ikili modül baytları */
    u32  len;       /* wasm bayt uzunluğu */
    u32  line;      /* Hata durumunda 1-tabanlı satır numarası */
    u32  col;       /* Hata durumunda 1-tabanlı sütun numarası */
    char msg[256];  /* Hata mesajı ("satır:sütun: açıklama" biçiminde) */
} MasmResult;

/* WAT kaynak metnini derler ve ikili wasm çıktısını arena üzerinde döndürür. */
MasmResult masm_assemble(Arena* arena, const char* src, u32 src_len);

/* NUL sonlandırmalı WAT dizgesi için yardımcı sarmalayıcı. */
MasmResult masm_assemble_cstr(Arena* arena, const char* src);

#endif /* MASM_MASM_H */
