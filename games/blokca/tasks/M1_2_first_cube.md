DURUM: TODO

# M1_2 — Komut tamponu ve ilk küp

## Dosyalar
- `src/gfx/cmdbuf.h/.c` — `ARCHITECTURE.md §2` opcode tablosunun tamamı. `cmd_buf_create(...)`, `cmd_draw(...)` vb. yazıcı fonksiyonlar. Frame arena'da yaşar.
- `src/gfx/camera.h/.c` — FPS kamera (yaw/pitch), uçuş modu (WASD + Space/Shift), fare hassasiyeti.
- `web/gpu.js` — tüm opcode'lar. GPU kaynakları `Map<id, GPUBuffer|GPUTexture>`. Derinlik tamponu, resize.
- `web/shaders/chunk.wgsl` — Vertex: 8-byte paketli format (`ARCHITECTURE.md §4`) açılır; Fragment: `texture_2d_array` örnekleme, basit yön ışığı.
- `src/main.c` — Tek küp için 24 vertex üret, 2 katmanlı test dokusu (C'de dama deseni) yükle, çiz.

## Kabul kriterleri
- Fare ile bakılan, WASD ile uçulan sahnede doku kaplı bir küp.
- Pencere boyutu değişince görüntü bozulmaz.
- Frame başına JS↔wasm çağrısı: `game_frame` + `game_cmdbuf*` dışında yok.
