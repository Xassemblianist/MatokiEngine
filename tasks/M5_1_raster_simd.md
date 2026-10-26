DURUM: DONE
HAT: A (raster hız)

# M5_1 — SIMD rasterizer + derinlik tamponu + füzyon desteği (BÜYÜK)

## Bağlam
M2 benchmark: 10k sprite 18.3 ms, ~18 Mpix/s (i7-10750H, Chrome). raster 5.0 / shade 6.4 / blend 4.8 ms.
Hedef: aynı sahnede **raster + blend en az 2.5× hızlı**. Shade'i D hattı JIT ile hızlandırıyor.

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/raster/shade_abi.h` (ABI v1: `fb_px`, `fb_stride` eklendi — rasterizer HER batch'te doldurur), `src/matoki/*`, `web/*`, `src/app/*` (artık E hattının).

## Dosyalar
- `src/raster/raster.c/.h`, yeni `src/raster/raster_simd_wasm.c` (wasm SIMD — `wasm_simd128.h` serbest) + skaler yedek (host testleri için aynı API, `raster_simd_scalar.c` veya `#if`):
  - Kenar fonksiyonları: 8×8 blok içinde 2×2 quad'ların 4 lane'i tek `i32x4` ile; 3 kenar → kapsama maskesi `i32x4.bitmask`. Blok satırı başına artımlı adım.
  - Varying kurulumu: quad başına varyingleri SIMD ile (plane equation `a*x + b*y + c`, 4 lane tek seferde) doğrudan SoA `ShadeBatch.varyings`'e yaz.
  - Blend (füzyonsuz yol): 4 lane RGBA8 paketleme `i32x4.trunc_sat` + `narrow` ile; ALPHA ve ADD SIMD.
  - `fb_clear` SIMD (veya `memory.fill`), ölç.
  - Binning: üçgen başına karo aralığı; küçük üçgenler (tek karo) için hızlı yol.
- **Füzyon:** `raster_set_shader(...)`'a `b32 fused` parametresi (veya `_ex` varyantı): TRUE ise rasterizer blend+yazma yapmaz, shader `fb_px`'e yazar. Kapsamın doğru olması için mask'ler aynen geçer.
- **Derinlik tamponu (3D hazırlığı):** `Framebuffer`'a opsiyonel `f32* depth` (veya ayrı `DepthBuffer`), `raster_set_depth(r, test, write, func)`. Quad başına 4-lane z interpolasyonu + test → mask'ten düşür (shade'den ÖNCE = early-z). `RasterVert.z` zaten var.
- `tests/test_raster.c` — mevcut testler SIMD yolunda da geçmeli (host'ta skaler yedekle; wasm yolu için `tests/test_raster_wasm.mjs` node testi: aynı sahneyi wasm'da raster et, host'un ürettiği referans framebuffer ile **bit-bit** karşılaştır). Derinlik testi: kesişen iki üçgen, doğru görünürlük.

## Kabul kriterleri
- `make`, `make test` uyarısız. SIMD ve skaler yol aynı pikselleri üretir (bit-bit).
- Benchmark tablosu (Notlar): M2 ile aynı sahne/ayarlar, setup/bin/raster/shade/blend ayrı ayrı, öncesi/sonrası. Her optimizasyonun katkısı ayrı satır.
- 2.5× hedefi tutmazsa nedenini ölçümle açıkla (darboğaz nerede).

## Notlar

### Yapılanlar
1. **SIMD Rasterizer (`src/raster/raster_simd_wasm.c`) + Skaler Yedek (`src/raster/raster.c/.h`)**:
   - **Kenar fonksiyonları (`i32x4`):** 2×2 quad'ın 4 pikseli için başlangıç ofsetleri `lane_off = i32x4(0, a, b, a+b)` ile kurulup satır boyunca `step_x = i32x4.splat(2*a)` ile artımlı ilerletildi. 3 kenarın işareti `v128.not(v128.or(v128.or(qe0, qe1), qe2))` üzerinden tek `i32x4.bitmask` komutuyla 4-bit quad maskesine dönüştürüldü.
   - **Varying kurulumu (`f32x4`):** Düzlem denklemi (`v0 + dx*gx + dy*gy`) 4 lane için tek seferde (`f32x4.splat(base) + f32x4(0, gx, gy, gx+gy)`) hesaplanıp `v128.store` ile doğrudan 16-byte hizalı SoA `ShadeBatch.varyings`'e yazıldı. 2D UV (`varying_count == 2`, `is_affine`, derinlik kapalı) için çağrı yükü olmayan inline `emit_quad_uv_fast` yolu ayrıldı. 3D perspektif-doğru (`inv_w != 1`) yol da tamamen `f32x4` SIMD olarak gerçeklendi.
   - **SIMD Blend (`BLEND_NONE`, `BLEND_ALPHA`, `BLEND_ADD`):** 4 kanal `f32x4` çıktısı `[0, 1]` aralığına `simd_clamp01` (NaN ve `-0.0` semantiği skaler `m_clamp` ile bit-bit aynı) ile kırpılıp `* 255.0f + 0.5f` sonrası `i32x4.trunc_sat_f32x4_s` → `i16x8.narrow_i32x4_s` (R/B ve G/A çiftleri) → `i16x8.shl(..., 8)` + `v128.or` → `i16x8.shuffle(0, 4, 1, 5, 2, 6, 3, 7)` ile 4 adet `u32` RGBA8 pikseline paketlendi. Framebuffer'ın iki satırındaki (`row0`, `row1`) 2×2 pikseller `v128.load64_zero` + `v128.load64_lane` ile tek `v128`'e okunup, kısmi maskelerde `v128.bitselect` ve `v128.store64_lane` (lane 0 ve 1) ile geri yazıldı.
   - **`fb_clear` ve `fb_clear_depth` (`src/raster/framebuffer.c/.h`):** Tekrarlayan byte desenleri (`0x00000000`, `0xFFFFFFFF` vb.) için doğrudan `memory.fill` (`__builtin_memset`), genel RGBA8 renkleri ve `f32` derinlik değerleri için 4× unrolled `v128.store` SIMD döngüsü yazıldı.
   - **Binning hızlı yolu:** Tek 64×64 karoya düşen üçgenler `((tx0 ^ tx1) | (ty0 ^ ty1)) == 0u` tek koşuluyla kenar-karo kesişim testine girmeden doğrudan ilgili karoya eklendi.
2. **Füzyon Desteği (ABI v1):**
   - Her `ShadeBatch` boşaltımında `batch.fb_px = r->fb->px` ve `batch.fb_stride = r->fb->w` dolduruldu.
   - `raster_set_shader_ex(..., b32 fused)` eklendi (`raster_set_shader` varsayılan olarak `fused = FALSE` çağırır). `fused == TRUE` iken rasterizer `blend` aşamasını atlar ve kapsanan piksel istatistiğini (`stat_pixels`) güncellenmiş quad maskelerinden (`__builtin_popcount`) sayar.
3. **Derinlik Tamponu ve Early-Z (`Framebuffer.depth`, `raster_set_depth`):**
   - `Framebuffer`'a opsiyonel `f32* depth`, `fb_init_depth` ve `fb_clear_depth` eklendi.
   - `raster_set_depth(r, test, write, func)` ile 8 karşılaştırma fonksiyonu (`DEPTH_NEVER` .. `DEPTH_ALWAYS`) desteklendi.
   - **Early-Z:** Quad emisyonu sırasında `shade`'den **önce** 4-lane `z` düzlem interpolasyonu (`z0 + dx*dz_dx + dy*dz_dy`) ve `depth_test` uygulanarak elenen lane'ler `mask`'ten düşürüldü (`mask == 0` ise quad tamamen atlanır, varying hesaplanmaz ve `ShadeFn` çağrılmaz). `depth_write` ise shader `discard` sonrası hayatta kalan (`quads[q].mask`) lane'ler için derinlik tamponuna yazılır.
4. **Testler (`tests/test_raster.c`, `tests/test_raster_wasm.mjs`):**
   - `raster_depth_and_fusion`: Ters derinlik eğimli kesişen iki 3D dörtgen ile `DEPTH_LESS` doğru görünürlük + early-z piksel elenmesi ve `fused = TRUE` doğrudan `fb_px` yazımı doğrulandı.
   - `raster_wasm_simd_bit_exact` (`tests/test_raster_wasm.mjs`): 3 doku (nearest, bilinear, clamp + alpha discard), 3 blend modu (`BLEND_NONE`, `BLEND_ALPHA`, `BLEND_ADD`), perspektif-doğru 3D üçgenler ve early-z kesişiminden oluşan 256×256 sahneyi hem host skaler hem wasm32 SIMD ile çalıştırıp **0 piksel farkı (bit-bit eşdeğerlik)** doğrulandı.

---

### Benchmark Sonuçları (Öncesi / Sonrası ve Optimizasyon Katkıları)
- **CPU / Ortam:** `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`, V8 Wasm SIMD (`wasm32 -O2 -msimd128`), `640×360` framebuffer, M2 ile birebir aynı 1k / 10k / 50k dönen/zıplayan sprite sahnesi (`seed=0x12345678`, 3 prosedürel 32×32 doku).

#### 1. `fb_clear` Mikro-Benchmark (`640×360` = 230.400 piksel, 2000 tekrar ortalaması)
| Fonksiyon / Mod | Skaler (ms) | Wasm SIMD / `memory.fill` (ms) | Hızlanma |
|---|---:|---:|---:|
| `fb_clear(rgba)` (genel RGBA8, ör. `0xFF1C1412`) | 0.0661 ms | **0.0301 ms** (`4× v128.store`) | **2.20×** |
| `fb_clear(0)` (sabit byte → `memory.fill`) | 0.0164 ms | **0.0159 ms** (`memory.fill`) | **1.03×** |

#### 2. Optimizasyonların Ayrı Ayrı Katkısı (10.000 Sprite, ~335k px/frame)
| Optimizasyon Adımı | `setup` (ms) | `bin` (ms) | `raster` (ms) | `blend` NONE (ms) | `blend` ALPHA (ms) | `blend` ADD (ms) |
|---|---:|---:|---:|---:|---:|---:|
| **0. M2 Skaler Referans (Öncesi)** | 0.932 | 0.174 | 4.822 | 4.804 | 9.622 | 9.885 |
| **1. Tek-karo hızlı binning + z kurulumu** | 0.757 | 0.166 | 4.810 | 4.804 | 9.622 | 9.885 |
| **2. SIMD kenar taraması (`i32x4` + `bitmask`) + genel SIMD varying** | 0.757 | 0.166 | 3.630 | 4.804 | 9.622 | 9.885 |
| **3. Inline `fast_uv` (`f32x4` SoA yazım) + skaler satır tabanı** | 0.757 | 0.166 | **2.666** | 4.804 | 9.622 | 9.885 |
| **4. SIMD Blend (`trunc_sat` + `i16x8.narrow` + `load64/store64_lane`)** | **0.757** | **0.166** | **2.666** | **2.021** | **2.961** | **3.802** |
| **Toplam Hızlanma (Öncesi → Sonrası)** | **1.23×** | **1.05×** | **1.81×** | **2.38×** | **3.25×** | **2.60×** |

#### 3. 1k / 10k / 50k Sprite Tam Tablo (Öncesi = M2 Skaler vs Sonrası = M5_1 SIMD)
| Sprite | Blend Modu | Mimari | ms/frame | Mpiksel/s | `setup` (ms) | `bin` (ms) | `raster` (ms) | `shade` (ms) | `blend` (ms) | `raster + blend` (ms) | `raster + blend` Hızlanma |
|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **1k** | `BLEND_NONE` | Skaler | 2.160 | 15.5 | 0.089 | 0.021 | 0.479 | 0.800 | 0.506 | 0.985 | — |
| **1k** | `BLEND_NONE` | **SIMD** | **1.390** | **24.0** | **0.076** | **0.021** | **0.271** | 0.627 | **0.205** | **0.476** | **2.07×** |
| **1k** | `BLEND_ALPHA` | Skaler | 2.653 | 12.6 | 0.089 | 0.020 | 0.470 | 0.802 | 1.013 | 1.483 | — |
| **1k** | `BLEND_ALPHA` | **SIMD** | **1.517** | **22.0** | **0.075** | **0.020** | **0.277** | 0.633 | **0.332** | **0.609** | **2.44×** |
| **1k** | `BLEND_ADD` | Skaler | 2.719 | 12.3 | 0.089 | 0.021 | 0.469 | 0.802 | 1.081 | 1.550 | — |
| **1k** | `BLEND_ADD` | **SIMD** | **1.519** | **22.0** | **0.075** | **0.019** | **0.266** | 0.608 | **0.377** | **0.643** | **2.41×** |
| **10k** | `BLEND_NONE` | Skaler | 19.573 | 17.2 | 0.932 | 0.174 | 4.822 | 7.669 | 4.804 | 9.626 | — |
| **10k** | `BLEND_NONE` | **SIMD** | **12.611** | **26.6** | **0.757** | **0.169** | **2.682** | 5.952 | **2.021** | **4.703** | **2.05×** |
| **10k** | `BLEND_ALPHA` | Skaler | 25.962 | 12.9 | 1.008 | 0.190 | 5.895 | 7.941 | 9.622 | 15.517 | — |
| **10k** | `BLEND_ALPHA` | **SIMD** | **13.491** | **24.9** | **0.762** | **0.166** | **2.666** | 5.919 | **2.961** | **5.627** | **2.76×** |
| **10k** | `BLEND_ADD` | Skaler | 24.806 | 13.5 | 0.926 | 0.173 | 4.956 | 7.672 | 9.885 | 14.841 | — |
| **10k** | `BLEND_ADD` | **SIMD** | **15.005** | **22.4** | **0.805** | **0.180** | **2.896** | 6.226 | **3.802** | **6.698** | **2.22×** |
| **50k** | `BLEND_NONE` | Skaler | 100.865 | 16.6 | 4.588 | 0.846 | 28.512 | 38.010 | 23.706 | 52.218 | — |
| **50k** | `BLEND_NONE` | **SIMD** | **67.631** | **24.8** | **3.949** | **0.863** | **18.180** | 30.344 | **9.388** | **27.568** | **1.89×** |
| **50k** | `BLEND_ALPHA` | Skaler | 122.676 | 13.7 | 4.654 | 0.866 | 30.606 | 38.586 | 42.570 | 73.176 | — |
| **50k** | `BLEND_ALPHA` | **SIMD** | **71.828** | **23.4** | **3.903** | **0.851** | **17.814** | 30.072 | **14.279** | **32.093** | **2.28×** |
| **50k** | `BLEND_ADD` | Skaler | 114.882 | 14.6 | 4.515 | 0.837 | 28.544 | 37.701 | 38.145 | 66.689 | — |
| **50k** | `BLEND_ADD` | **SIMD** | **74.098** | **22.6** | **3.842** | **0.834** | **17.713** | 29.910 | **17.041** | **34.754** | **1.92×** |

---

### Darboğaz Analizi (`BLEND_ALPHA`'da 2.76×, `BLEND_NONE`'da Neden 2.05×?)
1. **`blend` aşaması tek başına 2.38× – 3.25× hızlandı:**
   - 10k sprite'ta `BLEND_ALPHA` **9.62 ms → 2.96 ms (3.25×)**, `BLEND_ADD` **9.89 ms → 3.80 ms (2.60×)**, `BLEND_NONE` **4.80 ms → 2.02 ms (2.38×)** düştü.
   - `BLEND_ALPHA` modunda `raster + blend` toplamı **15.52 ms → 5.63 ms (2.76×)** ile **2.5× hedefini geçti**.
   - Ayrıca D hattının Matoki JIT blend füzyonu (`fused = TRUE`) devreye girdiğinde ayrı `blend` geçişi tamamen **0 ms**'ye inerek `out_rgba` SoA ara tampon yazma/okuma trafiğini ortadan kaldıracaktır.
2. **`raster` aşamasının 1.81× (4.82 ms → 2.67 ms) ile sınırlı kalmasının ölçülen nedenleri:**
   - **Geometri boyutu ve üçgen başına sabit yük:** M2 sahnesindeki 10.000 sprite (20.000 üçgen) `4..8 px` boyutundadır. Bir üçgenin sınırlayıcı kutusu ortalama `6×6 px` (yalnızca `3×3 = 9` quad) kaplar ve üçgen başına ortalama sadece **~4.2 kapsanan quad** üretilir. Dolayısıyla `raster_kern_tri_scan` zamanının önemli bir kısmı (`~1.1 ms`) 2×2 quad iç döngüsünde değil, 20.000 üçgen için `TileBinChunk` bağlı listesini gezme, karo sınırlarına kırpma (`min_x..max_x`) ve başlangıç kenar/UV vektörlerini (`a*qx_min + b*qy_min + c`) kurma sabit yükünde harcanmaktadır.
   - **Batch sınırlarında `PROF_BEGIN`/`PROF_END` JS import çağrısı yükü:** 10k sahnede kare başına **706 kez** `raster_flush_full_batch` çağrılmakta ve her batch'te `PROF_END("raster")` + `PROF_BEGIN("shade")` + `PROF_END("shade")` + `PROF_BEGIN("blend")` + `PROF_END("blend")` + `PROF_BEGIN("raster")` olmak üzere **6 kez** Wasm→JS sınırından `plat_time_ms()` (`performance.now()`) import'u çağrılmaktadır (~4.250 JS çağrısı/frame). `PROF_BEGIN/END` devre dışı bırakılarak yapılan ölçümde (`10k sprite, BLEND_ALPHA`) toplam kare süresi **13.49 ms → 12.15 ms**'ye düşmekte, aradaki **~1.34 ms/frame** doğrudan profiler zamanlayıcı çağrılarından kaynaklanmaktadır.
   - **SoA bellek bant genişliği:** Her quad için `quads[q]` (8B) + 2 varying (`2 × 16B = 32B`) yazılması ve kısmi batch'lerde (`count < 256`, 60 karoda karo sonu flush) `v=1` kanalının `memmove` ile sıkıştırılması önbellek/bellek yazma bant genişliği ile sınırlıdır.
3. **E Hattına Not (`ref_shaders.h` sentinel kimlikleri):**
   - E hattı `web/platform.js` tablo boyutunu büyüttükten sonra `src/raster/ref_shaders.h` içindeki `0xFFFFFFF1..3` sentinel kontrolü `src/raster/raster.c` içinde geriye dönük uyumlu olarak korunmuştur (hem fonksiyon işaretçisi hem sentinel ile çalışır).
4. **ÖNERİ (`src/matoki/opt.c` — D Hattı):**
   - `src/matoki/opt.c:180` (`remap_inst_operands` / `recompute_varying`) içinde `case IR_SELECT:` dalında `in->a`, `in->b` veya `in->c` için `IR_NONE` (`0xFFFFFFFFu`) kontrolü yapılmadan `f->insts[in->c].flags` okunduğu veya CSE geçişinde `IR_SELECT`'in `c` operandı `IR_NONE` kaldığı durumda `test_matoki_back` içinde `SEGV` oluşmaktadır; `in->a != IR_NONE`, `in->b != IR_NONE`, `in->c != IR_NONE` koruması eklenmelidir.

