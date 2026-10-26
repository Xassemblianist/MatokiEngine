DURUM: TODO

# M0_2 — Platform katmanı ve frame döngüsü

## Amaç
Tarayıcıda wasm'ın sürdüğü bir frame döngüsü + input.

## Dosyalar
- `src/plat/platform.h` — `ARCHITECTURE.md §2` import'ları, `__attribute__((import_module("env"), import_name("...")))` ile
- `src/plat/input.h` — Input tamponu yerleşimi (C struct, JS ile birebir):
  ```c
  typedef struct {
      u8  keys[256];        // KeyboardEvent.keyCode → 0/1
      i32 mouse_dx, mouse_dy;   // frame başına birikmiş, C okuduktan sonra JS sıfırlar
      u32 mouse_buttons;    // bit0 sol, bit1 sağ, bit2 orta
      i32 wheel;
      u32 pointer_locked;
      u32 screen_w, screen_h;
  } InputState;
  ```
- `src/core/log.h/.c` — `LOG("x=%d", x)` için mini printf (`%d %u %x %f %s` yeterli), `plat_log`'a gider. `ASSERT(c)` → `plat_panic`.
- `src/main.c` — `game_frame` içinde input'u oku, test: WASD'ye göre arka plan rengi değişsin (şimdilik `CMD_PASS_BEGIN` + `CMD_PASS_END` ile clear rengi).
- `web/index.html` — tam ekran canvas, tıklayınca pointer lock.
- `web/platform.js` — wasm yükle, `memory` oluştur, import'ları bağla, `requestAnimationFrame` döngüsü, input olaylarını `InputState`'e yaz.
- `web/gpu.js` — WebGPU init, komut tamponunu çözen `executeCommands(ptr, len)`. Bu görevde sadece opcode 6 ve 10.

## Kabul kriterleri
- `make serve` → tarayıcıda W/A/S/D basınca ekran rengi değişir.
- Konsolda C'den gelen `LOG` mesajları görünür.
- Fare hareketi `mouse_dx/dy`'de doğru birikir (LOG ile doğrula).
- WebGPU yoksa ekranda anlaşılır hata mesajı.
