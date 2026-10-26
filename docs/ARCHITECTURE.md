# MATOKI — Mimari Anayasa

Tarayıcıda, **GPU kullanmadan**, wasm SIMD + thread'lerle çalışan yazılım render motoru.
Shader'lar kendi dilimiz **Matoki** (`.mtk`) ile yazılır ve **çalışma anında wasm bytecode'una JIT derlenir**.
Bu motorun üstüne 2D, 3D, voxel — her türlü oyun çatallanır (`games/`).

## 0. Değişmez kurallar

1. Motor: **C11 freestanding → wasm32** (`clang --target=wasm32 -nostdlib`, `wasm-ld`). libc yok, malloc yok.
2. JS sadece **platform katmanı**: wasm yükleme, input, framebuffer'ı canvas'a basma, JIT modülü instantiate etme, worker'lar, depolama. Oyun/motor mantığı JS'e giremez.
3. Render tamamen CPU'da. WebGPU/WebGL **yok** (ileride opsiyonel 2. arka uç, karar `DECISIONS.md`'den geçer).
4. Her optimizasyon bir **benchmark** ile kanıtlanır. "Daha hızlı olmalı" yetmez; ölçüm tablosu görev notlarına yazılır.
5. Elle yazılmış WAT sadece clang çıktısını ölçülebilir şekilde (>%15) geçtiği yerde kalır.

## 1. Katmanlar

```
web/                      (JS, ~1000 satır sınırı)
 platform.js   wasm yükle, rAF döngüsü, input, framebuffer → canvas
 jit.js        plat_jit_load: byte'ları Module'e çevir, paylaşılan tabloya koy
 worker.js     (Faz 2) raster worker'ları
 filedrop.js   sürükle-bırak: dosya baytlarını wasm belleğine kopyalar (D-097)
 audio.js      AudioContext + AudioWorklet kurulumu (ilk etkileşimde)
 audio_worklet.js  paylaşılan bellekteki ses halkasını çıkışa kopyalar (D-090)

src/  (C11 → engine.wasm)
 core/    types, mem, arena, math, rng, log, profiler
 plat/    platform.h (JS import'ları), input.h
 raster/  framebuffer, üçgen kurulumu, karo binning, rasterizer, blend, doku örnekleme
 mbin/    wasm binary üretici (LEB128, section'lar, opcode yayıcı) = bizim assembler'ımız
 matoki/  lexer, parser, sema, IR, opt, codegen(→mbin), jit köprüsü
 gfx2d/   sprite batch, tilemap + çarpışma, Camera2D, animasyon (M7_1)
 gfx3d/   mesh, kamera, homojen kırpma (guard band), reverse-Z boru hattı (M8_A)
 asset/   JSON (RFC 8259), base64, OBJ/MTL, glTF 2.0/GLB yükleyicileri — sıfırdan (M8_D)
 audio/   synth (osilatör, ADSR, sıralayıcı) + paylaşılan bellek ses halkası (M7_2)
 img/ ui/ PNG, prosedürel doku, font, immediate-mode UI (M7_0)
 app/     demo uygulamaları, main.c

tools/host/   native (Linux) test araçları — SADECE burada libc serbest
 matc.c       .mtk → .wasm komut satırı derleyicisi (src/matoki'yi native derler)
 run_tests.sh wasm-validate + wasm-interp (wabt) ile doğrulama

games/        motorun üstündeki oyunlar (blokca/ = voxel, ileride)
```

## 2. wasm ↔ JS sözleşmesi

### Exports (engine.wasm)
```c
void  engine_init(u32 seed);
void  engine_frame(f64 now_ms, f32 dt);
u8*   engine_input_buf(void);     // InputState, JS yazar
u32*  engine_framebuffer(void);   // RGBA8, w*h u32
u32   engine_fb_width(void);
u32   engine_fb_height(void);
```

### Imports (`src/plat/platform.h`, modül `env`)
```c
void plat_log(const char* s, u32 len);
void plat_panic(const char* s, u32 len);
f64  plat_time_ms(void);
i32  plat_jit_load(const u8* wasm, u32 len, const char* export_name, u32 name_len);
     // dönen değer: paylaşılan fonksiyon tablosundaki indeks, hata ise -1
```

