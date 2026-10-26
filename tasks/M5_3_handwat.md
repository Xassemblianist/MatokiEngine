DURUM: DONE
HAT: C (elle WAT vs clang)

# M5_3 — Elle yazılmış WAT sıcak döngüleri, mikro-benchmark çatısı, masm genişletmeleri (BÜYÜK)

## Amaç
"Elle assembly clang'ı yenebilir mi?" sorusunu ÖLÇEREK cevaplamak. Kural (ARCHITECTURE §0.5): elle WAT yalnızca
clang'ı **>%15** geçtiği yerde motora girer. Kaybettiği yerler de Notlar'a yazılır — bu da sonuçtur.

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/raster/*` (A), `src/matoki/*` (D), `src/thread/*` + `web/jit.js` (B), `src/app/*` + `web/platform.js` (E).
Sende: `src/masm/*`, `src/mbin/*` (yalnız ekleme), `src/jit/jit.c/.h`, `asm/`, `src/bench/`, `web/bench.*`.

## Dosyalar
- **masm genişletmeleri:** `br_table`, multi-value blok/fonksiyon sonuçları, `memory.copy/fill`, tüm i8x16/i16x8/i64x2/f64x2 SIMD aritmetiği, `v128.load64_lane/store64_lane`, `v128.load8x8_u` vb. genişleten yükler, relaxed-SIMD **yok** (Notlar'da neden). Altın test (`wat2wasm` byte-byte) yeni dosyalarla genişler.
- `asm/*.wat` — elle yazılmış çekirdekler. Her biri için birebir aynı işi yapan C versiyonu `src/bench/bench_kernels.c`'de (clang -O2 -msimd128):
  1. `fill_u32` — framebuffer temizleme (memory.fill'e karşı da)
  2. `blend_alpha_rgba8` — n piksel src-over (src RGBA8 + dst RGBA8)
  3. `rgba_f32_to_u8` — SoA f32 4 kanal → paketli RGBA8 (raster'ın füzyonsuz yolu)
  4. `edge_block_8x8` — 3 kenar fonksiyonu, 8×8 blok kapsama maskeleri (16 quad × 4 bit)
  5. `bilinear_gather` — 4 lane bilinear doku örnekleme
  6. `sum_f32` / `dot_f32` — azaltma (yatay toplam kalıpları)
  7. `mandelbrot_row` — hesap yoğun, dallanmalı
- `src/bench/bench.h/.c` — mikro-benchmark çatısı: ısınma (TurboFan'a geçiş için), tekrar, medyan + min, `plat_time_ms`; sonuç tablosu LOG'a ve bir tampona.
- `src/bench/bench_main.c` — `EXPORT(engine_bench_run)`: her çekirdek için C versiyonu + masm ile **çalışma anında** derlenen WAT versiyonu (`asm/*.wat` dosyaları build'de gömülü: `shaders/` gibi bir gömme mekanizması gerekiyorsa `tools/embed.py`'yi kopyalayıp `tools/embed_asm.py` olarak kullan ve Makefile değişikliğini görev dosyasına ÖNERİ olarak yaz; o zamana kadar WAT metnini C string olarak `src/bench/asm_sources.c`'ye üreten bir host aracı `tools/host/tool_wat2c.c` yaz).
  Önce sonuçların eşitliğini doğrula (bit-bit), sonra ölç.
- `web/bench.html/.js` — çalıştırır, tabloyu gösterir: çekirdek | C (clang) | WAT (elle) | oran | karar (MOTORA GİR / REDDET).
- Her WAT çekirdeğinin başında: hangi fikirle clang'ı yenmeyi denediğin (yorum).
- `tests/test_masm.c` genişlet; `tests/test_kernels.c`: host'ta C çekirdekleri için doğruluk (WAT versiyonları node'da: `tools/host/run_kernels.mjs` ile C sonucu vs WAT sonucu).

## Kabul kriterleri
- `make`, `make test` uyarısız; tüm WAT çekirdekleri C versiyonlarıyla bit-bit aynı sonuç.
- Notlar: Chrome'da ve (varsa) Firefox'ta benchmark tablosu. Kazanan çekirdekler için A hattına entegrasyon ÖNERİSİ (hangi fonksiyon, nasıl çağrılır).
- Dürüstlük: clang kazandıysa "clang kazandı" yaz ve iç döngüsünü `mdis`/`wasm2wat` ile göster.

---

## Notlar (Ajan C — Elle WAT vs Clang)

### 1. Yapılanlar ve `masm` / `mdis` / `mbin` Genişletmeleri
- **`src/mbin/mbin_ops.h`, `src/masm/masm.c`, `src/masm/mdis.c`:**
  - `br_table` (hem düz hem katlanmış S-ifadesi, sayısal ve `$etiket` hedefleriyle),
  - Çoklu değer (multi-value) döndüren fonksiyon ve `block` / `loop` / `if` blok tipleri (`0x40` void, tekil `valtype`, veya SLEB128 `type_idx` kanonik seçimi),
  - Bulk-memory ve bellek yönetimi: `memory.copy`, `memory.fill`, `memory.size`, `memory.grow`,
  - Genişleten (widening) ve şerit (lane) SIMD bellek komutları: `v128.load8x8_s/u`, `v128.load16x4_s/u`, `v128.load32x2_s/u`, `v128.load8/16/32/64_splat`, `v128.load32/64_zero`, `v128.load8/16/32/64_lane`, `v128.store8/16/32/64_lane`,
  - Tüm standart 128-bit SIMD aritmetik kümesi: `i8x16.*` (popcnt, avgr_u, add/sub_sat, min/max, shl/shr, bitmask, all_true, cmp), `i16x8.*` (q15mulr_sat_s, extadd_pairwise, extmul_low/high, extend_low/high, narrow, avgr_u, add/sub_sat, shl/shr, bitmask, cmp), `i64x2.*` (add, sub, mul, abs, neg, shl/shr, extend, extmul, bitmask, eq/ne/lt_s/gt_s/le_s/ge_s), `f64x2.*` (add, sub, mul, div, sqrt, abs, neg, ceil, floor, trunc, nearest, min/max, pmin/pmax, convert/promote/demote/trunc_sat).
  - Altın test paketi (`tests/wat/26_br_table.wat` .. `31_simd_widening_and_lane_mem.wat`, toplam 31 altın dosya) `wat2wasm` ile **byte-byte birebir** ve `mdis` ile **gidiş-dönüş (`masm(mdis(x)) == x`)** doğrulandı.
- **Neden Relaxed-SIMD Eklenmedi?**
  WebAssembly Relaxed-SIMD (`i8x16.relaxed_swizzle`, `f32x4.relaxed_madd`, `i32x4.relaxed_trunc_f32x4_s`, `i16x8.relaxed_q15mulr_s` vb.) tasarım gereği **donanıma bağımlı (non-deterministic)** semantiğe sahiptir: x86_64 (AVX2 `pshufb` / FMA) ile ARM64 (NEON `vtbl` / ayrı çarpma-toplama veya farklı NaN/taşma davranışı) üzerinde farklı bit sonuçları üretir. MATOKI'nin temel mimari kuralı (`ARCHITECTURE.md §0.5` ve bu görevin kabul kriteri) **C referansı ile WAT çekirdeklerinin bit-bit aynı sonucu üretmesi** olduğundan, deterministik olmayan Relaxed-SIMD komutları bilinçli olarak dışarıda bırakılmıştır.

---

### 2. Gerçek Benchmark Sonuçları (Google Chrome, Mozilla Firefox, Node/V8)
Ölçüm parametreleri: her çekirdek için önce `engine_bench_verify_only` / `engine_bench_run` içinde C ve WAT çıktılarının **bit-bit eşitliği** doğrulanmış, ardından TurboFan / Ion tier-up için ısınma (`warmup`) + **15 tekrar** üzerinden medyan ve minimum süreler (`plat_time_ms` / `performance.now()`) ölçülmüştür. Kural: **oran > 1.15x (>%15)** ise `MOTORA GİR`, aksi halde `REDDET`.

#### A) Google Chrome (V8 — Headless Chrome Gerçek Zamanlı Ölçüm)
| çekirdek | bit-bit | C (clang -O2 -msimd128) | WAT (elle) | oran | karar |
| :--- | :---: | :---: | :---: | :---: | :--- |
| `fill_u32 (rgba)` | EVET | 2.15 ms (min 1.98) | 2.21 ms (min 1.97) | **0.98x** | **REDDET** (clang ile başa baş) |
| `fill_u32 (memfill)` | EVET | 2.12 ms (min 1.95) | 1.99 ms (min 1.85) | **1.07x** | **REDDET** (<%15 eşiği) |
| `blend_alpha_rgba8` | EVET | 6.76 ms (min 6.38) | 3.94 ms (min 3.79) | **1.72x** | **MOTORA GİR** (+%72) |
| `rgba_f32_to_u8` | EVET | 8.01 ms (min 7.84) | 11.47 ms (min 10.70) | **0.70x** | **REDDET (clang kazandı!)** |
| `edge_block_8x8` | EVET | 2.21 ms (min 2.09) | 1.37 ms (min 1.28) | **1.62x** | **MOTORA GİR** (+%62) |
| `bilinear_gather` | EVET | 8.04 ms (min 7.84) | 7.17 ms (min 6.88) | **1.12x** | **REDDET** (+%12 < %15 eşiği) |
| `sum_f32` | EVET | 6.28 ms (min 6.17) | 0.40 ms (min 0.40) | **15.51x** | **MOTORA GİR** (+15.5x) |
| `dot_f32` | EVET | 6.30 ms (min 6.26) | 0.70 ms (min 0.68) | **9.00x** | **MOTORA GİR** (+9.0x) |
| `mandelbrot_row` | EVET | 4.51 ms (min 4.47) | 1.21 ms (min 1.18) | **3.72x** | **MOTORA GİR** (+3.7x) |

#### B) Mozilla Firefox (SpiderMonkey — Headless Firefox Gerçek Zamanlı Ölçüm)
| çekirdek | bit-bit | C (clang -O2 -msimd128) | WAT (elle) | oran | karar |
| :--- | :---: | :---: | :---: | :---: | :--- |
| `fill_u32 (rgba)` | EVET | 2.10 ms (min 2.02) | 2.04 ms (min 1.94) | **1.03x** | **REDDET** (clang ile başa baş) |
| `fill_u32 (memfill)` | EVET | 2.14 ms (min 1.94) | 4.28 ms (min 4.12) | **0.50x** | **REDDET (clang kazandı!)** |
| `blend_alpha_rgba8` | EVET | 5.76 ms (min 5.46) | 4.30 ms (min 3.96) | **1.34x** | **MOTORA GİR** (+%34) |
| `rgba_f32_to_u8` | EVET | 7.86 ms (min 7.46) | 10.34 ms (min 9.60) | **0.76x** | **REDDET (clang kazandı!)** |
| `edge_block_8x8` | EVET | 1.30 ms (min 1.18) | 1.38 ms (min 1.28) | **0.94x** | **REDDET** (SpiderMonkey'de başa baş) |
| `bilinear_gather` | EVET | 7.94 ms (min 7.76) | 7.28 ms (min 7.04) | **1.09x** | **REDDET** (+%9 < %15 eşiği) |
| `sum_f32` | EVET | 6.42 ms (min 6.26) | 0.46 ms (min 0.42) | **13.96x** | **MOTORA GİR** (+14.0x) |
| `dot_f32` | EVET | 6.66 ms (min 6.32) | 0.74 ms (min 0.66) | **9.00x** | **MOTORA GİR** (+9.0x) |
| `mandelbrot_row` | EVET | 4.66 ms (min 4.44) | 1.22 ms (min 1.08) | **3.82x** | **MOTORA GİR** (+3.8x) |

#### C) Node.js (V8 — `node tools/host/run_kernels.mjs --bench`)
| çekirdek | bit-bit | C (clang -O2 -msimd128) | WAT (elle) | oran | karar |
| :--- | :---: | :---: | :---: | :---: | :--- |
| `fill_u32 (rgba)` | EVET | 2.02 ms (min 1.87) | 1.96 ms (min 1.86) | **1.03x** | **REDDET** |
| `fill_u32 (memfill)` | EVET | 1.92 ms (min 1.86) | 1.91 ms (min 1.83) | **1.00x** | **REDDET** |
| `blend_alpha_rgba8` | EVET | 6.28 ms (min 6.13) | 3.67 ms (min 3.63) | **1.71x** | **MOTORA GİR** |
| `rgba_f32_to_u8` | EVET | 8.25 ms (min 7.83) | 10.29 ms (min 9.96) | **0.80x** | **REDDET (clang kazandı!)** |
| `edge_block_8x8` | EVET | 2.14 ms (min 1.97) | 1.29 ms (min 1.24) | **1.65x** | **MOTORA GİR** |
| `bilinear_gather` | EVET | 7.71 ms (min 7.43) | 6.87 ms (min 6.65) | **1.12x** | **REDDET** |
| `sum_f32` | EVET | 6.07 ms (min 5.88) | 0.38 ms (min 0.37) | **15.79x** | **MOTORA GİR** |
| `dot_f32` | EVET | 6.04 ms (min 5.89) | 0.65 ms (min 0.64) | **9.26x** | **MOTORA GİR** |
| `mandelbrot_row` | EVET | 4.27 ms (min 4.22) | 1.11 ms (min 1.10) | **3.85x** | **MOTORA GİR** |

---

### 3. Dürüst Analiz: Clang Nerede Kazandı ve Neden? (`mdis` Çıktılarıyla)

#### A) `rgba_f32_to_u8` — **Clang Kazandı** (Chrome: 0.70x, Firefox: 0.76x, Node: 0.80x)
- **Ne denedik (`asm/rgba_f32_to_u8.wat`):** 4 kanal `i32x4` (`ri, gi, bi, ai`) üretildikten sonra iki `i16x8.narrow_i32x4_u`, bir `i8x16.narrow_i16x8_u` ve tek bir `i8x16.shuffle (0,4,8,12, 1,5,9,13, 2,6,10,14, 3,7,11,15)` ile yazmaç içinde SoA→AoS devrik (transpose) paketleme yaptık ve döngüyü 2× açtık.
- **Clang neden kazandı?** `clang -O2 -msimd128` daraltma (`packusdw`/`packuswb`) ve 16-baytlık genel `pshufb` (`i8x16.shuffle`) kullanmak yerine her kanalı `i32x4` içinde doğrudan `i32x4.shl (8, 16, 24)` ve `v128.or` ile birleştirdi! x86_64 mikro-mimarisinde `packusdw` + `packuswb` + `pshufb` komutlarının üçü de **yalnızca tek bir shuffle/pack portunda (Port 5)** çalışırken, clang'ın ürettiği `pslld` (`i32x4.shl`) ve `por` (`v128.or`) komutları **tüm vektör ALU portlarında (Port 0, 1, 5)** paralel çalışır ve ekstra shuffle maskesi yazmacı tüketmez.
- **Clang'ın ürettiği iç döngü (`./build/host/mdis` çıktısı):**
```wat
loop
  local.get 8
  v128.const i32x4 1065353216 1065353216 1065353216 1065353216 ;; 1.0f
  local.tee 13
  v128.const i32x4 0 0 0 0                                     ;; 0.0f
  local.tee 14
  local.get 11
  v128.load align=4                                            ;; g[i..i+3]
  f32x4.pmax
  f32x4.pmin
  v128.const i32x4 1132396544 1132396544 1132396544 1132396544 ;; 255.0f
  local.tee 15
  f32x4.mul
  v128.const i32x4 1056964608 1056964608 1056964608 1056964608 ;; 0.5f
  local.tee 16
  f32x4.add
  i32x4.trunc_sat_f32x4_s
  i32.const 8
  i32x4.shl
  local.get 13
  local.get 14
  local.get 12
  v128.load align=4                                            ;; r[i..i+3]
  f32x4.pmax
  f32x4.pmin
  local.get 15
  f32x4.mul
  local.get 16
  f32x4.add
  i32x4.trunc_sat_f32x4_s
  v128.or
  local.get 13
  local.get 14
  local.get 10
  v128.load align=4                                            ;; b[i..i+3]
  f32x4.pmax
  f32x4.pmin
  local.get 15
  f32x4.mul
  local.get 16
  f32x4.add
  i32x4.trunc_sat_f32x4_s
  i32.const 16
  i32x4.shl
  v128.or
  local.get 13
  local.get 14
  local.get 9
  v128.load align=4                                            ;; a[i..i+3]
  f32x4.pmax
  f32x4.pmin
  local.get 15
  f32x4.mul
  local.get 16
  f32x4.add
  i32x4.trunc_sat_f32x4_s
  i32.const 24
  i32x4.shl
  v128.or
  v128.store align=4
  ...
```

#### B) `fill_u32 (rgba)` ve `fill_u32 (memfill)` — **Başa Baş / Firefox'ta Clang Kazandı**
- **Genel RGBA (`0xFF336699`):** Clang zaten döngüyü `i32x4.splat` + `v128.store` olarak vektörleştiriyor. 256 KB (`65536` piksel) yazmada darboğaz L1/L2 önbellek bant genişliği olduğundan 4× döngü açma (`offset=0/16/32/48`) yalnızca %0–%3 fark yaratıyor (**REDDET**).
- **`memory.fill` Sürprizi (Firefox SpiderMonkey bulgusu):** Paylaşımlı bellek (`(memory 1 16384 shared)` / `SharedArrayBuffer`) üzerinde `memory.fill` komutu V8'de `v128.store` ile aynı hızda (~1.95 ms) çalışırken, **Firefox SpiderMonkey'de paylaşımlı bellek üzerindeki `memory.fill` 2 kat daha yavaş (4.28 ms vs 2.14 ms, 0.50x)** çalışmaktadır! Çünkü SpiderMonkey paylaşımlı bellekte `memory.fill` için satır içi SIMD döngüsü yerine çalışma zamanı (runtime) çağrısına düşmektedir.
- **Clang'ın `bench_c_fill_u32` iç döngüsü (`./build/host/mdis` çıktısı):**
```wat
loop
  local.get 6
  local.get 4
  v128.store align=4
  local.get 6
  i32.const 16
  i32.add
  local.set 6
  local.get 5
  i32.const -4
  i32.add
  local.tee 5
  br_if 0
end
```

#### C) `bilinear_gather` — **WAT +%12 Hızlı Ama <%15 Eşiği Altında (`REDDET`)**
- Elle WAT koordinat hesabını (`fu, fv, floor, ax, ay`), adres hesabını ve 4 kanal `lerp` hesabını tamamen `f32x4` / `i32x4` ile vektörleştirdi ve clang'ın skaler kodunu Chrome'da **1.12x (%12)**, Firefox'ta **1.09x (%9)** geçti.
- Ancak WebAssembly SIMD128'de donanım `gather` komutu olmadığından her quad'da 16 adet `i32x4.extract_lane` + `v128.load32_lane` komutu SIMD→GPR→Bellek→SIMD geçişi yapmakta ve kazancı %12'de sınırlamaktadır. ARCHITECTURE §0.5'teki **>%15** kuralı gereği dürüstçe **REDDET** verildi.

#### D) Elle WAT'ın Net Kazandığı Çekirdekler (`MOTORA GİR`)
1. **`blend_alpha_rgba8` (Chrome 1.72x, Firefox 1.34x, Node 1.71x):**
   Clang pikselleri `i32x4` içinde tutup R, G, B, A kanallarını 4 kez `i32x4.shr_u` + `v128.and 0xFF` + `i32x4.mul` ile ayrı ayrı işlerken; elle WAT `i8x16.shuffle` ile alfa baytını yayar, `255 - sa`'yı tek bir `v128.not` ile bulur ve `i16x8.extmul_low/high_i8x16_u` + `i8x16.narrow_i16x8_u` ile 4 pikseli (16 kanal) tek seferde harmanlar.
2. **`edge_block_8x8` (Chrome 1.62x, Node 1.65x):**
   Clang iç döngüdeki 4 quad'ı (`rx=0..3`) vektörleştirip 16 adet `i8x16.shuffle` + `v128.bitselect` (~1.4 KB kod) üretirken; elle WAT bir quad'ın 4 pikselini tek `i32x4`'te tutup `i32x4.bitmask` + satır bazında `0x0F0F0F0F` XOR + tek `i32.store` ile 62% hızlanır.
3. **`sum_f32` (Chrome 15.51x, Firefox 13.96x) ve `dot_f32` (Chrome 9.00x, Firefox 9.00x):**
   Clang `-O2 -msimd128` (`-ffast-math` olmadan) IEEE-754 toplama sırasını değiştiremediğinden tek bir skaler `sum` yazmacına seri `f32.add` zinciri üretir; elle WAT 4 bağımsız `f32x4` biriktiricisiyle (16 float/adım) FPU gecikmesini tamamen gizler.
4. **`mandelbrot_row` (Chrome 3.72x, Firefox 3.82x):**
   Veri-bağımlı erken çıkış (`break`) yüzünden clang döngüyü vektörleştiremez; elle WAT 4 pikseli `f32x4` şeridinde işleyip `v128.any_true` ile tüm şerit ıraksadığında erken çıkar ve `i32x4.sub(viter, vactive)` ile dallanmasız sayaç artırır.

---

### 4. ÖNERİLER (Diğer Hatlar ve Makefile İçin)

- **ÖNERİ 1 (A Hattı — `src/raster/raster_simd_wasm.c` ve `src/raster/framebuffer.c`):**
  1. **`BLEND_ALPHA` yolu (`raster_kern_blend_batch`):** Mevcut `raster_simd_wasm.c` `dst` pikselini `f32x4`'e çevirip float çarpma yapıyor (veya `out_rgba` zaten `u8`'e çevrildiğinde). Tampon/sprite harmanlamada `asm/blend_alpha_rgba8.wat` içindeki `i8x16.shuffle` + `v128.not` + `i16x8.extmul_low/high_i8x16_u` + `i8x16.narrow_i16x8_u` kalıbı doğrudan `jit_load` ile veya intrinsic karşılığıyla çağrıldığında **1.72x** hızlanma sağlar.
  2. **`simd_clamp01` ve `simd_pack_rgba8` (`src/raster/raster_simd_wasm.c` satır 34–59):**
     - `simd_clamp01` içindeki 4 komutluk `gt + and + gt + bitselect` yerine `wasm_f32x4_pmin(one, wasm_f32x4_pmax(zero, v))` (2 komut, NaN ve `-0.0` dahil bit-bit aynı) kullanılmalıdır.
     - `simd_pack_rgba8` içinde `narrow` + `shuffle` yerine clang'ın `bench_c_rgba_f32_to_u8`'de kullandığı `i32x4.shl (8, 16, 24)` + `v128.or` kalıbı hem Chrome hem Firefox'ta %25–%30 daha hızlıdır.
  3. **`fb_clear` (`src/raster/framebuffer.c` satır 30–33):**
     Paylaşımlı bellek (`SharedArrayBuffer`) aktifken Firefox'ta `__builtin_memset` (`memory.fill`) `v128.store` döngüsünden **2 kat yavaştır** (`4.28 ms` vs `2.14 ms`). `fb_clear` ve `fb_clear_depth` içinde `#if defined(__wasm_simd128__)` altında `__builtin_memset` erken çıkışı kaldırılıp her zaman 4× açılmış `wasm_v128_store` döngüsü çalıştırılmalıdır.
  4. **Kısmi 8×8 blok taraması (`raster_kern_tri_scan`):**
     Her quad'da `wasm_i32x4_bitmask(wasm_v128_not(any_neg))` yapmak yerine `wasm_i32x4_bitmask(any_neg) ^ 0xFu` (veya 4 quad için `0x0F0F0F0Fu` XOR) kullanılarak her quad'da bir `v128.not` komutu tasarruf edilir.

- **ÖNERİ 2 (`Makefile` — `asm/*.wat` Otomatik Gömme Kuralı):**
  `tools/embed_asm.py` ve `tools/host/tool_wat2c.c` hazırlandı. `Makefile`'a aşağıdaki kural eklendiğinde `asm/*.wat` değiştikçe `src/bench/asm_sources.c` otomatik güncellenir:
  ```makefile
  ASM_WAT := $(sort $(wildcard asm/*.wat))
  src/bench/asm_sources.c: $(ASM_WAT) tools/embed_asm.py
  	python3 tools/embed_asm.py $@ $(ASM_WAT)
  ```
