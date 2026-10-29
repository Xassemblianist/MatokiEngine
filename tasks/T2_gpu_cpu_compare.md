# T2 — CPU ↔ GPU karşılaştırma testi

Durum: DONE

## Amaç
Aynı Matoki çekirdeğinin (pixel + compute, shaders/ ve shaders/compute/ içindekilerin hepsi) CPU JIT çıktısı ile
WebGPU çıktısını headless Chrome'da (Vulkan + SwiftShader) karşılaştır; piksel başına tolerans (ör. 2/255) ve hata haritası
build/ altına PNG. `make gpucompare` hedefi; make gpucheck'e bağlanır.

## Dosyalar
`tools/host/webgpu_check.mjs`, `tools/host/gpu_compare.mjs` (yeni), `web/gpu_test.html`, `web/gpu.js`, `src/gpu/gpu_test.c`, `Makefile`
Ek zorunlu dosya: `src/matoki/wgsl.c` (texture2d_f32 derinlik dokusu WGSL texel yüklemesi düzeltmesi için).

## Kabul kriterleri
- [x] Tüm çekirdekler için fark tablosu Notlar'da; aşan varsa hatası düzeltildi ya da gerekçeli
- [x] make check + make gpucompare temiz

## Notlar
### Yapılanlar
1. **CPU ↔ GPU Karşılaştırma Mimarisi:**
   - 12 pixel shader (`shaders/*.mtk`) ve 4 compute shader (`shaders/compute/*.mtk`) olmak üzere toplam 16 çekirdeğin tamamı CPU Wasm JIT ve WebGPU RHI üzerinde aynı girdi/üniform/doku durumlarıyla çalıştırıldı.
   - `src/gpu/gpu_test.c` içinde her iki arka uçtan üretilen çıktılar karşılaştırılarak fark metriği (maksimum fark, ortalama fark, tolerans aşım adedi) hesaplandı ve 128x128 RGBA8 fark haritası (`diff_map`) üretildi.
   - `tools/host/gpu_compare.mjs` aracı yazılarak Headless Chrome üzerinden `gpu_test.html?compare=1` otomasyonu kuruldu; 16 çekirdeğin 128x128 fark haritaları geçerli PNG dosyaları olarak `build/diff_<name>.png` altına kaydedildi ve karşılaştırma anı ekran görüntüsü `build/gpu_compare_shot.png` olarak alındı.
   - `web/gpu.js` içine `readbackBufferData(handle, byteLength)` fonksiyonu eklendi; açık kalan komut tamponlarının GPU'ya teslimi güvenceye alındı.
   - `web/gpu_test.html` içine `runCompare()` fonksiyonu ve URL tetikleme modu eklendi.
   - `Makefile`'a `gpucompare` hedefi eklendi ve `gpucheck` hedefi `gpucompare`'i de çalıştıracak şekilde bağlandı.

2. **Düzeltilen Hatalar ve Ek Dosya Gerekçesi:**
   - `src/matoki/wgsl.c`: `texture2d_f32` (tek kanal float doku) kullanan `pbr3d.mtk` çekirdeğinde, `IR_TEXEL_LOAD` komutunun `pack4x8unorm` ile float değeri 8-bit kanallara bölüp bitcast etmesi sonucu oluşan bozulma giderildi. Float dokularda doğrudan `bitcast<i32>(textureLoad(...).r)` üretilerek bitcast doğruluğu sağlandı ve `pbr3d` farkı 0.000000'a indi.
   - `src/gpu/gpu_test.c`: Compute shader bind group binding numaraları Matoki WGSL sözleşmesine (`MTK_WGSL_BUF_BINDING(s) = 1u + s`) hizalandı; `rhi_present` ile komut akışı teslim edildi. Doku ve sampler kaynakları `engine_gpu_compare_init_gpu` ile GPU'ya gönderildi.

### Karşılaştırma Sonuç Tablosu (make gpucompare — Headless Chrome / SwiftShader & RTX 2060 Vulkan)

#### Standart Headless (SwiftShader / Vulkan)
| # | Çekirdek | Tür | Eleman | Max Fark | Ort Fark | Tolerans | Durum |
|---|---|---|---|---|---|---|---|
| 0 | `branchy.mtk` | Pixel | 16384 | 0.003922 | 0.000980 | 0.007843 (2/255) | **PASS** |
| 1 | `particle.mtk` | Compute | 1024 | 0.000000 | 0.000000 | 0.000100 (1e-4) | **PASS** |
| 2 | `prefix_sum_add.mtk` | Compute | 1024 | 0.000000 | 0.000000 | 0.000000 (uint) | **PASS** |
| 3 | `prefix_sum_scan.mtk` | Compute | 1024 | 0.000000 | 0.000000 | 0.000000 (uint) | **PASS** |
| 4 | `skinning.mtk` | Compute | 512 | 0.000000 | 0.000000 | 0.000100 (1e-4) | **PASS** |
| 5 | `dungeon3d.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 6 | `lit3d.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 7 | `ocean.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.011765 (3/255) | **PASS** |
| 8 | `particle3d.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 9 | `pbr3d.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 10 | `plasma.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.011765 (3/255) | **PASS** |
| 11 | `sky.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 12 | `solid.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 13 | `sprite.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 14 | `unlit3d.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |
| 15 | `vcolor.mtk` | Pixel | 16384 | 0.000000 | 0.000000 | 0.007843 (2/255) | **PASS** |

#### Donanım Doğrulaması (GPU=1 make gpucompare — NVIDIA RTX 2060 / Vulkan)
- Tüm 16 çekirdek PASS. Donanım rasterizer interpolasyonu kaynaklı maksimum fark piksel çekirdeklerinde 0.003922 (1/255), ortalama fark < 0.002941. Compute çekirdeklerinde fark 0.000000.
- `uptime` yükü test anında: load average: 5.59, 3.10, 2.81.
- `make check`: 313 test kusursuz geçti.
- Fark PNG haritaları `build/diff_*.png` ve ekran görüntüsü `build/gpu_compare_shot.png` altında üretildi.
