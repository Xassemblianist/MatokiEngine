DURUM: DONE
HAT: 3D ışık (Claude, 2026-10-27)

# M9_A — PBR materyal + güneş gölge haritası + gece-gündüz döngüsü

## Dosyalar
- `src/raster/raster.c`, `raster_simd_wasm.c` (yalnız-derinlik kipi, D-098)
- `src/gfx3d/shadow.h/.c` (yeni), `pipeline.h/.c` (`G3D_ATTR_SHADOW_POS`, `g3d_set_shadow`), `camera.h/.c` (`mat4_ortho_rz`)
- `shaders/pbr3d.mtk` (yeni), `src/app/shaders.c/.h` (`ref_shade_pbr3d`)
- `src/app/scene3d.c/.h` (gölge geçişi, PBR materyalleri, gün ışığı modeli, L/N/G tuşları), `src/app/main.c` (`engine_set_jit`, `engine_s3d_bench`)
- `tests/test_gfx3d.c` (`gfx3d_shadow_map`), `docs/DECISIONS.md` (D-098..D-100, Ö-10..Ö-12)

## Kabul
- [x] `make check` 96/96
- [x] Gölge testi: küpün altı gölgede, uzak nokta aydınlık, küpün üstü kendini gölgelemiyor; 24 bit kodlama gidiş-dönüşü
      (test d = 1.0'da 2^24 taşmasını yakaladı → düzeltildi)
- [x] pbr3d.mtk ilk derlemede geçerli wasm (4938 B, 591 IR komutu, 12 varying, 2 doku)
- [x] Tarayıcı: küp/küre/ızgara gölgeleri zeminde, metal kürede speküler parlama; gün saatine göre ortalama gökyüzü
      rengi öğle (140,178,229), şafak (176,161,174), gece (10,12,25)

## Ölçümler (i7-10750H, Chrome, 640×360, başlangıç görüşü, kareler arası 16 ms bekleme, medyan)
| | 1 worker | 8 worker |
|---|---:|---:|
| Lambert (lit3d), JIT | 6.93 ms | 4.99 ms |
| PBR + gölge, JIT | 14.24 ms | 10.44 ms |
| PBR + gölge, C referans | 29.28 ms | 15.66 ms |

1 worker PBR dökümü: gölge geçişi 3.7 ms (bunun 1.5 ms'i derinlik kodlama), shade 4.2 ms (Lambert'te ~2.1 ms),
g3d_vertex iki geçiş toplamı 1.8 ms. JIT, C referansından 2.1× hızlı.

## Ertelenenler
- f32 doku biçimi ile kodlamasız gölge (Ö-10), kademeli gölge (cascade), normal haritası, IBL/ortam küpü,
  trilinear, gölge geçişinin paralelleştirilmesi (8T'de seri kısım baskın).
