# F0_E — Platform gerçeklik taraması (H6) — yalnız araştırma

Durum: DONE

## Amaç
D-109'daki özelliklerin Ekim 2026 itibarıyla Chrome/Edge, Firefox, Safari (masaüstü + mobil) kararlı sürümlerdeki
durumu: WebGPU (compute, timestamp-query, subgroups, shader-f16, bindless/indirect durumu, 64-bit atomik yok mu?),
wasm relaxed SIMD, threads, memory64, JSPI/stack switching, multi-memory, wasm GC (gerekmez ama not), tail call,
Atomics.waitAsync, OffscreenCanvas+WebGPU worker'da, WebCodecs (AV1 donanım çözme), WebTransport, WebNN.
Her satır için kaynak bağlantısı (MDN, chromestatus, caniuse, webgpureport.org, spec). Uydurma yok; bilinmiyorsa "?".
Sonunda ARCH_V2 için öneriler (hangi özellik tavan, hangisi bekle).

## Dosyalar
- `docs/PLATFORM_SURVEY.md` (yeni; tek dosya)

## Kabul kriterleri
- [x] Her hücrenin kaynağı var
- [x] WebGPU'da olmayan ama Unreal-sınıfı render için gereken şeyler ve geçici çözümleri listelenmiş

## Notlar
- `docs/PLATFORM_SURVEY.md` tek belge olarak oluşturuldu.
- D-109 ve ARCH_V2'de hedeflenen 21 özellik/API tek tek araştırıldı; MDN, CanIUse, ChromeStatus, W3C WebGPU/Wasm şartnameleri ve GitHub repo kayıtlarıyla doğrulandı.
- Öne çıkan gerçekler:
  1. **WebGPU Core & Compute:** Chrome 113+, Firefox 125+ (Windows/macOS) ve Safari 18+ (macOS Sequoia, iOS 18+) ile geniş tabana oturmuştur.
  2. **Timestamp Query:** Chrome 121+'te mevcuttur ancak güvenlik (zamanlama/fingerprinting saldırıları) nedeniyle varsayılan olarak 100 µs seviyesine kuantize edilmektedir; nanosaniye hassasiyeti için developer flag gerekir. Safari ve Firefox'ta gizlilik sebebiyle sınırlıdır.
  3. **Subgroups:** Chromium 144+'te kararlıdır; Firefox ve Safari'de henüz yoktur (tavan özelliği).
  4. **Shader-f16:** Chrome 120+ ve Safari 18.2+ (Apple Silicon Metal) üzerinde kararlıdır; Android'de Vulkan donanım sürücüsüne (`VK_KHR_shader_float16_int8`) bağlıdır.
  5. **Bindless & Indirect Count:** WebGPU V1'de bindless ve `drawIndirectCount` yoktur (Milestone 2 aşamasında). Geçici çözüm: `Texture2DArray` + Sanal Doku Sayfaları (Virtual Texturing) ve tavanlı (bounded) indirect çağrılarında GPU culling sonrası `instanceCount = 0` atamasıdır.
  6. **64-bit Atomikler:** WGSL'de `atomic<u64>` yoktur (Metal kısıtlaması). W3C `atomic-vec2u-min-max` önerisiyle 64-bit min/max operasyonlarını `atomic<vec2u>` ile sunmayı hedeflemektedir (Chromium Canary prototip). Kararlı çapraz çözüm: Two-Pass Visibility Buffer (Derinlik pass'i + `EQUAL` derinlik testli ID pass'i).
  7. **Wasm Relaxed SIMD & Memory64:** Chrome (114+, 133+) ve Firefox (146+, 134+) desteklemektedir; Safari'de henüz yoktur. Safari yedeği için motor `engine_relaxed64.wasm` (tavan) ve `engine_simd32.wasm` (taban) olmak üzere iki binary derlemelidir.
  8. **Wasm Threads & Atomics:** Tüm tarayıcılarda COOP/COEP başlıklarıyla tam kararlıdır (Baseline).
  9. **Tail Calls & Atomics.waitAsync & WebTransport:** Tüm ana tarayıcılarda Baseline aşamasındadır.
  10. **WebCodecs AV1:** Chrome ve Firefox'ta hem yazılım (libdav1d) hem donanım çözücü vardır; Safari'de ise YALNIZCA donanım AV1 çipi olan cihazlarda (M3+, A17 Pro/iPhone 15 Pro, iPhone 16) çalışır, yazılımsal fallback yoktur (H.264/HEVC yedeği şarttır).
  11. **WebNN:** Yalnızca Chromium tarafında deneyseldir; ARCH_V2'den şimdilik uzak tutulmalı, beklemede kalmalıdır.
- Unreal-sınıfı render (Nanite/Lumen) için WebGPU'da eksik olan 8 ana özellik (Mesh Shaders, 64-bit atomikler, Bindless, drawIndirectCount, Donanımsal RT, Barycentrics, Conservative Raster, Subgroups) ve MATOKI geçici çözümleri belgede ayrıntılarıyla açıklandı.
- ARCH_V2 için tavan/taban mimari kararları, ikili derleme stratejisi ve RHI Render Worker modeli tanımlandı.
