# F0_E — Platform gerçeklik taraması (H6) — yalnız araştırma

Durum: TODO

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
- [ ] Her hücrenin kaynağı var
- [ ] WebGPU'da olmayan ama Unreal-sınıfı render için gereken şeyler ve geçici çözümleri listelenmiş
