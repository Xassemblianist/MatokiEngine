# F0_A — Matoki `compute` çekirdekleri: wasm + WGSL (H2)

Durum: TODO (M15_A ve M14_B merge edildikten sonra başlar)

## Amaç
ARCH_V2 §3: Matoki'ye `compute` çekirdek türü: N eleman üzerinde SPMD, `buffer<T>` okuma/yazma (f32/u32/vec4),
`global_id`, u32 atomik toplama. CPU hedefi: wasm SIMD (4 lane) + `job_parallel_for` ile parçalama. GPU hedefi: M15_A'nın
WGSL üreticisiyle `@compute @workgroup_size(64)`. Üç örnek çekirdek: parçacık güncelleme, prefix-sum (iki geçiş),
skinning (köşe × eklem matrisi).

## Dosyalar
- `src/matoki/*`, `tests/test_matoki_compute.c` (yeni), `shaders/compute/*.mtk` (yeni), `docs/MATOKI.md`

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] Her çekirdek için C referans fonksiyonu; wasm çıktısı referansla tolerans içinde aynı (test)
- [ ] WGSL çıktıları `build/wgsl/compute_*.wgsl`; GPU doğrulaması yöneticide
- [ ] CPU ölçümü: 1T ve 8T, C referansına göre hız (Notlar'a; makine yükünü not et)