### Bellek ve tablo
- `env.memory`: JS oluşturur, başlangıç 256 MB, max 1 GB. (Faz 2'de `shared: true`.)
- `env.__indirect_function_table`: JS oluşturur ve hem engine.wasm'a hem her JIT modülüne import eder.
  engine link bayrağı: `--import-memory --import-table --growable-table`.
- C tarafı JIT fonksiyonunu fonksiyon işaretçisi olarak çağırır: `((ShadeFn)(usize)idx)(batch, count)` → `call_indirect`.
- JIT modülleri belleği ve tabloyu import eder, kendi belleği yoktur.

### Framebuffer sunumu
JS her frame `engine_framebuffer()`'dan `Uint8ClampedArray` görünümü alır → `ImageData` → `putImageData`.
(SAB'e geçince ImageData SAB kabul etmediği için tek kopya gerekir; bu bilinçli bir maliyet.)

## 3. Raster çekirdeği

- Framebuffer: renk `u32 RGBA8`, derinlik `f32` (3D'de). Çözünürlük sabit iç çözünürlük (varsayılan 640×360), canvas CSS ile ölçeklenir.
- Üçgen kurulumu: **sabit nokta** kenar fonksiyonları, 8 bit alt-piksel hassasiyeti, top-left fill kuralı.
- Karo: 64×64. Üçgenler önce karolara bin'lenir, sonra karo karo raster edilir (Faz 2'de karo = worker işi).
- Gölgelendirme birimi: **2×2 quad** (4 piksel = 4 SIMD lane). Türevler (mip seçimi için) quad içinden.
- 2D sprite = 2 üçgen. Ayrı bir blit yolu YOK; tek yol, tek optimizasyon hedefi.

## 4. Shader ABI (C ↔ JIT'lenmiş Matoki)

```c
// src/raster/shade_abi.h — yerleşim değiştirmek mimari karardır
typedef struct {
    u32 mask;          // bit0..3: quad'daki hangi pikseller kapsanıyor
    u16 x, y;          // quad sol üst pikseli
} QuadHeader;

typedef struct {
    u32  count;                // batch'teki quad sayısı (<= 256)
    u32  varying_count;
    QuadHeader* quads;         // [count]
    f32* varyings;             // SoA: varyings[(v * count + q) * 4 + lane]
    const u8* uniforms;        // shader'ın uniform bloğu (std hizalama: 16 byte)
    f32* out_rgba;             // SoA: out[(c * count + q) * 4 + lane], c ∈ {r,g,b,a}
    const struct Texture* textures; // bağlı dokular
} ShadeBatch;

typedef void (*ShadeFn)(ShadeBatch* batch);   // wasm tipi: (i32) -> ()
```
- v0'da blend + framebuffer yazımı C'de. v1'de (M5) blend modu da JIT'e füzyonlanır.
- C'de her shader'ın **referans implementasyonu** olur; JIT çıktısı ona karşı test edilir (tol 1/255).

## 5. Matoki

Dil şartnamesi: `docs/MATOKI.md`. Derleyici `src/matoki/`, C ile yazılır, motorun içinde çalışır
(oyun açılırken / shader değişince derler). Aynı kaynak `tools/host/matc` olarak native de derlenir.

## 6. Thread'ler (Faz 2)

- `SharedArrayBuffer` + N worker, her worker aynı engine.wasm'ı aynı belleğe instantiate eder.
- COOP/COEP başlıkları: `tools/serve.py`.
- **JIT modülleri her worker'da ayrıca instantiate edilir** ve tablo indeksleri tüm worker'larda aynı olmalı:
  ana thread indeksi ayırır, worker'lara `(bytes, idx)` yayınlar, worker `table.set(idx, fn)` yapar.
- İş dağıtımı: karo kuyruğu, `memory.atomic.wait/notify`.

## 7. Build

```
make            # build/engine.wasm → web/
make host       # build/host/matc (native)
make test       # host testleri + wasm-validate
make serve      # http://localhost:8080
```
wasm bayrakları: `-std=c11 -O2 -ffreestanding -fno-builtin -nostdlib -msimd128 -mbulk-memory -mnontrapping-fptoint -Wall -Wextra -Werror`
