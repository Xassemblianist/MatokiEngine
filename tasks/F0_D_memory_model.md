# F0_D — Bellek modeli ölçümleri (H4)

Durum: TODO

## Amaç
ARCH_V2 §2 bellek modeli için sayılar. Tarayıcıda (wasm, paylaşımlı bellek) ve host'ta ölç:
`memory.copy` vs SIMD döngü vs `__builtin_memcpy` (1–64 MB), 16 MB durum bölgesi anlık görüntüsü, 8 iş parçacığıyla
paralel kopya, durum özeti (hash) hızı, `memory.grow` maliyeti, 4 GB'a yakın bellek davranışı; memory64'ün Chrome'da
bayrakla durumu (yalnız rapor).

## Dosyalar
- `tools/host/bench_memory.c` (yeni), `web/mem_bench.html` + `web/mem_bench.js` (yeni), `src/bench/bench_memory.c` (yeni)
- `Makefile` (yalnız yeni hedefler)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Host sonuçları tablo halinde Notlar'da; tarayıcı sayfası tek tıkla aynı tabloyu üretir
- [ ] H4 eşiği (16 MB < 2 ms) için ön değerlendirme
