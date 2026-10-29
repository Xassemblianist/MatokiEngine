# M14_A — Paralel karo dağıtımı (bin) + yalnız-derinlik hızlı yolu

Durum: DONE

## Amaç
D-105 sonrası 8T'de seri kalan iki raster maliyetini azalt:
1. `bin_tris` (~0.35 ms/kare, ana iş parçacığı): karolara dağıtımı paralelleştir (ör. karo satırı/sütun bandı başına
   iş; her iş yalnız kendi karolarına yazar, üçgenler gönderim sırasıyla gezilir → karo içi sıra korunur).
2. Gölge geçişi (`r3d_shadow` ~2.5 ms, shade_fn == NULL, varying 0): shader'sız, yalnız derinlik yazan
   üçgenler için özel hızlı tarama yolu (quad/ShadeBatch kurulumunu atla, SIMD kenar testi + derinlik).

## Dosyalar
- `src/raster/raster.c`, `src/raster/raster.h`, `src/raster/raster_simd_wasm.c`
- `tests/test_raster*.c` (yeni test eklenebilir)
- `docs/DECISIONS.md` (yeni D-106 kaydı ekle)

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Çıktı eski yolla bit düzeyinde aynı (renk + derinlik): mevcut `gfx3d_deferred_matches_immediate` ve gölge
      testleri geçmeli; yeni yollar için ayrıca karşılaştırma testi
- [x] Önce/sonra ölçüm (host -O2 bench veya test içi zamanlama): bin süresi ve yalnız-derinlik tarama süresi

## Notlar

### Yapılanlar
1. **Dilim Birleştirmeli ve Paralel Karo-Şeridi Binleme (`raster_flush_bins`, D-106):**
   - `g3d_flush` (ve `raster_process_tri_buf`) tarafından yapılan çoklu `raster_bin_tris` çağrıları artık kurulmuş
     `(BinnedTri* tris, u32 count)` dilimlerini `RasterSpanChunk` (128 dilim/blok, `ARENA_PUSH_ARRAY_RAW`) listesine
     O(1) sürede ekler. Böylece geçiş başına ~1000 kez `PROF_BEGIN("bin")`/`plat_time_ms()` sınır geçişi ve
     `TileBinChunk` sıfırlama (`memset`) maliyeti ortadan kaldırıldı.
   - `raster_flush` başında çağrılan `raster_flush_bins(r)`, bekleyen tüm dilimleri tek seferde dağıtır:
     - `nthreads <= 1` veya `< 32` üçgende `bin_spans_serial` çalışır.
     - `nthreads > 1` olduğunda karo ızgarası ayrık şeritlere (`tiles_y < 8 && tiles_x >= 4` ise satır başına sol/sağ
       2 şerit = 640×360'ta 12 şerit; 512×512'de 8 satır şeridi) bölünür ve `job_parallel_for` ile dağıtılır.
     - Her şerit görevi yalnızca kendi karolarına yazar ve tüm dilimleri gönderim sırasıyla gezdiği için karo içi
       üçgen sırası seri yolla **bit düzeyinde aynıdır**. `TileBinChunk` düğümleri `r->frame` kalan alanından
       `atomic_add_u32` ile kilitsiz çekilir ve iş bitiminde `r->frame->used` tam kullanılan blok sayısı kadar
       ilerletilir.
