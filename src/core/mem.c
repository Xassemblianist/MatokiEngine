/* core/mem.c — clang'ın örtük çağırdığı memcpy/memset/memmove/memcmp.
 * Sadece wasm derlemesinde kullanılır (host'ta libc sağlar).
 * -mbulk-memory ile __builtin_memcpy/memset tek bir memory.copy/fill opcode'una iner.
 */
#include "core/types.h"

#if defined(__wasm__)

void* memcpy(void* restrict dst, const void* restrict src, usize n) {
    __builtin_memcpy(dst, src, n);
    return dst;
}

void* memmove(void* dst, const void* src, usize n) {
    /* memory.copy örtüşen bölgeleri doğru işler */
    __builtin_memmove(dst, src, n);
    return dst;
}

void* memset(void* dst, int c, usize n) {
    __builtin_memset(dst, c, n);
    return dst;
}

int memcmp(const void* a, const void* b, usize n) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

#endif
