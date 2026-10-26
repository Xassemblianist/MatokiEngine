/* masm/mdis.h — WebAssembly ikili (binary) modül çözücüsü (disassembler).
 * İkili wasm modülünü okuyarak masm'ın yeniden derleyebileceği düz (flat) WAT
 * metnine dönüştürür. libc veya malloc kullanmaz; çıktı Arena üzerinde ayrılır.
 */
#ifndef MASM_MDIS_H
#define MASM_MDIS_H

#include "core/types.h"
#include "core/arena.h"

typedef struct {
    b32   ok;
    char* wat;       /* Arena üzerinde ayrılmış NUL-sonlandırmalı WAT metni */
    u32   len;       /* Metin uzunluğu (NUL hariç) */
    u32   offset;    /* Hata durumunda wasm içindeki bayt ofseti */
    char  msg[256];  /* Hata mesajı */
} MdisResult;

/* İkili WebAssembly modülünü düz (flat) WAT metnine çevirir. */
MdisResult mdis_disassemble(Arena* arena, const u8* wasm, u32 wasm_len);

#endif /* MASM_MDIS_H */
