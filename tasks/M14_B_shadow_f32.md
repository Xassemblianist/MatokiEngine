# M14_B — Gölge haritası f32 doku (Ö-10): kodlama/çözme kalkar

Durum: TODO

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
- [ ] `make check` hatasız + uyarısız
- [ ] JIT ve C referans pbr3d aynı sonucu verir (mevcut JIT-ref karşılaştırma testleri)
- [ ] Gölge sonucu eskisiyle eşdeğer (24-bit kuantalama farkı dışında) — test
- [ ] Kare başına kazanç ölçümü (shadow_encode kalkışı + shader çözme farkı) Notlar'da
