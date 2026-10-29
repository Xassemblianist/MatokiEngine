# F0_G — Belirlenimcilik kanıtı (H7, D-112)

Durum: TODO

## Amaç
`src/sim` (M16_A) + mevcut `phys` + `scene` ECS ile 10 000 tick'lik bir sahne: {1T, 2T, 8T} iş parçacığı ve
{host, Node wasm, başsız Chrome} (Firefox varsa ekle) üzerinde **aynı hash dizisi**. Bozan yer çıkarsa en küçük
örnekle kökünü bul (sıra, FMA, NaN, dinamik iş dağıtımı) ve Notlar'a yaz; düzeltme küçükse yap (sim birimlerine
`-ffp-contract=off` dahil). `make detcheck` hedefi.

## Dosyalar
- `tests/test_det.c` (yeni), `tools/host/det_check.mjs` (yeni; `webgpu_check.mjs` desenini kullanabilir), `Makefile`
  (yalnız `detcheck` hedefi ve sim birimleri için `-ffp-contract=off`), `src/sim/*` (yalnız küçük düzeltmeler)

## Kabul kriterleri
- [ ] `make check` + `make detcheck` hatasız
- [ ] Hash tablosu (ortam × iş parçacığı) Notlar'da; farklıysa kök neden
