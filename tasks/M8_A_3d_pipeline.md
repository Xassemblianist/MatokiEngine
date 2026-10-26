DURUM: DONE
HAT: 3D temel (tek ajan, 2026-10-27)

# M8_A — 3D boru hattı çekirdeği: mesh, vertex aşaması, kırpma, derinlik, kamera, demo sahnesi

İptal edilen `M8_1_3d.md` Faz 1'inin daraltılmış hali (DECISIONS D-072). Amaç: üzerine 3D oyun
kurulabilecek doğru ve ölçülmüş bir temel.

## Dosyalar
- `src/gfx3d/mesh.h/.c` — SoA `Mesh`, küp/düzlem/küre üreteçleri, normal + sınır hesabı
- `src/gfx3d/clip.h/.c` — outcode, guard band türetimi, Sutherland–Hodgman (yakın + guard düzlemleri)
- `src/gfx3d/camera.h/.c` — uçuş/orbit kamera, `mat4_perspective_rz` (reverse-Z, sonsuz uzak)
- `src/gfx3d/pipeline.h/.c` — `G3dContext`, `G3dMaterial`, `g3d_draw`, frustum culling, istatistikler
- `src/raster/raster.c/.h`, `raster_simd_wasm.c` — early-Z yazım hatası düzeltmesi + `raster_set_depth_ex`
- `src/app/scene3d.h/.c`, `src/app/main.c` (5. sahne), `src/app/shaders.c/.h` (`ref_shade_lit3d`)
- `shaders/lit3d.mtk`, `tests/test_gfx3d.c`, `web/platform.js` (yalnız `globalThis.matoki` hata ayıklama kancası)

## Kabul kriterleri
- [x] `make check` uyarısız, tüm testler geçiyor (66/66, wasm SIMD ↔ host bit-bit dahil)
- [x] Yakın düzlem kırpması: 10.000 rastgele üçgende varying hatası < 1e-5, tüm köşeler düzlemin içinde
- [x] Çatlak yok: yakın düzlemi + guard band'i kesen 40×40 ızgara zeminde 0 çift boyama, 0 delik
- [x] Perspektif-doğru UV: ışın izleme referansına karşı < 1e-4
- [x] Arka yüz ayıklama + derinlik: ayıklamasız/derinlikli çizim ayıklamalı çizimle piksel-piksel aynı
- [x] Tarayıcıda 5:3D sahnesi: dokulu zemin, 1000 küplük ızgara, dönen küp + küre; kameranın küpün
      içinden geçişinde bozulma yok (ekran görüntüsüyle kontrol edildi)

## Notlar

### Yapılanlar ve kararlar
- Reverse-Z + sonsuz uzak düzlem (D-080), yakın düzlem gerçek / x-y guard band kırpma (D-081),
  varying perspektif sözleşmesi (D-082), ekran uzayı arka yüz ayıklama (D-083).
- **Raster hatası bulundu ve düzeltildi (D-084):** derinlik testi quad üretilirken, yazımı ise batch
  boşaltılınca yapılıyordu; aynı batch'teki örtüşen üçgenler eski derinliği görüyordu (küpün arka
  yüzleri 8×8 bloklar halinde sızıyordu). Artık early-Z yazımı; discard'lı shader'lar için geç yazım modu.
  Test: `gfx3d_backface_and_depth`, `gfx3d_alpha_test_late_depth`.
- Guard band, raster'ın i32 kenar fonksiyonu ara toplamlarının taşma analizinden türetildi
  (640×360 → 768 px, 1280×720 → 320 px). 1080p için raster değişikliği önerisi: D-085.
- Rasterizer kapsanmayan lane'lerde de varying'leri düzlem denkleminden hesaplıyor → M8_B (türev/mipmap) için hazır.

### Benchmark (i7-10750H, Chrome 156, 640×360, başlangıç görüşü: 1003 nesne, 132 culled, ~6000 üçgen çizilen)
`engine_frame` doğrudan çağrılarak, 30 kare ısınma + 60 kare medyanı (`globalThis.matoki.exports`):

| Thread | Matoki JIT (ms/kare) | C referans (ms/kare) |
|---:|---:|---:|
| 1 | 7.22 | 11.64 |
| 4 | 6.36 | 6.92 |
| 8 | 3.95 | 7.01 |

Aşama dağılımı (JIT, profiler ortalaması):

| Aşama | 1T (ms) | 8T (ms) |
|---|---:|---:|
| g3d_vertex (dönüşüm + outcode + izdüşüm) | 1.25 | 1.22 |
| g3d_tri (sınıflandırma + kırpma + ayıklama) | 0.36 | 0.35 |
| setup | 0.51 | 0.48 |
| bin | 0.22 | 0.20 |
| raster | 1.94 | 0.29 |
| shade (`lit3d.mtk`) | 2.19 | 0.33 |

### Darboğaz ve sonraki adımlar
- 8T'de raster+shade 4.1 → 0.6 ms; seri kısım (vertex + tri + setup + bin ≈ 2.25 ms) baskın (Amdahl).
- Vertex aşaması ~21k köşe için 1.25 ms (~60 ns/köşe): köşe başına by-value `Mat4` geçen
  `mat4_mul_vec4` çağrısı, 80 B `ClipVert` + 80 B `RasterVert` yazımı, nesne başına `mat4_inverse`.
  Öneri: SoA SIMD vertex dönüşümü + nesne düzeyinde "tamamen içeride" testiyle outcode atlama;
  ızgara gibi çok sayıda küçük nesne için `job_parallel_for` ile paralel vertex aşaması. (Ölçülmedi — tahmin yazılmadı.)
- 4T sayıları gürültülü (ana thread senkron döngüde çalışırken worker uyanma gecikmesi); kesin
  ölçeklenme tablosu M8 sonraki işlerinde alınacak.
- Uzak zeminde moiré belirgin → M8_B (türevler + mipmap).
