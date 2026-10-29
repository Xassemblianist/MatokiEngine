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
 gfx3d/   mesh, kamera, homojen kırpma (guard band), reverse-Z boru hattı (M8_A), paralel ertelenmiş ön uç (D-105)
 asset/   JSON (RFC 8259), base64, OBJ/MTL, glTF 2.0/GLB yükleyicileri — sıfırdan (M8_D)
 audio/   synth (osilatör, ADSR, sıralayıcı) + paylaşılan bellek ses halkası (M7_2)
 img/ ui/ PNG, prosedürel doku, font, immediate-mode UI (M7_0)
 phys/    3D çarpışma (küre/AABB/kapsül/OBB/üçgen), SAH BVH, ışın izleme, sabit adımlı dünya (M9_B)
 scene/   seyrek küme ECS, hiyerarşik Transform, yansıma tabanlı JSON sahne G/Ç (M10_B)
 sim/     deterministik sabit adımlı simülasyon çekirdeği, durum anlık görüntüsü, rollback ve replay (M16_A)
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
InputRing* engine_input_ring(void); // JS → C SPSC kare damgalı girdi olay halkası (src/plat/input_ring.h)
u32*  engine_framebuffer(void);   // RGBA8, w*h u32
u32   engine_fb_width(void);
u32   engine_fb_height(void);
// İsteğe bağlı (D-103): fare kilidi. JS tuvale tıklamada wants=1 ise requestPointerLock çağırır,
// kilit değişince set_pointer_lock(0/1) bildirir; kilitliyken mouse_dx/dy = movementX/Y (konum sabit).
u32   engine_wants_pointer_lock(void);
void  engine_set_pointer_lock(u32 locked);
// İsteğe bağlı WebGPU RHI ikinci arka uç (D-108, src/gpu/rhi.h, web/gpu.js):
b32           engine_gpu_available(void);        // WebGPU cihazı aktifse 1, yoksa 0 (CPU yoluna kalır)
void          engine_gpu_set_available(b32 on);  // web/gpu.js kurulumda 1 veya 0 bildirir
const u8*     engine_gpu_cmd_stream(void);       // aktif Rhi komut akışı tamponu (RhiStreamHeader*)
u32           engine_gpu_cmd_bytes(void);        // akıştaki toplam geçerli bayt (başlık dahil)
RhiErrorRing* engine_gpu_error_ring(void);       // JS → C hata geri bildirim halkası (paylaşımlı bellek)
b32           engine_gpu_push_error(u32 kind, u32 handle, u32 line, u32 col,
                                    const char* msg, u32 msg_len);
u32           engine_gpu_test_frame(f64 now_ms, f32 dt); // bağımsız WebGPU RHI demosu (gpu_test.html)

