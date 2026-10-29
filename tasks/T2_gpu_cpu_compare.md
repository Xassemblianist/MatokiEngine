# T2 — CPU ↔ GPU karşılaştırma testi

Durum: TODO

## Amaç
Aynı Matoki çekirdeğinin (pixel + compute, shaders/ ve shaders/compute/ içindekilerin hepsi) CPU JIT çıktısı ile
WebGPU çıktısını headless Chrome'da (Vulkan + SwiftShader) karşılaştır; piksel başına tolerans (ör. 2/255) ve hata haritası
build/ altına PNG. `make gpucompare` hedefi; make gpucheck'e bağlanır.

## Dosyalar
`tools/host/webgpu_check.mjs`, `tools/host/gpu_compare.mjs` (yeni), `web/gpu_test.html`, `web/gpu.js`, `src/gpu/gpu_test.c`, `Makefile`

## Kabul kriterleri
- [ ] Tüm çekirdekler için fark tablosu Notlar'da; aşan varsa hatası düzeltildi ya da gerekçeli
- [ ] make check + make gpucompare temiz
