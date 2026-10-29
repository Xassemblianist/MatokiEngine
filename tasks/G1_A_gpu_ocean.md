# G1_A — Okyanus WebGPU'da tam çözünürlükte (ilk GPU sahnesi)

Durum: DONE

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
- [x] Headless Chrome + Vulkan (RTX 2060) 1920×1080 ekran görüntüsü: dalgalar, köpük, güneş parlaması, tekne net (gözle bak, build/ altına kaydet)
- [x] GPU kare süresi ölçümü (timestamp-query) Notlar'da; hedef 1080p < 4 ms
- [x] WebGPU kapalıyken (`--disable-features=WebGPU` veya SwiftShader) CPU yolu çalışıyor
- [x] make check + make gpucheck temiz

## Notlar
- **Yapılanlar:**
  - **RHI & WebGPU Genişletmesi (`src/gpu/rhi.*`, `web/gpu.js`, `src/matoki/wgsl.c`):** Compute pipeline ve compute pass ikili komutları (`RHI_CMD_CREATE_COMPUTE_PIPELINE`..`RHI_CMD_END_COMPUTE_PASS`, opcode `20..24`), storage tampon/doku bağlamaları (`RHI_BIND_STORAGE_BUFFER`, `RHI_BIND_STORAGE_TEXTURE`), `rgba16float`/`rgba32float` doku formatları, MSAA 4× render pass (`sample_count = 4`) ve `timestamp-query` GPU zamanlayıcı sorgusu (`GPUQuerySet` ile compute + render pass toplam GPU süresi) eklendi. `mtk_compile_wgsl` içinde `@compute` çekirdeklerinin `mtk_wgsl_compute`'a yönlendirilmesi bağlandı.
  - **GPU Okyanus Hattı (`src/gpu/gpu_ocean.*`, `shaders/ocean.mtk`, `shaders/sky.mtk`):**
    - 3 kademeli Tessendorf FFT (128×128 @ 250 m, 128×128 @ 42 m, 64×64 @ 8 m; kademe başına 8 gerçek alan / 4 Hermitsel karmaşık alan) Matoki `@compute` çekirdekleri (`ocean_spectrum.mtk`, `ocean_fft_stage.mtk`, `ocean_unpack.mtk`) üzerinden WGSL'e derlenerek GPU'da koşturuldu; sonuçlar `rgba16float` storage dokularına (`disp_tex_0..2`, `casc_tex_0..2`) yazıldı.
    - Kamera merkezli 160×160 geometrik ($s^4$) okyanus ızgarası (52.681 üçgen) köşe gölgelendiricisinde storage dokularından uzaklık ağırlıklı örnekleme ile GPU'da yer değiştirildi.
    - `shaders/ocean.mtk`, `shaders/sky.mtk`, `shaders/lit3d.mtk` ve `shaders/unlit3d.mtk` çalışma anında `mtk_compile_wgsl` ile WGSL'e derlenerek tekne ağları (10 parça: V-tabanlı gövde, güverte, kamara, ışıklı fener pencereleri, direk, bumba, civadra, kırmızı flama, şişkin ana ve flok yelkenleri), okyanus yüzeyi ve atmosferik gök kubbesi MSAA 4× ile çizildi.
  - **Otomatik Seçim, Geri Düşüş ve Tam Çözünürlük Katmanı (`src/app/scene_ocean.c`, `web/index.html`, `web/platform.js`):**
    - `#gpu-screen` WebGPU tuvali `devicePixelRatio` ile ekranın doğal çözünürlüğünde (`1920x1080`) altta, `#screen` 2B HUD tuvali üstte saydam katman olarak hizalandı.
    - WebGPU aktifken 9:Okyanus sahnesi GPU yolunu kullanır, kaldırma kuvveti fiziği belirlenimcilik (D-112) için CPU FFT üzerinde kalır; WebGPU kapalıyken (`--disable-features=WebGPU`) mevcut CPU yolu ve HUD (`CPU 640x360`) kesintisiz çalışır.
- **Ertelenenler:** Yok.
- **Ölçümler ve Doğrulama:**
  - **1920×1080 WebGPU + Vulkan (RTX 2060) `timestamp-query` GPU Kare Süresi:**
    - Koşu 1 (`uptime` load average: `2.55, 2.77, 3.07`): 20 kare ortanca GPU süresi **`1.321 ms`** (son kare `1.320 ms`, hedef `< 4 ms`), `resolution: "1920x1080"`, `hasTimestamp: true`, `tris: 52681`, `ring: { write: 0, read: 0, dropped: 0 }`.
    - Koşu 2 (`uptime` load average: `2.90, 2.68, 2.90`): 20 kare ortanca GPU süresi **`2.176 ms`** (< 4 ms).
    - Görsel doğrulama: `build/ocean_gpu_1080p.png` incelendi; 1080p MSAA 4× dalgalar, Jacobian beyaz köpük, güneş parlaması, su altı turkuaz saçılımı, yüzen yelkenli tekne ve `GPU 1920x1080 | GPU 1.32ms (ts)` HUD satırı net.
  - **CPU Geri Düşüşü (`--disable-features=WebGPU`):** `mode: "CPU"`, `gpuAvailable: false`, `resolution: "640x360"`, `tris: 4349`, `ok: true` (`build/ocean_cpu_fallback.png`).
  - **Birim ve Entegrasyon Testleri:** `TEST(ocean_gpu_matoki_compute_fft)` ile Matoki `@compute` FFT çekirdeklerinin CPU referans `ocean_fft_2d` ile uyumu (`max_err < 2e-3`) ve RHI compute/storage kodlama-çözme testleri doğrulandı (`312 geçti, 0 kaldı`, `make check` + `make gpucheck` + `make gpucheck GPU=1` 0 hata / 0 uyarı).
