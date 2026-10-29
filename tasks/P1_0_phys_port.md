# P1_0 — SamurayBalgami/Matoki fiziğini taşı (src/phys2)

Durum: IN_PROGRESS (agy)

## Amaç
Arkadaşın motorundaki fizik (`/home/ostriquetrum/matoki-wt/review/SamurayBalgami-Matoki/src/phys/*`, ~6.6k satır;
sahibi kod alınmasına izin verdi) bizim kurallara taşınır. **Yeni dizin `src/phys2/`** (M16_A `src/phys`'e dokunuyor;
çakışmasın — geçiş sonraki adım). İçerik: dünya + soft step çözücü, adalar/uyku, kısıt grafiği renklendirme + SIMD
temas çözücü, dinamik BVH geniş faz (kanonik çift sırası), şekiller + GJK/EPA + CCD + mesh, eklemler, XPBD yumuşak
cisimler, araç, karakter, DEM, okyanus, sorgular (raycast vb.).

## Uyarlama kuralları
- Tipler/arena/isimler bizim `core/` API'mize (`core/types.h`, `core/arena.h`, `core/math.h`; `phys2_` öneki).
  Onların `core/*` yardımcıları gerekirse `src/phys2/` içine özel (static/iç başlık) taşınır; bizim core'a dokunma.
- Onların iş sistemi yerine bizim `thread/job.h` (`job_parallel_for`). Belirlenimcilik: paralel yol seri yolla bit-bit.
- Bilinen hatalar düzeltilir: (1) uyandırılan adalar o adımda düzlem çifti almıyor (`world.c` broadphase: düzlem çiftleri
  uyandırmadan sonra eklenmeli), (2) karakter kontrolcüsü mesh ile çarpışmıyor ve tüm gövdeleri O(N) tarıyor (geniş
  faz sorgusu + mesh testi), (3) çift anahtarı 16 bit → gövde sınırı açıkça denetlenmeli ya da 32+32 bit anahtar.
- Onların testleri `tests/test_phys2_*.c` olarak bizim test çatımıza (`tests/test.h`) taşınır; bit-bit iddiaları
  (SIMD↔skaler, BVH↔SAP) testte kalır. Belirlenimcilik hash testi: aynı sahne 2 koşu + 1T↔8T aynı hash.
- Dosya başlarına kaynak notu: "SamurayBalgami/Matoki'den uyarlandı (izinle)".

## Dosyalar
- `src/phys2/*` (yeni), `tests/test_phys2_*.c` (yeni), `Makefile` (yalnız yeni kaynaklar gerekiyorsa),
  `docs/DECISIONS.md` (D-111: kaynak, uyarlama, düzeltilen hatalar), `tasks/P1_0_phys_port.md`

## Kabul kriterleri
- [ ] `make check` hatasız + uyarısız; taşınan testlerin hepsi geçer
- [ ] Düzeltilen 3 hata için ayrı regresyon testi
- [ ] Ölçüm (host -O2 ya da test zamanlaması): kutu yığını / 1000 gövde / kumaş adım süreleri, 1T ve 8T (makine yükünü not et)
