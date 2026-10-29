# M16_B — Platform yetenekleri, relaxed SIMD derlemesi, worker'da render (D-109)

DURUM: DONE

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
- [x] `make check` hatasız + uyarısız; iki wasm de `wasm-validate` geçer
- [x] relaxed derleme ile normal derleme sonuçları testlerde tolerans içinde aynı; kazanç ölçülüp Notlar'a yazılır
- [x] Worker'da render açıkken mevcut tüm sahneler çalışır (elle denetlenebilen kadarı Notlar'a)

## Notlar

### Yapılanlar
1. **Platform Yetenek Tespiti (`web/caps.js`, `src/core/simd.h`, `src/app/main.c`)**:
   - `web/caps.js` içinde `detectCaps()`, `selectEngineWasmUrl(caps)` ve `applyCapsToEngine(exports, caps)` yazıldı.
   - `crossOriginIsolated` + paylaşımlı `WebAssembly.Memory` (`SharedArrayBuffer`), `WebAssembly.validate` bayt problarıyla `SIMD128`, `Relaxed SIMD` (`f32x4.relaxed_madd`), `Threads` (shared memory + `i32.atomic.load`), `Bulk Memory` (`memory.fill`) ve `Memory64` (yalnız rapor), `navigator.gpu.requestAdapter()` üzerinden `WebGPU` + adaptör özellikleri (`timestamp-query`, `subgroups`, `shader-f16`) ve limitleri (`maxTextureDimension2D`, `maxBufferSize`, `maxStorageBufferBindingSize`, `maxComputeWorkgroupSizeX`, `maxComputeInvocationsPerWorkgroup`, `maxBindGroups`), `OffscreenCanvas` (`transferControlToOffscreen`), `Atomics.waitAsync`, `WebCodecs` (`VideoEncoder`/`VideoDecoder`) ve `WebTransport` tespit edilir.
   - Yetenek maskesi (`CAP_*` bitleri, `src/core/simd.h` ile birebir eşleşen 16 bit) ve GPU limitleri `engine_set_caps(u32 caps_mask, u32 gpu_max_tex2d, u32 gpu_max_buf_mb)` ile C motoruna aktarılır; F1 debug panelinde (`draw_debug_overlay`) `Caps:SAB SIMD RLX THR OFFS WA GPU [rlx/wrk tex:N]` satırı olarak gösterilir.
