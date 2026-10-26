DURUM: DONE
HAT: C (masm/mdis — bizim assembler'ımız)

# M3_2 — masm (assembler), mdis (disassembler), JIT köprüsü (BÜYÜK)

## Amaç
1. `masm`: wasm text formatını (WAT alt kümesi) **kendi assembler'ımızla** binary'ye çevirmek — mbin üstüne.
   Elle yazılmış sıcak döngüler (M5) bununla derlenip çalışma anında yüklenecek.
2. `mdis`: binary → metin. JIT çıktısını okumak için hata ayıklama aracı.
3. JIT köprüsü: C'den üretilen modülü tarayıcıda yükleyip fonksiyon işaretçisi olarak çağırmak.

## Hazır olanlar (DOKUNMA)
`Makefile` (`tools/host/tool_X.c` → `build/host/X` otomatik), `src/core/*`, `src/mbin/mbin.h` (yalnız ekleme yapabilirsin: data/global/element section desteği gerekiyorsa `mbin.c/.h`'e EKLE, mevcut API'yi bozma), `src/plat/platform.h`, `web/platform.js` ve `web/jit.js` (A'nın), `tests/test.h`.

## Dosyalar
- `src/masm/masm.h/.c` — WAT alt kümesi parser'ı + mbin ile kodlama, `src/`'de olduğu için tarayıcıda da çalışır (libc yok, malloc yok):
  - `(module ...)`, `(type)`, `(import "m" "n" (func|memory|table ...))`, `(func $isim (param $a i32) (result i32) (local ...) ...)`, `(export)`, `(memory)`, `(global)`, `(data)`.
  - Talimatlar: **düz (flat) form** + **katlanmış (folded) S-ifadeleri** `(i32.add (local.get 0) (i32.const 1))`.
  - `$isim` çözümleme (fonksiyon, local, label, type, global), `block/loop/if ... end` etiketleri, `br $etiket`.
  - Bellek talimatları `offset=N align=N`.
  - SIMD: `v128.const f32x4 ...`/`i32x4 ...`, `i8x16.shuffle` 16 lane, lane talimatları.
  - mbin_ops.h'deki TÜM opcode'lar desteklenir.
  - Hata mesajı: satır:sütun.
- `src/masm/mdis.h/.c` — binary → masm'ın okuyabildiği metin (düz form). Tüm section'lar, isimler `$f0` vb.
- `tools/host/tool_masm.c` (`masm in.wat -o out.wasm`), `tools/host/tool_mdis.c` (`mdis in.wasm`).
- `tests/wat/*.wat` — en az 25 dosya: kontrol akışı, bellek, SIMD, import/export, global, data, folded/flat karışık.
- `tests/test_masm.c`:
  - **Altın test:** her `tests/wat/*.wat` için `masm` çıktısı **`wat2wasm` çıktısıyla byte-byte aynı** (`wat2wasm --enable-all --no-check` değil; gerçek). Farklıysa farkın nedeni Notlar'a (ör. wat2wasm'ın LEB padding'i) ve eşdeğerlik `wasm-validate` + `wasm2wat` metin karşılaştırmasıyla kanıtlanır.
  - **Gidiş-dönüş:** `masm(mdis(x)) == x` tüm test dosyaları ve D hattının ürettiği modüller (`build/test_out/`'ta varsa) için.
  - Hata durumları: 15+ bozuk girdi, doğru satır:sütun.
- `src/jit/jit.h/.c` — `ShadeFn jit_load_shade(const u8* wasm, u32 len)` (plat_jit_load sarmalayıcı, hata → NULL + LOG), `void* jit_load(const u8*, u32, const char* export_name)`.
- `src/jit/jit_selftest.c` — `EXPORT(engine_jit_selftest) i32 engine_jit_selftest(void)`: mbin ile `(i32,i32)->i32` topla fonksiyonu + masm ile metinden derlenmiş bir SIMD fonksiyonu üretir, jit_load ile yükler, fonksiyon işaretçisiyle çağırır, sonuçları doğrular; 0 = başarı.
- `web/jit_test.html` + `web/jit_test.js` — engine.wasm'ı (platform.js'e dokunmadan, kendi küçük yükleyicisiyle; jit.js'deki `createJitLoader`'ı import ederek) yükler, `engine_jit_selftest` çağırır, sonucu sayfaya yazar. Ayrıca çağrı maliyeti benchmark'ı: 10M kez doğrudan C çağrısı vs `call_indirect` ile JIT fonksiyonu.

## Kabul kriterleri
- `make`, `make test` uyarısız; altın ve gidiş-dönüş testleri geçer.
- `http://localhost:8080/jit_test.html` → "JIT OK" + çağrı maliyeti tablosu (Notlar'a).
- Notlar: masm'ın desteklemediği WAT özellikleri listesi.

## Notlar
### Yapılanlar
1. **`mbin` eklentileri (`src/mbin/mbin.h`, `src/mbin/mbin.c`):** Mevcut API hiç bozulmadan Table (Section 4), Global (Section 6), Element (Section 9) ve Data (Section 11) bölümleri, paylaşılan bellek bayrağı (`MB_LIM_SHARED`), açık tip ekleme (`mb_type_add` — tekilleştirmesiz `(type ...)` bildirimi için) ve sıralı import/export kayıtları eklendi.
2. **`masm` (`src/masm/masm.h`, `src/masm/masm.c`):** Sıfır libc bağımlılıklı, `Arena*` tabanlı iki geçişli WAT alt kümesi ayrıştırıcısı ve kodlayıcısı yazıldı:
   - Tüm `mbin_ops.h` opcode'ları (kontrol akışı, i32/i64/f32/f64 aritmetik/karşılaştırma/dönüşüm, bellek `offset=N align=N`, `0xFC` saturating trunc, `0xFD` 128-bit SIMD `v128.const`, `i8x16.shuffle`, lane işlemleri).
   - Düz (flat) ve katlanmış (folded) S-ifadeleri (`(if (cond) (then ...) (else ...))`, `(block $lbl (result i32) ...)` dahil).
   - `$isim` sembol çözümleme (type, func, param/local, global, block/loop/if etiketi), satır içi `(export "...")` ve `(import "...")` kısayolları.
   - Ondalık ve onaltılık tam sayılar ile IEEE-754 kayan nokta sabitleri (`0x1.8p+2`, `inf`, `nan`, `nan:0x...` dahil) sıfır libc ile ayrıştırıldı.
3. **`mdis` (`src/masm/mdis.h`, `src/masm/mdis.c`):** Wasm ikili modüllerini tüm bölümleriyle (Type, Import, Func, Table, Memory, Global, Export, Elem, Code, Data) `masm`'ın yeniden derleyebileceği düz WAT metnine çeviren disassembler yazıldı.
4. **CLI ve Testler (`tools/host/tool_masm.c`, `tools/host/tool_mdis.c`, `tests/wat/*.wat`, `tests/test_masm.c`):**
   - `tests/wat/` altındaki **25 WAT dosyasının tamamında (25/25)** `masm` çıktısı `wat2wasm` çıktısıyla **byte-byte birebir aynıdır** (0 bayt fark).
   - Tüm 25 WAT dosyası ve `build/test_out/` altındaki tüm modüller (`all_ops.wasm`, `sprite.wasm` vb.) için `masm(mdis(x)) == x` gidiş-dönüş testi byte-byte geçmektedir.
   - 20 farklı bozuk WAT girdisinde `line:col` hata konumu doğrulandı.
5. **JIT Köprüsü ve Öz-Test (`src/jit/jit.h/.c`, `src/jit/jit_selftest.c`, `web/jit_test.html`, `web/jit_test.js`):**
   - `engine_jit_selftest()` hem `mbin` ile çalışma anında üretilen `(i32, i32) -> i32` toplama modülünü hem de `masm` ile WAT metninden çalışma anında derlenen 128-bit SIMD + paylaşılan bellek (`v128.load`, `f32x4.splat`, `f32x4.mul`, `f32x4.add`, `v128.store`, `f32x4.extract_lane`) fonksiyonunu `jit_load` üzerinden yükleyip C fonksiyon işaretçisiyle (`call_indirect`) çağırarak doğrulamaktadır (`JIT OK`).

### Çağrı Maliyeti Benchmark Sonuçları (10,000,000 İterasyon, V8 Wasm JIT)
Aynı işi yapan (`a + (b ^ 1)`) `noinline` C fonksiyonu ile çalışma anında ayrı bir `WebAssembly.Instance` olarak derlenip paylaşılan `__indirect_function_table` üzerinden çağrılan JIT fonksiyonunun 10 milyon çağrı karşılaştırması:

| Çağrı Türü | Liftoff (Isınmasız) | TurboFan (Isınmalı) | Çağrı Başına Süre (TurboFan) |
|---|---|---|---|
| Doğrudan C çağrısı (`call`, aynı Wasm instance) | 28.83 ms | 4.63 ms | **0.46 ns / çağrı** |
| JIT fonksiyonu (`call_indirect`, ayrı Wasm instance) | 52.86 ms | 27.10 ms | **2.71 ns / çağrı** |
| **Fark / Ek Maliyet (`call_indirect` cross-instance)** | +24.03 ms (1.83x) | +22.47 ms (5.85x) | **+2.25 ns / çağrı** |

*(Not: MATOKI mimarisinde (`ARCHITECTURE.md §4`) JIT shader fonksiyonları piksel başına değil **karo/tile başına** (64×64 piksel) çağrıldığı için çağrı başına ~2.25 ns'lik `call_indirect` geçiş maliyeti 60 FPS'de kare başına < 0.001 ms'dir ve tamamen ihmal edilebilir düzeydedir.)*

### `masm`'ın Desteklemediği WAT Özellikleri Listesi
- `br_table` talimatı (`mbin_ops.h` kapsamında yer almadığından).
- Çoklu bellek (multi-memory) ve çoklu tablo (reference-types kapsamındaki birden fazla tablo indeksi).
- Pasif (passive) veya bildirimsel (declarative) `data` / `elem` segmentleri (yalnızca aktif `i32.const offset` içeren segmentler desteklenir).
- `start` bölümü (Section 8) ve özel (custom / name) bölümler.
- Çoklu dönüş değeri (multi-value blok ve fonksiyon imzaları; fonksiyon ve blok başına en fazla 1 `result` desteklenir).
- Referans tipleri (`externref`, `ref.null`, `ref.func`, `table.get`, `table.set`) ve toplu bellek (`memory.copy`, `memory.fill`, `memory.init`, `data.drop`) talimatları.
- `mbin_ops.h` dışında kalan `f64` özel matematik opcode'ları (`f64.abs`, `f64.neg`, `f64.sqrt`, `f64.floor`, `f64.ceil`, `f64.min`, `f64.max`, `f64.copysign`) ve `i8x16`/`i16x8`/`i64x2`/`f64x2` tam SIMD aritmetik ailesi (`mbin_ops.h` içindeki tüm SIMD opcode'ları tam desteklenir).
- Kısaltılmış tek-dosya örtük `(module ...)` (üst düzey `(module ...)` parantezi zorunludur).

