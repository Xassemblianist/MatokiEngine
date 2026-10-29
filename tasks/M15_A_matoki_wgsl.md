# M15_A — Matoki → WGSL kod üretici (D-108)

Durum: TODO

## Amaç
Aynı `.mtk` kaynağından WebGPU fragment shader'ı (WGSL) üret. Matoki IR'ını (optimizasyon sonrası) girdi al;
skaler/SPMD anlam aynı kalsın (CPU'da quad başına 4 lane, GPU'da piksel başına bir çağrı). Uniform'lar tek bir
uniform buffer (std140/WGSL hizalama kuralları, MtkLayout ile uyumlu ofsetler belgelensin), dokular
`texture_2d<f32>` + `sampler` bind group'u, varying'ler `@location(n)`. `sample`/mip, `dFdx/dFdy` (→ `dpdx/dpdy`),
`discard`, trigonometri ve tüm yerleşikler. Ayrıca gfx3d varying sözleşmesine uyan basit bir WGSL vertex shader
üretici (mvp + model + öznitelikler) — G3dAttr listesine göre.

## Dosyalar
- `src/matoki/wgsl.c`, `src/matoki/wgsl.h` (yeni), `src/matoki/matoki.h` (yeni API bildirimi)
- `tests/test_matoki_wgsl.c` (yeni)
- `docs/MATOKI.md` (WGSL bölümü)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız
- [ ] `shaders/` altındaki 9 shader'ın hepsi WGSL'e çevriliyor; testte üretilen metin sözdizimi denetiminden geçer
      (kendi küçük denetleyicin: parantez/blok dengesi, bildirilen tüm tanımlayıcılar tanımlı, @group/@binding tekil)
- [ ] Üretilen WGSL'ler `build/wgsl/*.wgsl` olarak yazdırılabilir (test veya host aracı) — tarayıcıda doğrulanacak
- [ ] Bilinmeyen/desteklenmeyen IR komutu → derleme hatası (sessiz yanlış kod yok)