// Platform yetenek bildirimi (D-109, web/caps.js): açılışta tespit edilen yetenek bit maskesi
// (CAP_SAB, CAP_SIMD128, CAP_RELAXED_SIMD, CAP_THREADS, CAP_BULK_MEMORY, CAP_MEMORY64,
// CAP_WEBGPU, CAP_GPU_TIMESTAMP, CAP_GPU_SUBGROUPS, CAP_GPU_F16, CAP_OFFSCREEN, CAP_WAIT_ASYNC,
// CAP_WEBCODECS, CAP_WEBTRANSPORT, CAP_WORKER_RENDER, CAP_RELAXED_BUILD) ve WebGPU limitleri.
void  engine_set_caps(u32 caps_mask, u32 gpu_max_tex2d, u32 gpu_max_buf_mb);
```

### Yükleyici seçim kuralı ve Worker'da render (D-109, H10)
- `web/caps.js` açılışta tarayıcı ve Wasm yeteneklerini `WebAssembly.validate` probları ve API denetimleriyle tespit eder.
- **İkili seçimi**: SharedArrayBuffer / threads desteklenmiyorsa (veya `?st=1` / `?threads=0` verilmişse) tek iş parçacıklı yedek `engine_st.wasm` yüklenir (atomics ve shared-memory yok, iş kuyruğu seri çalışır). Çoklu iş parçacığı destekleniyorsa: `relaxedSimd` varsa `engine_relaxed.wasm` (`-mrelaxed-simd`), aksi halde `engine.wasm` yüklenir (`?relaxed=0|1` ile zorlanabilir).
- **Worker'da render**: ST modunda olmayan sistemlerde `OffscreenCanvas` (`transferControlToOffscreen`) ve `SharedArrayBuffer` mevcutsa motor döngüsü (`engine_init`, `engine_frame`, `putImageData`) ve alt raster worker havuzu bir render worker'da (`tid=0`) çalışır; ana iş parçacığı yalnızca girdi (`InputState` ve `InputRing`), ses (`AudioWorklet`), sürükle-bırak ve DOM olaylarını köprüler. Desteklenmezse (veya `?worker=0` verilirse) ana iş parçacığı `requestAnimationFrame` döngüsüne düşülür.

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

### WebGPU RHI İkili Komut Akışı ve Hata Halkası (D-108)
C tarafı (`src/gpu/rhi.h/.c`) GPU kaynaklarını nesil sayaçlı 32-bit tutamaçlarla (`RhiHandle = (type << 24) | (gen << 16) | index`, `0 = geçersiz`) yönetir ve her karede paylaşımlı bellekte düz, 4-bayt hizalı ikili bir komut akışı üretir. `web/gpu.js` kare sonunda `engine_gpu_cmd_stream()` + `engine_gpu_cmd_bytes()` görünümünü tek geçişte WebGPU çağrılarına çevirir; cihaz yoksa `engine_gpu_available() == 0` döner ve motor CPU yolunda kalır.

- **Akış Başlığı (`RhiStreamHeader`, 16 bayt)**:
  - `+0`: `u32 magic = 0x4D475055` (`"MGPU"`, little-endian)
  - `+4`: `u32 version = 1`
  - `+8`: `u32 byte_len` (başlık dahil toplam bayt, `>= 16`, 4'ün katı)
  - `+12`: `u32 cmd_count` (akıştaki tam komut sayısı)
- **Komut Başlığı (`RhiCmdHeader`, 8 bayt)**:
  Her komut `u32 opcode; u32 size;` ile başlar (`size` bu 8 baytlık başlık dahil komutun 4-bayt hizalı toplam bayt uzunluğudur). Değişken uzunluklu yükler (`CREATE_SHADER` WGSL metni, `WRITE_BUFFER` ve `WRITE_TEXTURE` baytları) komut yapısının hemen ardından gelir ve `ALIGN_UP(len, 4)` bayta sıfırla tamamlanır:
  1. `RHI_CMD_CREATE_BUFFER (1, 20 B)`: `handle, byte_size, usage`
  2. `RHI_CMD_CREATE_TEXTURE (2, 32 B)`: `handle, width, height, mip_levels, format, usage`
  3. `RHI_CMD_CREATE_SAMPLER (3, 32 B)`: `handle, address_u, address_v, mag_filter, min_filter, mipmap_filter`
  4. `RHI_CMD_CREATE_SHADER (4, 16 + align4(code_len) B)`: `handle, code_len, u8 code[]`
  5. `RHI_CMD_CREATE_PIPELINE (5, 96 + attr_count * 12 B)`: `handle, vertex_shader, fragment_shader, char vs_entry[16], char fs_entry[16], vertex_stride, attr_count, topology, cull_mode, front_face, color_format, blend_mode, depth_format, depth_test, depth_write, depth_compare, RhiVertexAttr attrs[attr_count]` (`shader_location, offset, format`)
  6. `RHI_CMD_CREATE_BIND_GROUP (6, 24 + entry_count * 20 B)`: `handle, pipeline, group_index, entry_count, RhiBindEntry entries[entry_count]` (`binding, kind, resource, offset, size`)
  7. `RHI_CMD_DESTROY_RESOURCE (7, 12 B)`: `handle`
  8. `RHI_CMD_WRITE_BUFFER (8, 20 + align4(data_len) B)`: `dst_buffer, dst_offset, data_len, u8 data[]`
  9. `RHI_CMD_WRITE_TEXTURE (9, 40 + align4(data_len) B)`: `dst_texture, mip_level, x, y, width, height, bytes_per_row, data_len, u8 data[]`
  10. `RHI_CMD_BEGIN_PASS (10, 40 B)`: `color_texture (0=tuval), depth_texture, flags (bit0=clear_color, bit1=clear_depth), f32 clear_rgba[4], f32 clear_depth`
  11. `RHI_CMD_SET_PIPELINE (11, 12 B)`: `pipeline`
  12. `RHI_CMD_SET_BIND_GROUP (12, 16 B)`: `group_index, bind_group`
  13. `RHI_CMD_SET_VERTEX_BUFFER (13, 24 B)`: `slot, buffer, offset, size`
  14. `RHI_CMD_SET_INDEX_BUFFER (14, 24 B)`: `buffer, index_format (0=uint16, 1=uint32), offset, size`
  15. `RHI_CMD_DRAW (15, 24 B)`: `vertex_count, instance_count, first_vertex, first_instance`
  16. `RHI_CMD_DRAW_INDEXED (16, 28 B)`: `index_count, instance_count, first_index, i32 base_vertex, first_instance`
  17. `RHI_CMD_SET_VIEWPORT (17, 32 B)`: `f32 x, y, w, h, min_depth, max_depth`
  18. `RHI_CMD_END_PASS (18, 8 B)`: yalnız başlık
  19. `RHI_CMD_PRESENT (19, 8 B)`: yalnız başlık
- **Hata Halkası (`RhiErrorRing`, `engine_gpu_error_ring()`)**:
  `write_pos (+0)`, `read_pos (+4)`, `capacity (+8, 16)`, `dropped (+12)` ve ardından 16 × 256 baytlık `RhiErrorEntry` (`kind, handle, line, col, msg_len, char msg[236]`). `web/gpu.js` WGSL derleme ve doğrulama hatalarını Atomics ile halkaya yazar; C tarafı `rhi_pop_error` / `rhi_poll_errors` ile tüketir.

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
