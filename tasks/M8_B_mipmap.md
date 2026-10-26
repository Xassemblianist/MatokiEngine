DURUM: DONE
HAT: 3D temel (tek ajan, 2026-10-27)

# M8_B — Matoki türevleri (dFdx/dFdy/fwidth) ve mipmap'li örnekleme

İptal edilen `M8_1_3d.md` Faz 2'nin uygulanmış hali (D-086, D-087).

## Dosyalar
- `src/raster/shade_abi.h` — `Texture.levels` (eski `_pad`), `TEX_FLAG_MIPMAP`, `TEX_OFF_LEVELS`, mip yerleşimi + semantik yorumu
- `src/matoki/ir.h/.c` — `IR_DERIV_X/Y`, `IR_TEX_LEVELS`, `ir_emit_mip_level`, `ir_emit_sample_level`, mip'li `ir_emit_sample`
- `src/matoki/ir_eval.c`, `opt.c`, `codegen.c` — yeni op'lar, yardımcı lane kontrol akışı
- `src/matoki/parser.c`, `sema.c`, `lower.c` — `@mipmap`, `dFdx`, `dFdy`, `fwidth`, `sample_level`
- `src/raster/texture.h/.c` — `tex_init_mipmapped`, `tex_build_mips`, `tex_mip_offset`
- `src/raster/ref_shaders.h/.c` — `ref_tex_mip_level`, `ref_tex_level_view`, `ref_sample_tex_level`
- `src/ui/editor.c` (vurgu listesi), `src/bench/bench_main.c`, `src/img/image.c`, `tests/test_kernels.c` (`_pad` → `levels`)
- `tests/test_matoki_mip.c` (yeni), `tests/test_matoki_back.c` + `tools/host/run_shade.mjs` (mip zinciri formatı, DERIV + mip'li sample üretimi)
- `shaders/lit3d.mtk` (`@mipmap`), `src/app/shaders.c` (C ref mip), `src/app/scene3d.c` (M tuşu), `docs/MATOKI.md`

## Kabul kriterleri
- [x] `make check` uyarısız, 69/69 test
- [x] Diferansiyel test: 500 rastgele program (DERIV, @mipmap sample, sample_level, rastgele `levels` 0..4) ×
      (unopt, opt, füzyonlu) — `ir_eval` ↔ node wasm 0 uyumsuzluk
- [x] Matoki @mipmap `sample` ↔ C `ref_sample_tex_level(ref_tex_mip_level)`: 4 örnekleyici × ~25.6k quad
      (~102k lane), 0 bit farkı, 6 seviyenin hepsi kullanıldı
- [x] Seviye seçimi bilinen ölçeklerde (0.25×..4096×), NaN → 0
- [x] Mip zinciri yerleşimi (kare olmayan dahil: 256×16, 16×256, 128×32)
- [x] 3D demoda M tuşu: kapalıyken uzak zeminde moiré halkaları, açıkken griye yumuşak geçiş (ekran görüntüsüyle)

## Notlar
- **Bulunan anlambilim sorunu (D-087):** türev, kapsanmayan lane'in değerini kapsanan lane'e taşır; varying
  `if` bölgesi yalnız kapsanan lane'lere göre atlanınca kapsanmayan lane çöp kalıyordu. Diferansiyel test
  opt öncesi/sonrası farkıyla yakaladı. Türev içeren shader'larda kontrol akışı 4 lane ile yürüyor
  (türevsiz shader'ların üretilen kodu değişmedi).
- Raster kapsanmayan lane'lerin varying'lerini zaten düzlem denkleminden hesaplıyordu (M8_A'da doğrulandı).

### Ölçümler (i7-10750H, Chrome 156, 640×360, 3D başlangıç görüşü, 1 thread, JIT, 60 kare ortalaması)
| | mip kapalı | mip açık |
|---|---:|---:|
| `lit3d` wasm boyutu | 1656 B | 2359 B |
| `lit3d` IR komutu (opt sonrası) | 193 | 238 |
| shade (ms/kare) | 2.14 | 3.10 |
| kare (ms, medyan) | 7.89 | 8.93 |

Mip'in maliyeti: shade +%45 (tüm dokular için seviye hesabı + taban/boyut kaydırmaları). Seviye
quad içinde sabit olduğundan ileride uniform-quad optimizasyonu (lane 0'da skaler hesap) düşünülebilir;
ölçülmeden öneri olarak kalır.

### Ertelenenler
- Trilinear filtreleme, sRGB-doğru mip indirgeme, anizotropik filtre.
- `texel_fetch` / `bitcast_f32` dil yerleşikleri (gölge haritası işi gelince).
