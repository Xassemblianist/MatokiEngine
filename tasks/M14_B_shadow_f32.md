# M14_B — Gölge haritası f32 doku (Ö-10): kodlama/çözme kalkar

Durum: DONE (argon)

## Amaç
DECISIONS Ö-10: Matoki'ye f32 doku biçimi ekle (ör. `uniform shadow: texture2d_f32;` + `sample_depth`/`load` ile
tek kanal f32 okuma). Gölge haritası derinliği RGB24'e kodlanmadan doğrudan `fb.depth`'ten okunur;
`shadow_end`'deki kodlama (~0.6–1.1 ms) ve pbr3d.mtk'deki çözme kalkar. Ö-10'u D-107 olarak KABUL'e taşı.

## Dosyalar
- `src/matoki/*` (lexer/parser/sema/ir/lower/codegen: yeni doku türü), `src/raster/shade_abi.h` (Texture'a biçim alanı gerekiyorsa)
- `src/gfx3d/shadow.c/.h`, `shaders/pbr3d.mtk`, `src/app/shaders.c` (`ref_shade_pbr3d`), `src/app/r3d.c`
- `tests/test_matoki_*.c`, `tests/test_gfx3d.c`
- `docs/DECISIONS.md`, `docs/MATOKI.md`

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız
- [x] JIT ve C referans pbr3d aynı sonucu verir (mevcut JIT-ref karşılaştırma testleri)
- [x] Gölge sonucu eskisiyle eşdeğer (24-bit kuantalama farkı dışında) — test
- [x] Kare başına kazanç ölçümü (shadow_encode kalkışı + shader çözme farkı) Notlar'da

## Notlar
- **Matoki `texture2d_f32` ve `sample_depth` (D-107):**
  - `src/raster/shade_abi.h` içine `TEX_FLAG_F32` (`8u`) bayrağı eklendi. `sizeof(u32) == sizeof(f32) == 4` olduğundan `Texture.px` yeni alan eklenmeden doğrudan `f32*` tamponuna (`(u32*)s->fb.depth`) işaret eder.
  - `src/matoki/matoki.h/.c`, `parser.c`, `sema.c`, `lower.c`, `variant.c`, `ir.h/.c` içine `MTK_TYPE_TEXTURE2D_F32` tipi, `mtk_type_is_texture()` yardımcısı ve `sample_depth(tex: texture2d_f32, uv: vec2) -> f32` yerleşiği (`ir_emit_sample_depth`) eklendi.
  - `ir_emit_sample_depth` mevcut `IR_TEXEL_LOAD` + `IR_BITCAST_I2F` komutlarını kullanır; Wasm SIMD yolunda `IR_BITCAST_I2F` sıfır maliyetlidir (0 komut) ve yeni IR opcode gerekmez. En yakın komşu (`@nearest`) ve çift doğrusal (`@bilinear`) filtrelemeyi `@wrap` / `@clamp` ile destekler.
- **Gölge haritası ve PBR shader (`shadow.c/.h`, `shaders/pbr3d.mtk`, `src/app/shaders.c`):**
  - `shadow_init` içinde `s->tex.px = (u32*)s->fb.depth` ve `s->tex.flags = TEX_FLAG_CLAMP | TEX_FLAG_F32` olarak bağlandı; `shadow_end` içindeki 1024×1024 RGB24 kodlama geçişi (`shadow_encode`) tamamen kaldırıldı.
  - `shaders/pbr3d.mtk` içinde `uniform shadow: texture2d_f32 @clamp;` kullanıldı; `sdepth()` fonksiyonu kaldırılıp `lit_tap` doğrudan `step(sample_depth(shadow, p), z)` olarak sadeleştirildi.
  - `src/app/shaders.c` içindeki `ref_shade_pbr3d` C referansı da `pbr_sample_depth` ile doğrudan `f32` gölge derinliğini okuyacak şekilde güncellendi.
- **Testler:**
  - `tests/test_matoki_front.c` (`matoki_texture2d_f32_and_sample_depth`): `texture2d_f32` sembol yerleşimi, `sample_depth` en yakın komşu + bilinear + clamp değerlendirmesi, `tex_size` ve negatif sema testleri.
  - `tests/test_matoki_back.c` (`matoki_pbr3d_shadow_f32_jit_and_ref`): `shaders/pbr3d.mtk` için `ir_eval` ↔ Wasm SIMD JIT ↔ Wasm fused bit-bit eşdeğerliği, C referansı (`ref_shade_pbr3d`) ile eşdeğerlik ve önceki 24-bit RGB kodlu gölge + `sdepth()` shader'ı ile eşdeğerlik (`<= 1e-5`).
  - `tests/test_gfx3d.c` (`gfx3d_shadow_map`): doğrudan `f32` gölge dokusunun `sm.fb.depth` ile birebirliği ve eski 24-bit RGB nicelemesiyle `1 / (2^24 - 1)` farkına kadar eşdeğerliği.
- **Benchmark sonuçları:**
  - **`pbr3d.mtk` derleme / codegen istatistikleri:**
    | Metrik | Önceki (`texture2d` + `sdepth`) | Yeni (`texture2d_f32` + `sample_depth`) | Fark |
    |---|---|---|---|
    | Toplam IR komutu | 565 | 514 | -51 (-%9.0) |
    | Döngü içi IR komutu (`loop_inst_count`) | 475 | 427 | -48 (-%10.1) |
    | Wasm döngü içi op (`wasm_loop_ops`) | 1324 | 1224 | -100 (-%7.6) |
    | Wasm modül boyutu (`wasm_len`) | 4291 B | 3973 B | -318 B (-%7.4) |
  - **Çalışma zamanı (V8 Node.js Wasm SIMD `-O2 -msimd128`):**
    - `shadow_encode` (1024×1024 = 1,048,576 texel): **1.063 ms → 0.000 ms** (tamamen kalktı; 8T paralel yolda da ~0.56 ms + iş kuyruğu bariyeri tasarrufu).
    - `pbr3d.mtk` Wasm SIMD gölgelendirme (512 batch × 256 quad = 524,288 piksel, 31 tekrar medyanı): **18.139 ms → 17.053 ms** (**-1.085 ms**, **%6.0** hızlanma; 138.4 ns/quad → 130.1 ns/quad).
    - Tek iş parçacığında kare başına toplam kazanç (1024² gölge + ~524k PBR piksel): **~2.15 ms/kare** (`1.063 ms` kodlama + `1.085 ms` shader çözme).
- **Ertelenen:** Yok.

