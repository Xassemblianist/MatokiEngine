# M14_A — Paralel karo dağıtımı (bin) + yalnız-derinlik hızlı yolu

Durum: TODO

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
- [ ] `make check` hatasız + uyarısız
- [ ] Çıktı eski yolla bit düzeyinde aynı (renk + derinlik): mevcut `gfx3d_deferred_matches_immediate` ve gölge
      testleri geçmeli; yeni yollar için ayrıca karşılaştırma testi
- [ ] Önce/sonra ölçüm (host -O2 bench veya test içi zamanlama): bin süresi ve yalnız-derinlik tarama süresi
