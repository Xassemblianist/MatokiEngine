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
| `texture2d` | sadece uniform, opak tanıtıcı |

Örtük dönüşüm YOK, `f32(x)`, `i32(x)` açıkça yazılır. Swizzle: `v.xy`, `c.rgb`, `v.zyx`.

## 5. Bildirimler

- `uniform isim: tip;` → uniform bloğuna sırayla, 16-byte hizalı yerleşir. C tarafı yerleşimi derleyicinin döndürdüğü tablodan öğrenir.
  `texture2d` uniform bildirimlerinde örnekleyici bayrakları opsiyonel niteliklerle belirtilir: `uniform tex: texture2d @bilinear @clamp;` (varsayılan: en yakın komşu + wrap; `@nearest` ve `@wrap` da açıkça yazılabilir).
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
 sin cos (polinom, inline) pow exp2 log2 sample(tex, uv) sample_level(tex, uv, level) tex_size(tex)
 dFdx dFdy fwidth`

`sample`: örnekleyici davranışı `texture2d` uniform bildirimindeki `@bilinear`, `@clamp` ve `@mipmap`
nitelikleriyle derleme zamanında seçilir (varsayılan: en yakın komşu + wrap, mip yok).

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

Struct, dizi, pointer, özyineleme, vertex shader (vertex dönüşümü v0'da C'de), compute kernel'leri,
trilinear filtreleme, ince (fine) türevler.
Bunlar v1+ için `DECISIONS.md`'de tartışılır.

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
