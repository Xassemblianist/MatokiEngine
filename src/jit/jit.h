/* jit/jit.h — Çalışma anında üretilen WebAssembly modüllerini yükleme köprüsü.
 * plat_jit_load import'unu sarmalar; yüklenen fonksiyonun paylaşılan tablodaki
 * indeksini C fonksiyon işaretçisine (call_indirect için) dönüştürür.
 */
#ifndef JIT_JIT_H
#define JIT_JIT_H

#include "core/types.h"
#include "raster/shade_abi.h"

/* Verilen wasm ikilisini instantiate eder ve export_name fonksiyonunu fonksiyon
 * işaretçisi olarak döndürür. Hata durumunda LOG basar ve NULL (0) döner.
 * Not: Wasm tablosunda indeks 0 geçerli olabileceğinden (ancak wasm-ld --stack-first
 * ve başlangıç tablosu 1 elemanlı olduğundan JIT fonksiyonları her zaman idx >= 1 alır),
 * hata kontrolü plat_jit_load < 0 üzerinden yapılır.
 */
void*   jit_load(const u8* wasm, u32 len, const char* export_name);

/* Gölgelendirici (ShadeFn) modülü yükleme kısayolu ("shade" export'unu arar). */
ShadeFn jit_load_shade(const u8* wasm, u32 len);

#endif /* JIT_JIT_H */
