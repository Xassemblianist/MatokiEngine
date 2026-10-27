DURUM: DONE
HAT: 3D oyun (Claude, 2026-10-27)

# M10_A — "Ocak": motorun 3D parçalarını birlikte kullanan küçük oyun

Vadide yüzen sütun adacıklara dağılmış 12 kıvılcımı topla (en fazla 3 taşınır), meşalen (30 s) sönmeden ortadaki
ocağa taşı, güneş batmadan (210 s) ateşi yak. Tasarım özgün; konusu projenin kendisi: ateşi icat etmek.

## Dosyalar
- `src/app/r3d.h/.c` (yeni): ortak 3D çizim katmanı — gün ışığı modeli, gölge + ana geçiş, PBR/Lambert/emissive çizim (D-101)
- `shaders/unlit3d.mtk` + `ref_shade_unlit3d` (kendinden ışıklı nesneler)
- `src/app/scene3d.c` r3d'ye taşındı (davranış aynı)
- `src/app/scene_ocak.h/.c` (yeni), `src/app/main.c` (7. sahne "7:Ocak", `engine_ocak_debug/yaw` test export'ları)

## Kullanılan motor parçaları
phys (`phys_move_and_slide` kapsül, `phys_world_raycast` kamera çarpışması), scene/ECS (kıvılcımlar Transform + Spark
entity'leri, sorgular), gfx3d + r3d (PBR, güneş gölgesi, gün ışığı, emissive), audio/synth (6 efekt + özgün La minör akşam teması).

## Kabul
- [x] `make check` geçer
- [x] Oynanabilirlik botu (tarayıcıda, JS'ten girdi tamponuna tuş basarak): 4 seferde 12 kıvılcım toplanıp ocağa
      taşındı, oyun kazanıldı (oyun süresi ~50–58 s; gün sınırı 210 s). Tüm sütun geçişleri zıplanabilir doğrulandı.
- [x] Kamera çarpışması: başlangıçta kameranın sütun içinde kalması hatası bulundu → hedef→göz ışını ile öne çekme
- [x] Kazanma ekranı ve büyüyen ocak ateşi görsel olarak doğrulandı

## Ölçüm (i7-10750H, Chrome, 640×360, başlangıç görüşü, 16 ms aralıklı medyan; 4 Argon ajanı aynı anda CPU kullanırken)
| 1 worker | 8 worker |
|---:|---:|
| 9.17 ms | 5.44 ms |

## Ertelenenler
- Ses kulakla dinlenmedi. Gerçek klavye/fare ile insan oynanışı yapılmadı (bot girdi tamponunu kullandı).
- Matoki Sans (M10_D) gelince HUD Türkçe karakterli olacak; retro modu (M10_B) seçeneği; iskelet animasyonlu karakter (M10_C).
