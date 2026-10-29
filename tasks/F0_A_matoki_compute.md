# F0_A — Matoki `compute` çekirdekleri: wasm + WGSL (H2)

Durum: DONE

## Amaç
ARCH_V2 §3: Matoki'ye `compute` çekirdek türü: N eleman üzerinde SPMD, `buffer<T>` okuma/yazma (f32/u32/vec4),
`global_id`, u32 atomik toplama. CPU hedefi: wasm SIMD (4 lane) + `job_parallel_for` ile parçalama. GPU hedefi: M15_A'nın
WGSL üreticisiyle `@compute @workgroup_size(64)`. Üç örnek çekirdek: parçacık güncelleme, prefix-sum (iki geçiş),
skinning (köşe × eklem matrisi).

## Dosyalar
- `src/matoki/*`, `tests/test_matoki_compute.c` (yeni), `shaders/compute/*.mtk` (yeni), `docs/MATOKI.md`

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] Her çekirdek için C referans fonksiyonu; wasm çıktısı referansla tolerans içinde aynı (test)
- [x] WGSL çıktıları `build/wgsl/compute_*.wgsl`; GPU doğrulaması yöneticide
- [x] CPU ölçümü: 1T ve 8T, C referansına göre hız (Notlar'a; makine yükünü not et)

## Notlar

### 1. Yapılanlar
- **Ön Uç (`lexer.c`, `ast.h`, `parser.c`, `sema.c`, `matoki.h`, `matoki.c`)**:
  - `buffer isim: buffer<f32>;`, `buffer<u32>`, `buffer<vec4>` üst seviye bildirimleri (`AST_DECL_BUFFER`, `MTK_SYM_BUFFER`, `MtkLayout.buffer_count`, `MtkLayout.is_compute`) ve `buf[idx]` / `buf[idx].swizzle` indeksleme ifadeleri (`AST_EXPR_INDEX`) eklendi.
  - Tamsayı literallerinde `u`/`U` soneki (`0u`, `255u`) desteği eklendi.
  - `fn compute()` giriş noktası doğrulaması, compute çekirdeklerine özgü `global_id: u32` yerleşiği ve `atomic_add(buf: buffer<u32>, idx: u32, val: u32) -> u32` yerleşik fonksiyonu eklendi (`global_id`'nin `frag` içinde, `discard`'ın `compute` içinde kullanımı derleme hatası olarak yakalanır).
- **IR, Optimizasyon ve Lowering (`ir.h`, `ir.c`, `opt.c`, `lower.c`)**:
  - `IR_GLOBAL_ID`, `IR_BUF_LOAD`, `IR_BUF_STORE` ve `IR_ATOMIC_ADD` komutları eklendi (`IR_BUF_IMM(slot, comp, is_vec4)` kodlaması ve `IrFunc` içinde `buffer_count`, `buffer_atomic_mask`, `buffer_elem_types`, `is_compute` alanları).
  - `fn compute()` içinde koşullu erken `return;` deyimleri aktif lane maskesi (`ret_mask_id`) ile maskelenerek sonraki `IR_BUF_STORE` ve `IR_ATOMIC_ADD` yan etkilerine koşullu maske (`in->c`) olarak bağlandı.
  - CSE ve DCE geçişleri yan etkili compute komutlarını (`IR_BUF_STORE`, `IR_ATOMIC_ADD`) ve `IR_BUF_LOAD` bellek bağımlılıklarını koruyacak şekilde güncellendi.
- **CPU Arka Ucu (`codegen.h`, `codegen.c`, `ir_eval.c`, `matoki.c`)**:
  - `MtkComputeBatch` ABI'si (`start`, `count`, `total_count`, `buffer_count`, `uniforms`, `buffers[16]`), `ir_eval_compute` referans yorumlayıcısı, `mtk_codegen_compute` Wasm SIMD üreticisi ve `mtk_compute_dispatch` (`job_parallel_for` üzerinden 4-lane grup dağıtımı) eklendi.
  - `mtk_codegen_compute` içinde 4'lü tam SIMD grupları için hızlı döngü (`is_fast_body`) + kalan elemanlar için maskeli kuyruk bloğu üretilir:
    - Ardışık skaler erişimler (`buf[global_id + c]`): tek `v128.load` / `v128.store`.
    - Blok tabanı erişimleri (`buf[global_id >> 2]`): 4 lane aynı kelimeyi okuduğundan tek `v128.load32_splat`.
    - `buffer<vec4>` (`buf[global_id]`) ve 4-adımlı skaler blok erişimleri (`buf[global_id * 4 + 0..3]`): 4× `v128.load` / `v128.store` + `i8x16.shuffle` ile yerinde 4×4 AoS↔SoA transpoze.
    - `IR_ATOMIC_ADD`: aktif lane'ler üzerinde wasm `i32.atomic.rmw.add` (`0xFE 0x1E`).
- **WGSL Arka Ucu (`wgsl.h`, `wgsl.c`)**:
  - `mtk_wgsl_compute` ile `@group(0) @binding(0) var<uniform> mtk_u: MtkUniforms;` ve `@group(0) @binding(1 + s) var<storage, read_write> buf_<s>: array<T>;` (`f32`, `u32`, `vec4<f32>` veya `atomic_add` kullanılan tamponlarda `atomic<u32>`) bağlamaları ve `@compute @workgroup_size(64) fn compute(@builtin(global_invocation_id) gid: vec3<u32>)` giriş noktası üretildi.
- **Örnek Çekirdekler ve Testler (`shaders/compute/*.mtk`, `tests/test_matoki_compute.c`)**:
  - `shaders/compute/particle.mtk`, `shaders/compute/prefix_sum_scan.mtk`, `shaders/compute/prefix_sum_add.mtk`, `shaders/compute/skinning.mtk`.
  - `tests/test_matoki_compute.c`: Sema hata testleri, erken `return;` + `atomic_add` + swizzled `vec4` yazım testi ve 3 örnek çekirdeğin C referansı ↔ `ir_eval_compute` ↔ Wasm SIMD çıktısı doğrulaması.

### 2. Gerçek GPU (WebGPU / Vulkan) Doğrulaması
- Üretilen `build/wgsl/compute_particle.wgsl`, `build/wgsl/compute_prefix_sum_scan.wgsl`, `build/wgsl/compute_prefix_sum_add.wgsl`, `build/wgsl/compute_skinning.wgsl` ve `build/wgsl/compute_atomic.wgsl` dosyaları `node tools/host/webgpu_check.mjs http://127.0.0.1:8097/ ... --enable-features=Vulkan --use-angle=vulkan` ile gerçek GPU üzerinde (**NVIDIA Turing / Vulkan**) derlendi, `createComputePipelineAsync` ile boru hatları oluşturuldu ve compute dispatch çıktıları GPU'dan geri okunarak referansla birebir (`exact_match`) doğrulandı.

### 3. CPU Benchmark Sonuçları (1T ve 8T vs C Referansı)
- **Donanım & Makine Yükü**: Intel Core i7-10750H @ 2.60 GHz (6 fiziksel çekirdek / 12 thread), ölçüm anında sistem yükü (`loadavg`): `1.19, 1.52, 1.70`.
- **Ortam**: Wasm32 (`clang --target=wasm32 -O2`), V8 / Node.js; 31 tekrar medyanı (7 ısınma turu sonrası). 1T = tek thread (`engine_set_worker_count(0)`), 8T = ana thread + 7 `worker_threads` (`engine_set_worker_count(7)`, `mtk_compute_dispatch` + `job_parallel_for`).

| Çekirdek | Eleman Sayısı (`N`) | C Referans 1T (ms) | Matoki Compute 1T SIMD (ms) | 1T Hızlanma (vs C Ref) | Matoki Compute 8T SIMD (ms) | 8T Hızlanma (vs C Ref) | 8T / 1T Ölçeklenme |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Parçacık Güncelleme** (`particle.mtk`) | 65,536 parçacık | 0.5290 | 0.1992 | **2.66×** | 0.0726 | **7.28×** | 2.74× |
| **Prefix-Sum (İki Geçiş)** (`prefix_sum_scan` + `add`) | 262,144 `u32` | 0.2835 | 0.1259 | **2.25×** | 0.0725 | **3.91×** | 1.74× |
| **Skinning (32 Eklem)** (`skinning.mtk`) | 65,536 köşe | 1.8467 | 0.8800 | **2.10×** | 0.2001 | **9.23×** | 4.40× |

