# G1_B — GPU'da yaşayan sahne + gfx3d WebGPU arka ucu (H5)

Durum: TODO

## Amaç
ARCH_V2 H5: "GPU'da yaşayan sahne CPU'yu boşaltır". G1_A'nın altyapısını genelleştir:
- Örnek (instance) tamponu GPU'da kalıcı; CPU yalnız değişen dönüşümleri yükler (dirty list).
- Compute ile frustum + Hi-Z oklüzyon kırpma → dolaylı çizim argümanları (multi-draw-indirect varsa kullan, yoksa döngü).
- Kümelenmiş ileri aydınlatma (clustered forward), PBR (pbr3d.mtk WGSL), gölge haritası (CSM 3 kademe).
- "5:3D" sahnesi ve Kuyu/Ocak WebGPU varsa GPU'da çizilir; yoksa CPU yolu.
- 100k örnek benchmark sahnesi (tuşla): CPU gönderim süresi ölç.

## Dosyalar
`src/gpu/*`, `web/gpu.js`, `src/gfx3d/*`, `src/app/scene3d.*`, `src/app/r3d.*`, `src/app/scene_kuyu.c`, `src/app/scene_ocak.c`, `src/app/main.c`, `shaders/*.mtk`, `tests/*`

## Kabul kriterleri
- [ ] 100k örnek: CPU gönderimi < 0.5 ms, GPU kare süresi Notlar'da (H5 sonucu ARCH_V2'ye işlenir)
- [ ] 3D/Kuyu/Ocak 1080p ekran görüntüleri GPU yolunda doğru
- [ ] make check + make gpucheck temiz
