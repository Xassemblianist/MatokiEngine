# G1_A — Okyanus WebGPU'da tam çözünürlükte (ilk GPU sahnesi)

Durum: TODO

## Amaç
Şu an tüm 3D sahneler CPU'da çiziliyor; WebGPU yolu (src/gpu/rhi, web/gpu.js, Matoki→WGSL M15_A, compute F0_A) var ama
sahnelere bağlı değil. "9:Okyanus" sahnesini WebGPU varsa GPU'da, ekranın doğal çözünürlüğünde (devicePixelRatio) çiz:
- FFT kademeleri GPU compute'ta (Matoki compute çekirdeği ya da gerekiyorsa elle WGSL; tercih Matoki → WGSL), sonuç
  depolama dokularına; köşe yer değiştirmesi vertex shader'da dokudan örnekleme, ekran-uzayı ızgara ya da kamera
  merkezli projeksiyonlu ızgara (ufka kadar, aliasing yok).
- `shaders/ocean.mtk` + `sky.mtk` WGSL'e; eksik dil özellikleri Matoki'ye eklenir (CPU JIT arka ucu bozulmadan).
- Tekne mesh'i GPU'da; kaldırma fiziği CPU'da kalır (yükseklik CPU FFT'sinden ya da GPU'dan geri okuma ile — belirlenimcilik
  için sim-kritik yol CPU'da, D-112).
- WebGPU yoksa mevcut CPU yolu aynen çalışır (geri düşüş). HUD: "GPU"/"CPU", çözünürlük, GPU zamanı (timestamp-query varsa).
- MSAA 4× + doğru sRGB çıkış.

## Dosyalar
`src/gpu/*`, `web/gpu.js`, `web/index.html`, `web/platform.js`, `src/app/scene_ocean.*`, `src/gfx3d/ocean.*`,
`shaders/ocean.mtk`, `shaders/sky.mtk`, `src/matoki/*` (yalnız eksik özellik), `tests/*`, `tools/host/webgpu_check.mjs`

## Kabul kriterleri
- [ ] Headless Chrome + Vulkan (RTX 2060) 1920×1080 ekran görüntüsü: dalgalar, köpük, güneş parlaması, tekne net (gözle bak, build/ altına kaydet)
- [ ] GPU kare süresi ölçümü (timestamp-query) Notlar'da; hedef 1080p < 4 ms
- [ ] WebGPU kapalıyken (`--disable-features=WebGPU` veya SwiftShader) CPU yolu çalışıyor
- [ ] make check + make gpucheck temiz
