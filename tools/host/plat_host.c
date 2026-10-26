/* tools/host/plat_host.c — platform.h import'larının native implementasyonu.
 * Testler ve matc gibi host araçları src/ kodunu bununla linkler.
 * plat_jit_load host'ta desteklenmez (-1); JIT testleri wasm-interp ile yapılır.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../src/plat/platform.h"

void plat_log(const char* s, u32 len) {
    fwrite(s, 1, len, stdout);
    fputc('\n', stdout);
}

void plat_panic(const char* s, u32 len) {
    fputs("PANIC: ", stderr);
    fwrite(s, 1, len, stderr);
    fputc('\n', stderr);
    abort();
}

f64 plat_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec * 1000.0 + (f64)ts.tv_nsec / 1e6;
}

i32 plat_jit_load(const u8* wasm, u32 len, const char* export_name, u32 name_len) {
    (void)wasm; (void)len; (void)export_name; (void)name_len;
    return -1;
}
