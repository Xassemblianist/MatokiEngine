# M13_B — Paralel 3D ön uç (ertelenmiş çizim + g3d_flush) ve paralel gölge kodlama

Durum: DONE (Claude)

## Amaç
8 iş parçacığında kare süresine hâkim olan seri ön ucu (köşe + üçgen + kurulum ≈ 2.9 ms) ve seri gölge
kodlamasını job sistemine dağıtmak; çıktı bit düzeyinde aynı kalmalı.

## Dosyalar
- `src/raster/raster.c/.h` (`raster_setup_tri`, `raster_bin_tris`, `raster_pin_state`)
- `src/gfx3d/pipeline.c/.h` (`g3d_set_deferred`, `g3d_flush`, `g3d_set_slice_limit`)
- `src/gfx3d/shadow.c` (paralel kodlama)
- `src/core/arena.c/.h` (`arena_push_raw`, sıfırlamasız `arena_sub`) — görev sırasında gerekti
- `src/app/r3d.c/.h`, `src/app/main.c` (`engine_set_g3d_parallel`)
- `tests/test_gfx3d.c` (`gfx3d_deferred_matches_immediate`)
- `docs/DECISIONS.md` D-105

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Ertelenmiş çıktı anlık yolla bit düzeyinde aynı (renk, derinlik, istatistik), ara boşaltma ve yedek yol dahil
- [x] scene3d PBR + gölge, JIT açık: 1T ve 8T ölçümü, paralel açık/kapalı

## Notlar
- **Raster:** üçgen kurulumu `setup_tri_body` (saf, iş parçacığı güvenli) ve karo dağıtımı `bin_tris` olarak ayrıldı;
  dışarıya `raster_setup_tri`, `raster_bin_tris(…, submitted)`, `raster_pin_state` açıldı. Anlık yol değişmedi.
- **gfx3d:** `g3d_draw` gövdesi `vertex_range` + `tri_range` (+ `TriSink`) olarak bölündü; anlık ve ertelenmiş yol aynı
  kodu çalıştırır. Ertelenmiş modda köşe işleri 4096 köşe, üçgen işleri 1024 üçgen; çıktılar iş parçacığı dilimlerine,
  dağıtım ana iş parçacığında gönderim sırasıyla. Dilim tahmini (çıktı × 2, boş frame belleğinin yarısını geçmez);
  yetmezse iş ana iş parçacığında kesin kapasiteyle yeniden işlenir (`stats.items_fallback`).
- **Test:** `gfx3d_deferred_matches_immediate` — kırpılan zemin + 6 küre + 3 alfa harmanlı küp; renk, derinlik,
  g3d ve raster istatistikleri anlık yolla birebir. Ara boşaltma (kapasite 2) ve yedek yol (16 KB dilim) dahil.
- **Ölçüm** (Chrome, scene3d PBR + gölge, JIT açık, 80 kare medyanı, `engine_set_g3d_parallel` ile aç/kapa):

  | | paralel kapalı | paralel açık |
  |---|---|---|
  | 1T | 18.99 ms | 17.16 ms |
  | 8T | 10.14 / 10.34 ms | 7.09 / 7.14 ms |

  8T bölgeler: g3d_vertex 1.59 → 0.56 ms, g3d_tri + setup 1.31 → 0.38 ms, shadow_encode ort. 1.09 → 0.56 ms.
  1T kazancı köşe dizilerinin artık memset edilmemesinden (`arena_push_raw`).
- **Ertelenen:** karo dağıtımı (bin, ~0.35 ms) hâlâ seri; gölge geçişinin rasterı (r3d_shadow ~2.5 ms) sıradaki hedef.
  Kuyu sahnesi kendi g3d bağlamını kullanıyor, henüz ertelenmiş moda geçirilmedi.
