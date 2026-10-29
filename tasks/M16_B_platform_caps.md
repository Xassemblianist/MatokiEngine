# M16_B — Platform yetenekleri, relaxed SIMD derlemesi, worker'da render (D-109)

Durum: TODO

## Amaç
D-109'u uygula:
1. Yetenek tespiti (`web/caps.js`): SAB/crossOriginIsolated, wasm SIMD, relaxed SIMD, threads, bulk memory,
   memory64 (yalnız rapor), WebGPU (+ adapter features: timestamp-query, subgroups, shader-f16, limitler),
   OffscreenCanvas, Atomics.waitAsync, WebCodecs, WebTransport. Sonuç wasm'a `engine_set_caps(bitmask, ...)` ile
   verilir; HUD/debug panelinde gösterilir.
2. İkinci derleme `build/engine_relaxed.wasm` (`-mrelaxed-simd`): relaxed SIMD'den kazanç beklenen sıcak döngüler
   için `core` altında `simd_madd` gibi makrolar (relaxed varsa `f32x4.relaxed_madd`, yoksa mul+add). Yükleyici
   yeteneğe göre seçer. Kazancı ölç (ör. vertex dönüşümü, skinning, gölge kodlama).
3. Render'ı ana iş parçacığından al: OffscreenCanvas + transferControlToOffscreen ile motor döngüsü bir worker'da;
   ana iş parçacığı yalnız girdi/ses/DOM köprüsü. Desteklenmezse eski yol.

## Dosyalar
- `web/caps.js` (yeni), `web/platform.js`, `web/index.html`, `web/worker.js`, `web/threads.js`
- `Makefile`, `src/core/simd.h` (yeni ya da mevcut SIMD başlığı), `src/gfx3d/vertex_wasm.c`, `src/anim/skin_wasm.c`
- `src/app/main.c` (yalnız `engine_set_caps` dışa aktarımı ve HUD satırı)
- `docs/ARCHITECTURE.md` §2 (yükleyici seçim kuralı, yeni dışa aktarımlar)

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız; iki wasm de `wasm-validate` geçer
- [ ] relaxed derleme ile normal derleme sonuçları testlerde tolerans içinde aynı; kazanç ölçülüp Notlar'a yazılır
- [ ] Worker'da render açıkken mevcut tüm sahneler çalışır (elle denetlenebilen kadarı Notlar'a)