2. **Yalnız-Derinlik (`shade_fn == NULL`) Hızlı Tarama Yolu (`raster_scan_depth_only_simd` / `raster_scan_depth_only_scalar`):**
   - `state->shade_fn == NULL` iken `QuadHeader`/`ShadeBatch` kurulumu ve genel `emit_quad_*` çağrısı tamamen atlanır.
   - `DEPTH_LESS`, `DEPTH_GREATER` ve testsiz yazım (`!depth_test || DEPTH_ALWAYS`) derleme zamanı sabitleriyle
     özelleştirilerek quad başına `switch (state->depth_func)` dallanması kaldırıldı.
   - **8×8 Trivial-Accept blokları:** 4 quad sütununun `zx0..zx3` tabanları blok başına bir kez hesaplanır; Wasm
     SIMD'de her 2 satırlık dilimde 16 piksel 4 adet tam 128-bit `wasm_v128_load` + `simd_depth_cmp` +
     `wasm_v128_bitselect` + `wasm_v128_store` ile doğrudan derinlik tamponuna yazılır (eski yoldaki 16× `load64` +
     8× `store64` + 16 fonksiyon çağrısı yerine). Host skaler yolda da dallanmasız (`?:`) güncelleme ile derleyicinin
     8'li döngüyü otomatik vektörleştirmesi sağlandı.
   - **Kısmi 8×8 bloklar ve `<16×16` küçük üçgenler:** Satır başına `dy_z`, `drow0`, `drow1` dış döngüye alındı;
     Wasm SIMD'de çift `load64` ve 4 komutluk maske kurulumu kaldırılıp tek `load64` + `k_quad_mask_lut[mask]` +
     `simd_depth_cmp` + `bitselect` ile birleştirildi. Aynı çift `load64` temizliği genel early-Z yoluna da uygulandı.
3. **Testler (`tests/test_raster.c`):**
   - `raster_depth_only_fast_path_bit_exact`: 256×256 üzerinde 400 üçgenle (küçük `<16×16`, kısmi 8×8 ve büyük 8×8
     trivial-accept) `DEPTH_LESS`, `DEPTH_GREATER`, `DEPTH_LEQUAL`, `DEPTH_GEQUAL`, `DEPTH_ALWAYS` ve
     `depth_write=FALSE` modlarında `shade_fn == NULL` hızlı yolunu genel quad/batch yoluyla (`shade_noop_fused`)
     bit-bit karşılaştırır (`diffs == 0`).
   - `raster_parallel_binning_bit_exact`: 640×360 (12 şerit) üzerinde 80 dilim × 6 üçgenle `1T`, `4T` ve `8T`
     binleme + raster çıktısını (`stat_tris_binned`, `stat_quads`, `stat_pixels`, `fb.px`, `fb.depth`) bit-bit doğrular.
   - `raster_m14a_wasm_simd_and_bench`: Wasm32 SIMD altında 512×512 6000 üçgenlik gölge geçişinde hem `4T` paralel
     binlemeyi hem de `shade_fn == NULL` hızlı yolunu referansla bit-bit (`diff == 0`) doğrular ve medyan süreleri ölçer.

### Benchmark Sonuçları (512×512 gölge geçişi, 1000 çizim dilimi × 6 üçgen = 6000 üçgen, 8407 binlenen üçgen)

| Ortam / Mod | `bin` Önce (ms) | `bin` Sonra (ms) | Hızlanma (`bin`) | `shadow_scan` Önce (ms) | `shadow_scan` Sonra (ms) | Hızlanma (`scan`) | Toplam Gölge Önce (ms) | Toplam Gölge Sonra (ms) | Çıktı FNV-1a Hash |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Host `-O2` (1T skaler) | 0.1283 | 0.0770 | **1.67×** | 5.1538 | 3.7669 | **1.37×** | 5.2820 | 3.8439 | `0xbacf6066c5211fc4` (bit-bit aynı) |
| Wasm `-O3` SIMD (1T) | 0.2273 | 0.0979 | **2.32×** | 4.0152 | 2.1987 | **1.83×** | 4.2425 | 2.2965 | `0xabd974208c4b45c3` (bit-bit aynı) |
| Wasm `-O3` SIMD (4T `worker_threads`) | 0.2832 | 0.0827 | **3.42×** | 1.2533 | 0.7500 | **1.67×** | 1.5365 | 0.8327 | `0xabd974208c4b45c3` (bit-bit aynı) |
| Wasm `-O3` SIMD (8T `worker_threads`) | 0.2027 | 0.0681 | **2.98×** | 0.8272 | 0.5065 | **1.63×** | 1.0299 | 0.5746 | `0xabd974208c4b45c3` (bit-bit aynı) |
| Test içi Wasm `-O2` SIMD (1T, `make check`) | — | 0.108 | — | 4.969 (genel yol) | 2.170 (depth-only) | **2.29×** | — | — | `diff = 0` |

