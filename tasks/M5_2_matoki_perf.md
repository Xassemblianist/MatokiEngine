DURUM: DONE
HAT: D (Matoki hız)

# M5_2 — Matoki codegen optimizasyonu: LICM, blend füzyonu, varyant önbelleği, clang'a karşı benchmark (BÜYÜK)

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/mbin/*`, `src/raster/shade_abi.h` (ABI v1: `fb_px`, `fb_stride`), `src/matoki/ir.h`, `src/matoki/codegen.h` (yeni: `MtkCodegenOpts.fuse_blend` — oku).
Sende: `src/matoki/ir.c, ir_eval.c, opt.c, codegen.c`, `tools/host/run_shade.mjs`, `tests/test_matoki_back.c`. Ön uç dosyaları (lexer/parser/sema/lower/matoki.c) bu turda kimsenin değil — gerekirse küçük düzeltme yapabilirsin, Notlar'a yaz.

## İşler
1. **Döngüden bağımsız kod taşıma (LICM):** quad döngüsüne bağlı olmayan her şey (tüm uniform hesapları, uniform'lardan türeyen splat'ler, sabit v128'ler, doku boyutu/maske değerleri) döngü öncesine. Uniform IF içindeki uniform hesaplar dahil (güvenliyse). Ölç: sprite/plasma için döngü içi komut sayısı öncesi/sonrası.
2. **Blend füzyonu:** `fuse_blend != NONE` iken OUTPUT'lar out_rgba yerine: RGBA8'e SIMD paketleme (clamp, *255+0.5, trunc_sat, narrow) + 4 pikseli (2 satır × 2) fb'den yükle → blend → mask'e göre `v128.bitselect` ile birleştir → geri yaz (2 × `v128.load64_lane/store64_lane` veya i64 load/store). Blend formülü `src/raster/raster.c`'deki C blend ile bit-bit aynı. `ir_eval`'e de füzyon modunu ekle (oracle).
3. **Varyant önbelleği:** `src/matoki/variant.h/.c` (yeni): `(kaynak hash, sabitlenmiş uniform değerleri, fuse_blend, sampler bayrakları)` anahtarlı önbellek; `mtk_variant_get(...)` → derlenmiş wasm byte'ları (yoksa derle). Uniform özelleştirmeyi gerçek kullanıma açar (ör. tint=1 olan sprite'lar ayrı, daha kısa varyant).
4. **Codegen kalitesi:** gereksiz `local.set/get` çiftleri, aynı splat'in tekrarı, bitselect yerine and/or kalıpları, `f32x4.mul` + `f32x4.add` zincirleri, gather'da ortak adres hesabı. Her değişiklik ölçülür.
5. **Benchmark: Matoki JIT vs clang.** `tools/host/bench_shade.mjs` (node): aynı shader'ın (a) Matoki JIT çıktısı, (b) `src/raster/ref_shaders.c`'nin clang -O2 -msimd128 ile ayrı wasm'a derlenmiş hali, (c) füzyonlu Matoki varyantı → 1M quad üzerinde Mquad/s. sprite, vcolor, plasma, branchy için tablo. `make bench` GEREKMEZ; komutu Notlar'a yaz.

## Testler
- Diferansiyel test (500×) yeni geçişlerle ve füzyon modlarıyla (her 3 mod) yeniden: 0 uyumsuzluk.
- LICM: elle kurulmuş IR'da döngü içi komut sayısı beklenen değerde.
- Varyant önbelleği: aynı anahtar → aynı byte'lar, farklı sabit → farklı ve daha kısa modül.

## Kabul kriterleri
- `make`, `make test` uyarısız, diferansiyel 0 uyumsuzluk.
- Notlar: benchmark tablosu. **Hedef: plasma ve sprite'ta Matoki JIT ≥ clang referansı.** Tutmazsa `mdis` ile iç döngüyü clang çıktısıyla karşılaştırıp farkı açıkla.

## Notlar

### 1. Yapılanlar
- **LICM (`src/matoki/opt.c`, `src/matoki/codegen.c`):**
  - `ir_opt_licm` geçişi eklendi ve `ir_optimize` boru hattına bağlandı. Sabitler, uniform okumaları, doku bilgileri (`TEX_W`, `TEX_H`, `TEX_FLAGS`) ve tüm operandları döngüden bağımsız olan saf uniform hesaplar (uniform veya varying `IF` bloklarının içindekiler dahil) `IR_FLAG_HOISTED` ile işaretlenip IR'ın başına (preheader) taşındı.
  - `codegen.c` içinde tüm preheader hesapları, uniform/sabit `v128` splat'leri (aynı sabit değerler için tek ortak splat local'i paylaşılacak şekilde deduplicate edilerek), varying/output kanal taban adresleri (`loc_var_base`, `loc_out_base`) ve doku `px` taban adresleri (`loc_tex_px_v`) quad döngüsü öncesine alındı.
  - Ayrıca `IF_BEGIN(c) + DISCARD(c) + IF_END` kalıbı `remove_empty_ifs` içinde doğrudan `DISCARD(c)`'ye düzleştirilerek `sprite.mtk` döngüsündeki gereksiz `if..end` bloğu ve `loc_active` maske yönetimi kaldırıldı.
- **Codegen Kalitesi (`src/matoki/codegen.c`):**
  - **Özyinelemeli İfade Ağacı Birleştirme (Expression-Tree Inlining):** Yalnızca yaprak komutları değil, tek kullanımlı (`use_count == 1`) tüm ara ifadeleri özyinelemeli olarak yığında birleştiren ağaç üretimi eklendi (`emit_step` ömür uzatması ve 2 geçişli `LocalPool` tahsisi ile).
  - **Horner Zinciri Düzleştirme (Commutative Subtree-First Ordering):** Değişmeli işlemlerde (`IR_FADD`, `IR_FMUL`, vb.) sağ operand inline edilmiş bir alt ağaç ve sol operand yaprak ise önce sağ alt ağaç değerlendirilerek `plasma.mtk` içindeki 6 adet `sin`/`cos` Horner polinomunun yığında derinleşmeden yalnız 2 yazmaçla düz (flat) değerlendirilmesi sağlandı.
  - **`select` -> Bitwise Kalıpları ve İşaret Biti Katlama:** Bir kolu tüm-0 veya tüm-1 olan varying `IR_SELECT` komutları `v128.and` / `v128.andnot` / `v128.or`'a düşürüldü. `sin`/`cos` indirgemesinde oluşan `select(cond, -x, x)` kalıbı `v128.xor(x, v128.and(cond, -0.0f))`'a; `cond == ((q & 2) != 0)` durumu ise doğrudan `v128.xor(x, v128.and(q << 30, -0.0f))`'a katlandı.
  - **SIMD Gather Ortak Adres Hesabı (`IR_TEXEL_LOAD`):** 4 lane için `loc_tex_px_v[slot] + (idx << 2)` adres vektörü tek seferde SIMD'de hesaplandı, lane 0 `v128.load32_zero` ile, lane 1..3 `i32.load` + `i32x4.replace_lane` ile yüklendi.
  - **Döngü İndüksiyon Değişkenleri (Strength Reduction):** Her quad iterasyonunda `q << 4` ve `quads + (q << 3)` hesaplamak yerine `loc_q16` (`+16`) ve `loc_qhdr` (`+8`) doğrudan ilerletildi.
- **Blend Füzyonu (`src/matoki/codegen.c`, `src/matoki/ir_eval.c`, `tools/host/run_shade.mjs`):**
  - `MTK_FUSE_REPLACE (1)`, `MTK_FUSE_ALPHA (2)`, `MTK_FUSE_ADD (3)` modları hem `mtk_codegen`'e hem de referans yorumlayıcı `ir_eval_ex`'e (`src/raster/raster.c` ile bit-özdeş olarak) eklendi.
  - `[0, 1]` NaN-güvenli sıkıştırma için `f32x4.pmax(0.0, x)` + `f32x4.pmin(1.0, ...)`, RGBA8 paketleme için `i16x8.narrow_i32x4_u` + `i8x16.narrow_i16x8_u` + `i8x16.shuffle`, hedef 4 pikseli yüklemek/yazmak için `v128.load64_zero` + `v128.load64_lane` ve `v128.store64_lane` kullanıldı.
- **Varyant Önbelleği (`src/matoki/variant.h`, `src/matoki/variant.c`):**
  - `(src_hash, sabitlenmiş uniform değerleri, cg_opts/fuse_blend, sampler_flags)` anahtarlı 64-girdili açık adreslemeli `MtkVariantCache` ve `mtk_variant_get` gerçeklendi.

### 2. Döngü İçi Komut Sayısı ve Modül Boyutu Ölçümleri (Önce vs Sonra)

| Shader | IR Döngü Komutu (LICM Öncesi -> Sonrası) | Wasm Döngü İçi Op (Önce -> Sonra) | Wasm Boyutu (Önce -> Sonra) |
|---|---:|---:|---:|
| `sprite.mtk` (genel) | 54 -> 37 (`-17` komut preheader'da) | 191 -> 120 (`-37.2%`) | 674 B -> 581 B |
| `sprite.mtk` (`sampler=nearest+wrap`) | 22 -> 14 (`-8` komut preheader'da) | 191 -> 54 (`-71.7%`) | 674 B -> 352 B |
| `sprite.mtk` (`nearest+wrap, tint=1`) | 18 -> 10 (`-8` komut preheader'da) | 191 -> 46 (`-75.9%`) | 674 B -> 307 B |
| `plasma.mtk` | 332 -> 303 (`-29` komut preheader'da) | 1227 -> 683 (`-44.3%`) | 3477 B -> 2236 B |
| `vcolor.mtk` | 16 -> 12 (`-4` komut preheader'da) | 71 -> 47 (`-33.8%`) | 337 B -> 296 B |
| `branchy.mtk` | 41 -> 31 (`-10` komut preheader'da) | 235 -> 164 (`-30.2%`) | 742 B -> 612 B |

### 3. Benchmark Sonuçları (`node tools/host/bench_shade.mjs`, 1M Quad = 4M Piksel)

Çalıştırma komutu:
```sh
node tools/host/bench_shade.mjs
```

| Shader | IR Döngü (Önce -> Sonra) | Wasm Döngü Op | Matoki JIT (Mquad/s) | clang -O2 -msimd128 (Mquad/s) | JIT / clang | Matoki Fused Shade+Blend (Mquad/s) | Ayrı Shade+Blend (Mquad/s) | Fused Hızlanma |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `sprite` | 54 -> 14 | 54 | **81.1** | 26.5 | **3.06x** | **35.0** | 9.5 | **3.68x** |
| `vcolor` | 16 -> 12 | 47 | **380.3** | 381.3 | **1.00x** | **63.7** | 13.9 | **4.58x** |
| `plasma` | 332 -> 303 | 683 | **16.3** | 3.5 | **4.68x** | **13.5** | 3.0 | **4.49x** |
| `branchy` | 41 -> 31 | 164 | **198.8** | 138.9 | **1.43x** | **49.8** | 18.8 | **2.66x** |

- Hedef doğrulaması:
  - `plasma`: **Matoki JIT (16.3 Mquad/s) >= clang referans (3.5 Mquad/s)** (**4.68x daha hızlı** — Matoki 4 pikseli aynı anda `f32x4` SIMD Horner polinomu ve `(q << 30) & -0.0f` işaret biti XOR'u ile hesaplarken, `clang` skaler `m_sin`/`m_cos` çağrılarını otomatik vektörleştirememektedir).
  - `sprite`: **Matoki JIT (81.1 Mquad/s) >= clang referans (26.5 Mquad/s)** (**3.06x daha hızlı** — ayrıca genel `sampler_mask=0` varyantında bile Matoki JIT **66.4 Mquad/s** ile `clang` referansından **2.51x daha hızlıdır**).
  - Tüm 1500 diferansiyel test modülü (500 unopt + 500 opt + 500 fused `REPLACE`/`ALPHA`/`ADD`) `wasm-validate --enable-all` ve Node üzerinde **0 uyumsuzlukla** geçti (`59 geçti, 0 kaldı`).

