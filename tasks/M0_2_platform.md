DURUM: DONE
HAT: A (platform) — M0_1'den sonra

# M0_2 — Platform katmanı ve framebuffer

## Amaç
wasm'ın her frame doldurduğu framebuffer tarayıcıda görünür; input wasm'a ulaşır.

## Dosyalar
- `src/plat/input.h` — JS ile birebir yerleşim:
  ```c
  typedef struct {
      u8  keys[256];            // KeyboardEvent.keyCode → 0/1
      i32 mouse_x, mouse_y;     // framebuffer koordinatında
      i32 mouse_dx, mouse_dy;   // frame başına birikmiş; C okuyunca kendisi sıfırlar
      u32 mouse_buttons;        // bit0 sol, bit1 sağ, bit2 orta
      i32 wheel;
  } InputState;
  ```
- `src/raster/framebuffer.h/.c` — `Framebuffer { u32* px; u32 w, h; }`, `fb_init(arena, w, h)`, `fb_clear(fb, rgba)`, `fb_rect(fb, x, y, w, h, rgba)` (kırpmalı).
- `src/app/main.c` — export'lar: `engine_input_buf`, `engine_framebuffer`, `engine_fb_width`, `engine_fb_height`. 640×360 framebuffer; her frame zamanla kayan bir XOR/plazma deseni. Fare imlecinin altına 8×8 beyaz kare. Space'e basınca desen değişsin. FPS'i saniyede bir LOG'la.
- `web/index.html` — siyah arka plan, canvas ortalanmış, `image-rendering: pixelated`, en-boy oranı korunarak tam ekrana ölçeklenir.
- `web/platform.js` — `new WebAssembly.Memory({initial: 4096, maximum: 16384})` (engine.wasm'ın import limitleriyle AYNI olmalı) + `new WebAssembly.Table({element: "anyfunc", initial: 1})`; import'ları bağla (`plat_log`, `plat_panic`, `plat_time_ms`, `plat_jit_load`). C string'leri `TextDecoder` ile çöz. `engine_init`, rAF döngüsünde `engine_frame` → framebuffer'ı `ImageData` ile canvas'a bas. Input olaylarını `InputState`'e yaz (fare koordinatlarını canvas ölçeğine göre çevir). `plat_panic` → konsola hata + döngüyü durdur + ekrana kırmızı mesaj.
- `web/jit.js` — `plat_jit_load(ptr, len, nameptr, namelen)`: byte'ları kopyala → `new WebAssembly.Module` → `{env: {memory, __indirect_function_table: table}}` ile instantiate → `table.grow(1)`, export'u son indekse koy, indeksi döndür; hata → konsola yaz, -1. (M3'e kadar çağrılmayacak ama hazır olsun.)

## Kabul kriterleri
- `make serve` → tarayıcıda akıcı animasyon, fare karesi imleci takip ediyor, Space çalışıyor.
- Konsolda C'den gelen FPS logları.
- Geçici bir `ASSERT(0)` ile test et: konsolda dosya:satır'lı hata ve döngüyü durdurur. (Testten sonra kaldır.)
- Frame başına JS→wasm çağrısı sadece `engine_frame` (+ framebuffer görünümü değiştiyse yeniden alma).

## Notlar
- `src/plat/input.h`: JS ile birebir uyumlu 280 byte `InputState` yapısı tanımlandı (`_Static_assert` ile doğrulandı).
- `src/raster/framebuffer.h/.c`: 16-byte hizalı `fb_init`, `fb_clear` ve sınır kırpmalı `fb_rect` gerçeklendi.
- `src/app/main.c`: `engine_input_buf`, `engine_framebuffer`, `engine_fb_width`, `engine_fb_height` export'ları eklendi. 640×360 çözünürlükte 3 farklı XOR/plazma deseni, fare koordinatında 8×8 beyaz kare, Space tuşuyla desen geçişi ve saniyede bir FPS loglaması (`LOG`) gerçeklendi.
- `web/index.html`, `web/platform.js`, `web/jit.js`: `WebAssembly.Memory` (4096..16384 sayfa) ve `WebAssembly.Table` kuruldu; her karede yalnızca `engine_frame` çağrılacak şekilde `ImageData` görünümü önbelleklendi (`memory.buffer` değişirse yenilenir). Fare koordinatları en-boy oranı (`object-fit: contain`) hesaba katılarak 640×360 uzayına ölçeklendi.
- Tarayıcı testi: **Google Chrome 156.0.8078.47** ile `http://localhost:8080` üzerinde doğrulandı.
  - Konsol çıktı ve FPS: `FPS: 60.0 (61 kare / 1017 ms)` (~60-61 FPS sabit rAF hızı).
  - Space tuşu ile desen geçişi (`Desen modu: 1`) ve fare imleci takibi doğrulandı.
  - Geçici `ASSERT(0)` testi: Konsola `[MATOKI PANIC] src/app/main.c:66: ASSERT: 0` basıldı, ekranda kırmızı hata katmanı (`#panic-overlay`) açıldı ve rAF döngüsü durdu; test sonrasında `ASSERT(0)` kaldırıldı.