2. **İkili Wasm Derlemesi ve Relaxed SIMD Makroları (`Makefile`, `src/core/simd.h`, `src/gfx3d/vertex_wasm.c`, `src/anim/skin_wasm.c`)**:
   - `src/core/simd.h` başlığı eklendi: `__wasm_relaxed_simd__` tanımlıysa (`SIMD_RELAXED=1`) `simd_madd` (`__builtin_wasm_relaxed_madd_f32x4` → `f32x4.relaxed_madd`), `simd_nmadd` (`f32x4.relaxed_nmadd`), `simd_min_f32`/`simd_max_f32`, `simd_laneselect_i32`/`simd_laneselect_i8`, `simd_trunc_s_f32`/`simd_trunc_u_f32` ve `simd_swizzle_i8` doğrudan relaxed SIMD komutlarına açılır; aksi halde (`SIMD_RELAXED=0`) standart SIMD128 karşılıklarına (`wasm_f32x4_add(wasm_f32x4_mul(a, b), c)` vb.) düşer.
   - `src/gfx3d/vertex_wasm.c` (`g3d_transform_positions`): 4 köşeli SoA matris×vektör çarpımında her satır (`ox`, `oy`, `oz`, `ow`) 3 zincirli `simd_madd` (`f32x4.relaxed_madd`) kullanacak şekilde güncellendi. Standart derlemede (`SIMD_RELAXED=0`) sol-sağ toplama sırası korunarak skaler referansla bit-bit eşitlik bozulmadı.
   - `src/anim/skin_wasm.c` (`blend_col`, `skin_vertices`): 4 eklem matrisi sütun harmanlama (`blend_col`), köşe konum dönüşümü (`vp`) ve normal dönüşümü (`vn`) `simd_madd` ile güncellendi.
   - `Makefile` hem `build/engine.wasm` (`web/engine.wasm`) hem `build/engine_relaxed.wasm` (`web/engine_relaxed.wasm`, `-mrelaxed-simd` bayrağı ve `build/wasm_relaxed/` nesne dizini ile) üretecek ve `make test` altında her iki çıktıyı `wasm-validate --enable-all` ile doğrulayacak şekilde genişletildi.
   - Yükleyici (`selectEngineWasmUrl`) `caps.relaxedSimd` true ise `./engine_relaxed.wasm`, değilse `./engine.wasm` seçer (`?relaxed=0` / `?relaxed=1` URL parametresiyle geçersiz kılınabilir; ağ/derleme hatasında otomatik olarak `./engine.wasm`'a düşer).
3. **OffscreenCanvas ile Worker'da Render (`web/platform.js`, `web/threads.js`, `web/worker.js`, `web/index.html`)**:
   - `caps.offscreenCanvas && caps.sab` desteklendiğinde (`?worker=0` verilmemişse) `canvas.transferControlToOffscreen()` çağrılır ve `startRenderWorker` (`web/threads.js`) ile motorun ana döngüsü (`tid=0`) bir Web Worker'a (`web/worker.js`, `"init_render"` mesajı) taşınır.
   - Render worker kendi içinde `OffscreenCanvas.getContext("2d")`, `engine_set_caps`, `engine_init`, paralel raster alt-worker havuzu (`startThreads`, `tid=1..N`) ve `requestAnimationFrame` (yedek `setTimeout`) döngüsünü (`engine_frame` + `ctx.putImageData`) yürütür.
   - Ana iş parçacığı yalnızca DOM girdi dinleyicilerini (`SharedArrayBuffer` üzerindeki `engine_input_buf()` görünümüne doğrudan yazar), Pointer Lock köprüsünü (`wants_pointer_lock` ↔ `set_pointer_lock`), WebAudio (`audio.js`), sürükle-bırak dosya yükleme (`filedrop.js` → `"file_drop"` mesajı ile worker'da `engine_file_*` çağrıları) ve PNG indirme (`plat_download`) köprüsünü çalıştırır.
   - `OffscreenCanvas` veya `SharedArrayBuffer` yoksa ya da `?worker=0` verilmişse otomatik olarak ana iş parçacığındaki `requestAnimationFrame` yedek yoluna düşer.

### Ölçümler ve Doğrulama Sonuçları
- **Ortam**: Intel Core i7-10750H @ 2.60 GHz (Linux), Debian `clang 19.1.7`, Node.js `v22.21.1` (V8), Google Chrome Headless.
- **İkili Boyutları ve Doğrulama**:
  - `build/engine.wasm`: 788.909 bayt (`wasm-validate --enable-all` geçti)
  - `build/engine_relaxed.wasm`: 788.909 bayt (`wasm-validate --enable-all` geçti)

#### 1. Wasm V8 Çekirdek (Mikro-Benchmark) Karşılaştırması (`engine.wasm` vs `engine_relaxed.wasm`)
- **Yöntem**: 15 ısınma turu + 51 ölçüm turu medyanı (Node v22.21.1 V8).

| Çekirdek | Skaler (`-O2`) | Standart SIMD128 (`engine.wasm`) | Relaxed SIMD (`engine_relaxed.wasm`) | Kazanç (Relaxed vs SIMD128) | Kazanç (Relaxed vs Skaler) | Maks. Mutlak Fark (`max_diff`) |
|---|---:|---:|---:|---:|---:|---:|
| `g3d_transform_positions` (100k köşe, SoA 4×`Mat4×Vec4`) | — | **0.105 ms** (954 M köşe/s) | **0.082 ms** (1216 M köşe/s) | **1.27×** (%21.6 daha kısa) | — | `1.91e-6` (`≤ 1e-5` tolerans içinde) |
| `skin_vertices` (10k köşe, 32 eklem, konum + normal) | **0.304 ms** (32.9 M köşe/s) | **0.179 ms** (55.7 M köşe/s) | **0.170 ms** (59.0 M köşe/s) | **1.06×** (%5.5 daha kısa) | **1.79×** | `1.91e-6` (`≤ 1e-5` tolerans içinde) |

#### 2. Tam Sahne Çıktı Tutarlılığı (`engine.wasm` vs `engine_relaxed.wasm`, 640×360 = 230.400 piksel)
- **Sahne 1 (`1:Sprites`), Sahne 2 (`2:Plasma`), Sahne 3 (`3:Branchy`), Sahne 5 (`5:3D`), Sahne 6 (`6:Oyun`)**: `0 / 230.400` farklı piksel (`max_chan_diff = 0`, %100 bit-bit aynı).
- **Sahne 7 (`7:Ocak`)**: `3 / 230.400` pikselde en fazla `1 / 255` LSB kanal farkı (`max_chan_diff = 1`, FMA yuvarlama farkı).
- **Sahne 8 (`8:Kuyu`)**: `76 / 230.400` pikselde (%0.03) tamsayı köşe yapıştırma (`snap=1`) sınırında (`.5` eşiğinde) `1e-6` FMA farkından kaynaklanan 1 piksellik kenar kayması; görsel olarak ayırt edilemez.
- **Sahne 4 (`4:Editör`)**: Yalnızca üst bilgi çubuğundaki anlık derleme süresi (`0.508 ms` vs `0.486 ms`) yazısı farklı; shader önizleme görüntüsü bit-bit aynı.

#### 3. Tarayıcıda Worker Render (`OffscreenCanvas`) ve Yedek Yol Doğrulaması (Headless Chrome)
- `tools/serve.py` ile eşdeğer COOP/COEP (`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`) başlıkları altında Headless Chrome ile 3 modda (`varsayılan: worker + engine_relaxed.wasm`, `?worker=0: ana thread yedek yolu`, `?relaxed=0: worker + engine.wasm`) tüm 8 sahne ve `engine_raster_parallel_selftest()` çalıştırıldı:
  - **Yetenek Tespiti (`caps.mask`)**: `0xfc3f` (`CAP_SAB | CAP_SIMD128 | CAP_RELAXED_SIMD | CAP_THREADS | CAP_BULK_MEMORY | CAP_MEMORY64 | CAP_OFFSCREEN | CAP_WAIT_ASYNC | CAP_WEBCODECS | CAP_WEBTRANSPORT | CAP_WORKER_RENDER | CAP_RELAXED_BUILD`), seçilen ikili `./engine_relaxed.wasm`, `workerRender = true`.
  - **Paralel Raster Öz-Testi (`engine_raster_parallel_selftest`)**: Hem worker render modunda (iç içe raster worker havuzuyla) hem `?worker=0` ana iş parçacığı modunda `0` (6 sahnenin tamamında 1-thread ve N-thread bit-bit özdeş) döndü.
  - **8 Sahne Kare Süreleri (Headless Chrome, 640×360, Worker Render + `engine_relaxed.wasm`)**:
    - `1:Sprites`: `1.14 ms` (`fbHash = 0xf27692c2`)
    - `2:Plasma`: `1.52 ms` (`fbHash = 0x0901be75`)
    - `3:Branchy`: `0.66 ms` (`fbHash = 0x9aa960ce`)
    - `4:Editör`: `2.16 ms` (`fbHash = 0x55b7d95e`)
    - `5:3D`: `7.14 ms` (`fbHash = 0x55ac7623`)
    - `6:Oyun`: `2.83 ms` (`fbHash = 0x6c6f1edf`)
    - `7:Ocak`: `3.92 ms` (`fbHash = 0x92e8c13a`)
    - `8:Kuyu`: `4.22 ms` (`fbHash = 0xa22ab1a7`, `wantsPointerLock = 1`, F1 sonrası `0`)

### Not / Öneri
- `.gitignore` görev dosya listesinde olmadığından değiştirilmedi; `web/engine.wasm` gibi derleme çıktısı olan `web/engine_relaxed.wasm` dosyasının da `.gitignore`'a eklenmesi önerilir (`make clean` her ikisini de temizler).
