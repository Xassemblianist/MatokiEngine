/* jit/jit.c — C ↔ JS JIT modül yükleme köprüsü implementasyonu.
 * plat_jit_load ile çalışma anında wasm baytlarını WebAssembly.Module'e derletir,
 * paylaşılan fonksiyon tablosuna ekletir ve tablo indeksini işaretçiye çevirir.
 */
#include "jit/jit.h"
#include "plat/platform.h"
#include "thread/thread.h"
#include "core/log.h"

static u32 jit_cstrlen(const char* s) {
    u32 n = 0;
    if (!s) return 0;
    while (s[n] != '\0') n++;
    return n;
}

void* jit_load(const u8* wasm, u32 len, const char* export_name) {
    if (!wasm || len == 0 || !export_name) {
        LOG("[jit] jit_load: invalid arguments (wasm=%p, len=%u)", (const void*)wasm, len);
        return NULL;
    }
    u32 name_len = jit_cstrlen(export_name);
    if (name_len == 0) {
        LOG("[jit] jit_load: empty export_name");
        return NULL;
    }

    i32 idx = plat_jit_load(wasm, len, export_name, name_len);
    if (idx < 0) {
        LOG("[jit] jit_load failed for export '%s' (len=%u)", export_name, len);
        return NULL;
    }

    /* Tüm aktif worker thread'lerin aynı tablo indeksine modülü yüklemesini bekle */
    if (!thread_wait_jit_ack((u32)idx)) {
        return NULL;
    }

    return (void*)(uptr)(u32)idx;
}

ShadeFn jit_load_shade(const u8* wasm, u32 len) {
    return (ShadeFn)jit_load(wasm, len, "shade");
}
