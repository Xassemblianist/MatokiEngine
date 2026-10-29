# Matoki Dil Şartnamesi — v0 (taslak)

Matoki, MATOKI motorunun shader/kernel dilidir. Küçük, statik tipli, C-vari.
Tek amacı: piksel başına yazılmış görünen kodu **4 lane wasm SIMD**'e derlemek. Genel amaçlı dil DEĞİLDİR.

## 1. Yürütme modeli: SPMD (ISPC tarzı)

Kullanıcı tek bir piksel için yazar. Derleyici her ifadeyi **2×2 quad'ın 4 pikseli için aynı anda** üretir.
- `varying` (varsayılan): her lane'de farklı olabilir → `v128` (f32x4 / i32x4).
- `uniform`: tüm lane'lerde aynı → skaler wasm değeri (f32 / i32). Uniform bloğundan gelen her şey uniform'dur.
- uniform ⊕ varying → varying (skaler `splat` edilir). Tip denetçisi bunu otomatik takip eder.

Not: `vec3` bir matematik vektörüdür, SIMD lane'i değildir. `vec3` varying değer = 3 adet `v128` (x'ler, y'ler, z'ler) — SoA.

## 2. Örnek

```matoki
// sprite.mtk
uniform tint: vec4;
uniform tex:  texture2d;
in uv: vec2;

fn frag() -> rgba {
    let c = sample(tex, uv);
    if c.a < 0.01 { discard; }
    return c * tint;
}
```

## 3. Sözcükler

- Yorum: `// ...`, `/* ... */`
- Anahtar kelimeler: `fn let var if else for in return discard uniform const true false`
- Sayılar: `1`, `1.0`, `0x1F`, `1e-3`. Tamsayı literali bağlamdan tip alır.

## 4. Tipler

| Tip | Açıklama |
|---|---|
| `f32`, `i32`, `u32`, `bool` | skaler |
| `vec2 vec3 vec4` | f32 vektörleri |
| `ivec2 ivec3 ivec4` | i32 vektörleri |
| `mat3 mat4` | sütun-öncelikli (sadece uniform olarak, v0) |
| `rgba` | `vec4` takma adı (0..1 aralığında renk) |
| `texture2d` | sadece uniform, opak RGBA8 doku tanıtıcısı |
| `texture2d_f32` | sadece uniform, opak tek kanal `f32` doku tanıtıcısı (ör. gölge derinlik tamponu) |

Örtük dönüşüm YOK, `f32(x)`, `i32(x)` açıkça yazılır. Swizzle: `v.xy`, `c.rgb`, `v.zyx`.

## 5. Bildirimler

- `uniform isim: tip;` → uniform bloğuna sırayla, 16-byte hizalı yerleşir. C tarafı yerleşimi derleyicinin döndürdüğü tablodan öğrenir.
  `texture2d` ve `texture2d_f32` uniform bildirimlerinde örnekleyici bayrakları opsiyonel niteliklerle belirtilir: `uniform tex: texture2d @bilinear @clamp;`, `uniform shadow: texture2d_f32 @clamp;` (varsayılan: en yakın komşu + wrap; `@nearest` ve `@wrap` da açıkça yazılabilir; `@mipmap` yalnızca `texture2d` tipinde geçerlidir).
- `in isim: tip;` → varying girdi, `ShadeBatch.varyings`'ten sırayla okunur.
- `const isim = ifade;` → derleme zamanı sabiti.
- `fn isim(p: tip, ...) -> tip { ... }` → yardımcı fonksiyonlar; **hepsi inline edilir**, özyineleme yasak.
- Giriş noktası: `fn frag() -> rgba`.

## 6. Kontrol akışı

- `if/else`: koşul varying ise **maskeli** yürütülür: her iki dal hesaplanır, `v128.bitselect` ile birleşir.
  Optimizasyon: maske tamamen 0 / tamamen 1 ise dal atlanır (`v128.any_true` / `i32x4.all_true`).
  Koşul uniform ise normal wasm `if`.
