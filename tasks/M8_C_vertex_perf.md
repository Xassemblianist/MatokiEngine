DURUM: DONE
HAT: 3D temel (tek ajan, 2026-10-27)

# M8_C — 3D vertex aşaması hızlandırma

M8_A'da ölçülen darboğaz: seri vertex aşaması (8T'de raster+shade paralelleşince baskın kalıyordu).

## Dosyalar
- `src/gfx3d/vertex.h`, `vertex.c` (skaler), `vertex_wasm.c` (f32x4) — SoA konum dönüşümü
- `src/gfx3d/pipeline.c/.h` — SoA klip konumları, `ClipVert` yalnız kırpılan üçgenler için talep üzerine,
  tamamen görünür nesnede outcode atlama, `g3d_set_opt` ölçüm anahtarları, `draws_inside` istatistiği
- `src/app/main.c` (`engine_g3d_opt` export'u), `src/app/scene3d.c` (`s3d_clear`/`s3d_flush` profil bölgeleri)
- `tests/test_gfx3d.c` (`gfx3d_transform_positions_exact`)

## Kabul
- [x] `make check` 80/80
- [x] Görüntü değişmedi: 3D sahne alanında (üst çubuk ve HUD'daki zaman yazıları hariç) tüm optimizasyon
      kombinasyonları skaler yolla piksel-piksel aynı (0 fark); host'ta skaler dönüşüm `mat4_mul_vec4` ile bit-bit aynı

## Ölçümler (i7-10750H, Chrome 156, 640×360, 3D başlangıç görüşü, 871 çizim / ~21k köşe, 100 kare ortalaması)
| Sürüm | `g3d_vertex` (1T) |
|---|---:|
| M8_A (köşe başına 80 B ClipVert + 80 B RasterVert, `mat4_mul_vec4` çağrısı) | 1.25 ms |
| ClipVert yalnız kırpmada, skaler dönüşüm (`opt=0`) | 1.09 ms |
| + SoA f32x4 dönüşüm (`opt=1`) | 0.83 ms |
| + tamamen içerideki nesnede outcode atlama (`opt=3`) | **0.78 ms (1.6×)** |

Kare süresi (mip açık): 1T 8.47 → 8.25 ms. 8T: **4.10 ms** (karelerin arasında 16 ms bekleyerek, rAF'a benzer).

### Ölçüm yöntemi uyarısı
`engine_frame` art arda beklemesiz çağrılırsa 8T 6.5 ms çıkıyor: worker'lar kareler arasında JS olay
döngüsüne dönemiyor ve paralel flush uyanma gecikmesiyle uzuyor (`s3d_flush` 3.6 ms duvar saati).
Gerçekçi aralıklı ölçümde 4.1 ms. Thread'li ölçümler bundan sonra aralıklı alınmalı.

## Sonraki adım önerisi (ölçülmedi)
Kalan vertex maliyetinin büyüğü çizim başı sabit iş (871 küçük küp: matris çarpımı + tersi,
`raster_set_*` her çizimde tri tamponunu işliyor). Seçenekler: aynı durumdaki çizimlerde `raster_set_*`'ı
no-op yapmak, örneklemeli (instanced) çizim, vertex aşamasını çizimler arasında paralelleştirmek.
