/* plat/platform.h — JS platform katmanından gelen import'lar ve export makrosu.
 * Sözleşme: docs/ARCHITECTURE.md §2. Değiştirmek mimari karardır.
 * Native (host) derlemede import'lar tools/host/plat_host.c tarafından sağlanır.
 */
#ifndef PLAT_PLATFORM_H
#define PLAT_PLATFORM_H

#include "core/types.h"

#if defined(__wasm__)
#define PLAT_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#define EXPORT(name)      __attribute__((export_name(#name)))
#else
#define PLAT_IMPORT(name)
#define EXPORT(name)
#endif

PLAT_IMPORT(plat_log)      void plat_log(const char* s, u32 len);
PLAT_IMPORT(plat_panic)    void plat_panic(const char* s, u32 len);
PLAT_IMPORT(plat_time_ms)  f64  plat_time_ms(void);
/* wasm modülünü instantiate eder, export'u paylaşılan tabloya koyar.
 * Dönüş: tablo indeksi (C'de fonksiyon işaretçisi olarak kullanılır), hata: -1 */
PLAT_IMPORT(plat_jit_load) i32  plat_jit_load(const u8* wasm, u32 len,
                                              const char* export_name, u32 name_len);
PLAT_IMPORT(plat_download) void plat_download(const char* name, u32 name_len,
                                              const u8* ptr, u32 len);

#endif