- `for i in 0..N { }`: N uniform olmalı (v0'da derleme zamanı sabiti). Küçük döngüler açılır.
- `discard;`: lane'i aktif maskeden çıkarır, yazılmaz.
- `return`: sadece fonksiyon sonunda (v0).

## 7. Yerleşik fonksiyonlar

`abs min max clamp mix step smoothstep floor fract sqrt rsqrt dot cross length normalize
 sin cos (polinom, inline) pow exp2 log2 sample(tex, uv) sample_depth(tex, uv) sample_level(tex, uv, level) tex_size(tex)
 dFdx dFdy fwidth`

`sample`: örnekleyici davranışı `texture2d` uniform bildirimindeki `@bilinear`, `@clamp` ve `@mipmap`
nitelikleriyle derleme zamanında seçilir (varsayılan: en yakın komşu + wrap, mip yok).

`sample_depth(tex: texture2d_f32, uv: vec2) -> f32`: `Texture.px`'in doğrudan `f32*` (ör. `fb.depth`)
dizisine işaret ettiği tek kanal `f32` dokuyu `@clamp` / `@wrap` ve `@nearest` / `@bilinear`
niteliklerine göre örnekler; RGBA8 paket açma veya 24-bit sabit nokta çözme yapmadan ham 32-bit
texel kelimesini doğrudan `f32` olarak döndürür (DECISIONS D-107).

### 7.1 Türevler (v1)
`dFdx(x)`, `dFdy(x)`: **kaba (coarse)** quad türevi — 2×2 quad'ın 4 lane'inin hepsine
`x[lane1] - x[lane0]` (dFdx) ve `x[lane2] - x[lane0]` (dFdy). `fwidth(x) = abs(dFdx(x)) + abs(dFdy(x))`.
f32 ve vec2/3/4 için bileşen bazında. Uniform argümanda sonuç 0.

Yardımcı lane'ler: türev kullanan bir shader'da, üçgenin kapsamadığı lane'ler de ("yardımcı lane")
hesaplamaya katılır — varying `if` bölgeleri kapsama maskesine göre değil 4 lane'e göre atlanır,
rasterizer da kapsanmayan lane'lerin varying'lerini düzlem denkleminden doldurur. Böylece türevler her
zaman tanımlı ve deterministiktir. `discard` yalnızca yazımı etkiler.

```matoki
// uzaklığa göre kenarı yumuşayan ızgara çizgisi
let g = abs(fract(uv * 16.0) - vec2(0.5, 0.5));
let w = fwidth(uv * 16.0);
let line = min(g.x / w.x, g.y / w.y);
```

### 7.2 Mipmap (v1)
`uniform t: texture2d @mipmap;` → `sample(t, uv)` seviyeyi uv'nin quad türevlerinden seçer:
`rho² = max(|∂uv/∂x|², |∂uv/∂y|²)` (texel cinsinden), `seviye = floor(log2(rho))` — f32 üs bitlerinden
tamsayı olarak (karekök/log yok, C referansıyla bit-bit aynı). Trilinear yok (v1: tek seviye).
Dokunun mip zinciri `tex_init_mipmapped` + `tex_build_mips` ile kurulur; yerleşim `shade_abi.h`'de.
Zincir küçük kenar 1'e inince biter (256×16 → 5 seviye).

`sample_level(t, uv, level: i32)`: açık seviye; `[0, seviye_sayısı-1]` aralığına kırpılır.

## 8. Derleyici hattı

```
kaynak → lexer → parser → AST → sema (tip + uniform/varying analizi)
      → IR (SSA, tipli, lane-agnostik)
      → opt: sabit katlama, uniform özelleştirme*, CSE, DCE, inline
      → lowering: varying op → SIMD op, uniform op → skaler op, maske yönetimi
      → codegen: mbin ile wasm modülü (memory+table import, tek export: "shade")
      → plat_jit_load → tablo indeksi
```
\* **Uniform özelleştirme**: çağıran "bu uniform şu değerde sabit" derse (ör. `tint = 1,1,1,1`), değer IR'a gömülür ve `c * tint` tamamen silinir. clang'ın yapamadığı şey bu.

Üretilen fonksiyon: `(param $batch i32)`, içinde `count` kadar quad döngüsü. Yerel değişkenler wasm local'leri.

## 9. Hata mesajları

`dosya:satır:sütun: hata: mesaj` + kaynak satırı ve `^` işareti. Derleyici ilk hatada durmaz, en fazla 20 hata toplar.

## 10. v0'da OLMAYANLAR (bilinçli)

Struct, genel dizi, pointer, özyineleme, vertex shader (vertex dönüşümü v0'da C'de),
trilinear filtreleme, ince (fine) türevler.
Bunlar v1+ için `DECISIONS.md`'de tartışılır (Not: `compute` çekirdekleri ve `buffer<T>` desteği Faz F0_A ile eklendi; bkz. §12).

## 11. WGSL Kod Üretimi (WebGPU Arka Ucu, D-108)

Aynı `.mtk` kaynağı CPU için wasm SIMD'e (`mtk_codegen`) derlendiği gibi, WebGPU arka ucu için
optimizasyon sonrası `IrFunc` üzerinden **WGSL**'e (`mtk_wgsl_frag` / `mtk_compile_wgsl`, `src/matoki/wgsl.h`) derlenir.
CPU'da quad başına 4 lane (SPMD) çalışan semantik, GPU'da piksel başına tek çağrı olarak birebir korunur.

### 11.1 Uniform Tamponu ve `MtkLayout` Uyumluluğu
- Sema aşamasında her `uniform` bildirimi `MtkLayout` içinde 16-bayt sınırına (`ALIGN_UP(offset, 16)`) hizalanır.
- WGSL tarafında tüm skaler/vektör/matris uniform'lar tek bir uniform tamponunda paketlenir:
  ```wgsl
  struct MtkUniforms {
      data: array<vec4<f32>, N>, // N = (uniform_bytes + 15) / 16
  };
  @group(0) @binding(0) var<uniform> mtk_u: MtkUniforms;
  ```
- `byte_offset` ofsetindeki her 32-bit bileşen `mtk_u.data[byte_offset / 16].<xyzw[(byte_offset / 4) % 4]>` ile okunur
  (`i32`/`u32` uniform'lar için `bitcast<i32>(...)`). Böylece CPU `ShadeBatch.uniforms` bellek yerleşimi ile
  WebGPU uniform buffer yerleşimi **bayt-bayt aynıdır** (`vec3` veya `mat3` dolgu farkı oluşmaz).

### 11.2 Dokular ve Örnekleyiciler (`@group(0)`)
- Her `texture2d` slotu `s` (`0 <= s < texture_count`) için iki bağlama üretilir:
  - `@group(0) @binding(1 + 2*s) var tex_<s>: texture_2d<f32>;`
  - `@group(0) @binding(2 + 2*s) var samp_<s>: sampler;`
- Doku boyutları `textureDimensions(tex_<s>, 0)` ve mip sayısı `textureNumLevels(tex_<s>)` ile sorgulanır;
  texel okumaları `textureLoad(tex_<s>, vec2<i32>(wx, wy), level)` + `pack4x8unorm` ile CPU referansıyla aynı
  en yakın/çift doğrusal/wrap/clamp/mipmap semantiğini korur.

### 11.3 Varying `@location(n)` ve Vertex Shader Sözleşmesi (`mtk_wgsl_vert`)
- Fragment girdisi `struct FragIn`, `@builtin(position) frag_pos: vec4<f32>` ve her `in` bildirimi için
  bildirim sırasıyla `@location(0..k-1)` (`f32`, `vec2<f32>`, `vec3<f32>`, `vec4<f32>`) içerir.
- `mtk_wgsl_vert(attrs, attr_count, ...)` fonksiyonu `gfx3d` boru hattının `G3dAttr` listesine (`UV`, `NORMAL`,
  `VIEW_DEPTH`, `COLOR`, `WORLD_POS`, `SHADOW_POS`) göre `@group(1) @binding(0)` altında `VsUniforms` (`mvp`,
  `model`, `normal_mat`, `shadow_mat`) ve eşleşen `@location(0..k-1)` çıkışlarını üreten WGSL vertex shader'ını üretir.
- Türevler `IR_DERIV_X/Y` → `dpdx`/`dpdy`, `IR_DISCARD` → `if (cond) { discard; }`, `IR_SELECT` → `select(if_false, if_true, cond)` olarak çevrilir.

## 12. Compute Çekirdekleri (`fn compute()`, Faz F0_A)

Matoki aynı SPMD modeliyle `N` eleman üzerinde çalışan `compute` çekirdeklerini hem CPU (4-lane wasm SIMD + `job_parallel_for`) hem de WebGPU (`@compute @workgroup_size(64)`) hedeflerine derler.

### 12.1 Sözdizimi ve Yerleşikler
- **Buffer bildirimleri**: `buffer isim: buffer<f32>;`, `buffer isim: buffer<u32>;` veya `buffer isim: buffer<vec4>;`.
  Bildirim sırasına göre `MtkLayout` içinde `MTK_SYM_BUFFER` (`slot = 0 .. buffer_count-1`, en fazla 16 adet) olarak kaydedilir.
- **Giriş noktası**: `fn compute() { ... }` (parametre almaz, dönüş değeri yoktur; erken `return;` aktif lane maskesini günceller).
- **`global_id`**: Yalnızca compute çekirdeğinde geçerli, `u32` (varying) yerleşik değişkeni; çalışılan elemanın küresel indeksini (`0 .. N-1`) verir.
- **Buffer okuma/yazma**: `let v = buf[idx];`, `buf[idx] = expr;`, `buf[idx] += expr;` ve `buffer<vec4>` üzerinde bileşen/swizzle yazımı (`buf[idx].yz = vec2(...)`).
- **Atomik toplama**: `let old: u32 = atomic_add(buf, idx, val);` — yalnızca `buffer<u32>` üzerinde geçerlidir; önceki değeri döndürür.
- **Tamsayı literalleri**: İsteğe bağlı `u`/`U` soneki (`0u`, `255u`, `0xFFu`) doğrudan `u32` sabiti üretir.

### 12.2 CPU Hedefi (`mtk_codegen_compute` & `mtk_compute_dispatch`)
- Üretilen wasm modülü `"compute"` (ve uyumluluk için `"shade"`) adıyla `(param $batch i32)` imzalı fonksiyonu dışa aktarır.
- `MtkComputeBatch` (`src/matoki/codegen.h`) başlangıç indeksi (`start`), eleman sayısı (`count`), toplam boyut (`total_count`), `uniforms` işaretçisi ve `buffers[0..15]` taban adreslerini taşır.
- Çekirdek döngüsü 4'lü tam SIMD gruplarını hızlı yolda (`is_fast_body`) işler; 4'ün katı olmayan kuyruk elemanlarını (`end_gid - fast_end`) maskeli kuyruk bloğunda sınır taşması olmadan yürütür.
- **Vektörleştirilmiş erişimler**:
  - `buf[global_id + c]` (`buffer<f32>` / `buffer<u32>`): tek `v128.load` / `v128.store`.
  - `buf[global_id >> 2]` (`buffer<f32>` / `buffer<u32>`): 4 lane aynı kelimeyi okuduğundan tek `v128.load32_splat`.
  - `buf[global_id]` (`buffer<vec4>`) ve `buf[global_id * 4 + 0..3]` (`buffer<f32>` / `buffer<u32>`): 4× `v128.load` / `v128.store` + `i8x16.shuffle` ile yerinde 4×4 AoS↔SoA transpoze.
  - `atomic_add`: aktif lane'ler üzerinde wasm `i32.atomic.rmw.add` (`0xFE 0x1E`).
- `mtk_compute_dispatch(fn, batch, total_count, grain)` (`src/matoki/matoki.c`): 4-elemanlık SIMD gruplarını `job_parallel_for` üzerinden mevcut işçi thread'lerine (`1T..8T`) dağıtır.

### 12.3 WGSL Hedefi (`mtk_wgsl_compute`)
- `uniform_bytes > 0` ise `@group(0) @binding(0) var<uniform> mtk_u: MtkUniforms;` üretilir.
- Her `buffer` slotu `s` için `@group(0) @binding(1 + s) var<storage, read_write> buf_<s>: array<T>;` üretilir (`T` = `f32`, `u32`, `vec4<f32>` veya `atomic_add` kullanılan tamponlarda `atomic<u32>`).
- Giriş noktası `@compute @workgroup_size(64) fn compute(@builtin(global_invocation_id) gid: vec3<u32>)` olarak üretilir; `IR_GLOBAL_ID` → `gid.x`, `IR_ATOMIC_ADD` → `atomicAdd(&buf_<s>[idx], val)`.

