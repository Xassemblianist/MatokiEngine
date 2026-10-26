# BLOKÇA — Mimari Anayasa

Tarayıcıda çalışan, sıfırdan yazılmış bir voxel sandbox oyunu (Minecraft klonu).
Motor, render, dünya üretimi, fizik, ses, UI, kayıt formatı — **hepsi bizim**.

## 0. Değişmez kurallar

1. **Oyun mantığının %100'ü C11 (freestanding) → wasm32.** libc yok, Emscripten yok, STL yok.
   Derleyici: `clang --target=wasm32 -nostdlib`, linker: `wasm-ld`.
2. **JS sadece "platform katmanı"dır.** Canvas, input, WebGPU çağrıları, ses çıkışı, depolama.
   JS'te oyun mantığı YASAK. JS dosyası toplamı ~1500 satırı geçmemeli.
3. **Mojang asset'i yok.** Dokular, sesler, font, model — hepsi kodla (prosedürel) üretilir.
   Hem yasal temizlik hem de "her şeyi biz yapıyoruz" ilkesi.
4. **Dinamik bellek yok (malloc yok).** Arena + pool allocator. Bkz. §3.
5. **Sadece offline / tek oyunculu.** Ağ kodu yok (şimdilik).

## 1. Katmanlar

```
┌─────────────────────────────────────────────────────────┐
│ web/  (JS platform katmanı)                             │
│  platform.js : wasm yükle, import tablosu, frame döngüsü│
│  gpu.js      : komut tamponunu çöz → WebGPU çağrıları   │
│  audio.js    : AudioWorklet, wasm'ın PCM tamponunu çalar│
│  storage.js  : IndexedDB (region dosyaları)             │
└──────────────▲──────────────────────────┬───────────────┘
               │ imports (env.*)          │ exports (game_*)
┌──────────────┴──────────────────────────▼───────────────┐
│ src/ (C11 → game.wasm)                                  │
│  core/   types, arena, math, rng, noise, string, sort   │
│  plat/   platform.h — JS import bildirimleri            │
│  gfx/    cmdbuf, kamera, mesh, doku atlası üreteci      │
│  world/  chunk, blok kaydı, terrain gen, ışık, mesher   │
│  game/   oyuncu, fizik, raycast, envanter, mob, crafting│
│  ui/     bitmap font, immediate-mode UI, HUD            │
│  audio/  synth (osilatör, gürültü, zarf), SFX üreteci   │
│  save/   region formatı, kendi LZ sıkıştırmamız         │
└─────────────────────────────────────────────────────────┘
```

## 2. wasm ↔ JS sınırı

Sınır geçişi pahalıdır. Kural: **frame başına mümkün olduğunca az çağrı.**

### Exports (C → JS)
```c
void  game_init(u32 seed);
void  game_frame(f64 now_ms, f32 dt);   // tek giriş noktası
u8*   game_input_buf(void);              // JS input durumunu buraya yazar
u8*   game_cmdbuf(void);                 // C render komutlarını buraya yazar
u32   game_cmdbuf_len(void);
f32*  game_audio_buf(void);              // AudioWorklet buradan PCM okur
```

### Imports (JS → C), `src/plat/platform.h`
```c
void  plat_log(const char* s, u32 len);
void  plat_panic(const char* s, u32 len);
f64   plat_time_ms(void);
void  plat_storage_write(u32 key, const void* p, u32 len);   // async, fire&forget
void  plat_storage_read_req(u32 key);                         // sonuç game_on_storage ile gelir
```

### Render komut tamponu
C, her frame lineer bellekte bir **komut akışı** yazar; JS frame sonunda bir kez çözer.
Her komut: `u32 opcode, u32 byte_len, payload...` (4-byte hizalı).

| opcode | isim | payload |
|---|---|---|
| 1 | `CMD_BUF_CREATE`   | id, size, usage |
| 2 | `CMD_BUF_UPLOAD`   | id, dst_off, src_ptr, len (src = wasm belleği) |
| 3 | `CMD_BUF_DESTROY`  | id |
| 4 | `CMD_TEX_CREATE`   | id, w, h, layers, format |
| 5 | `CMD_TEX_UPLOAD`   | id, layer, src_ptr, len |
| 6 | `CMD_PASS_BEGIN`   | clear rgba, clear depth |
| 7 | `CMD_PIPELINE`     | pipeline_id (JS tarafında önceden tanımlı WGSL) |
| 8 | `CMD_UNIFORMS`     | src_ptr, len |
| 9 | `CMD_DRAW`         | vbuf_id, first, count, chunk_origin(xyz i32) |
| 10| `CMD_PASS_END`     | — |

Veri kopyalanmaz: payload, wasm belleğine işaretçi taşır; JS `memory.buffer` üzerinden okur.

## 3. Bellek

- Tek `WebAssembly.Memory`, başlangıçta 256 MB, max 1 GB.
- `core/arena.h`: `Arena { u8* base; u32 cap, used; }`
  - `perm` arena: oyun ömrü boyunca (blok kaydı, atlas).
  - `frame` arena: her frame başında sıfırlanır.
  - `chunk_pool`: sabit boyutlu chunk slotları, freelist.
- Heap sınırı `__heap_base` sembolünden alınır.

## 4. Dünya

- **Chunk:** 32×32×32 blok (`Section`). Dikey dünya: y ∈ [0, 256) → 8 section/kolon.
- Blok: `u16` id. Işık: ayrı `u8` (4 bit gök, 4 bit blok).
- Koordinatlar: dünya `i32`, chunk içi `u8`. İndeks: `x + z*32 + y*1024`.
- Render mesafesi: varsayılan 8 kolon yarıçap.
- Mesher: önce yüz eleme (culled faces), sonra **greedy meshing**.
- Vertex formatı: **8 byte** paketli (`u32 pos(5+5+5 bit)+normal(3)+ao(2)+...`, `u32 tex+light`).
  Chunk orijini uniform/draw parametresi olarak gider.

## 5. Render

- Arka uç: **WebGPU**, elle yazılmış WGSL (`web/shaders/*.wgsl`).
- Doku: `texture_2d_array`, 16×16 katmanlar, **C'de prosedürel üretilir** ve upload edilir.
- Geçişler: opak → alpha-test (yaprak) → saydam (su, arkadan öne sıralı) → UI.
- (Gelecek, eğlence modu) Yazılım rasterizer arka ucu — "Hayalet GPU".

## 6. Thread'ler (Faz 2)

- `SharedArrayBuffer` + Web Worker. COOP/COEP başlıkları gerekli → `tools/serve.py`.
- Ana thread: oyun + render. Worker'lar: terrain gen + meshing.
- İş kuyruğu: lock-free SPSC halkalar, `memory.atomic.wait/notify`.

## 7. Build

```
make          # build/game.wasm
make serve    # http://localhost:8080 (COOP/COEP ile)
```
Bayraklar: `-O2 -msimd128 -mbulk-memory -mnontrapping-fptoint -fno-builtin -ffreestanding`
`memcpy/memset` için `core/mem.c` kendi implementasyonu (bulk-memory intrinsics).
