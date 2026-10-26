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
 sin cos (polinom, inline) pow exp2 log2 sample(tex, uv)`

`sample`: örnekleyici davranışı `texture2d` uniform bildirimindeki `@bilinear` ve `@clamp` nitelikleriyle derleme zamanında seçilir (varsayılan: en yakın komşu + wrap). v2'de quad türevleriyle mipmap.

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

Struct, dizi, pointer, özyineleme, vertex shader (vertex dönüşümü v0'da C'de), compute kernel'leri.
Bunlar v1+ için `DECISIONS.md`'de tartışılır.
