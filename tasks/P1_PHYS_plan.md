# Fizik v2 — plan (M16_A merge edildikten sonra başlar)

Durum: TODO (yönetici sıralar)

Hedef: Unreal/Jolt sınıfı, **belirlenimci** (rollback, D-110), SIMD + iş parçacıklı fizik. ARCH_V2: durum bölgesinde,
işaretçisiz; çözücü sırası kanonik (paralellik sonucu değiştirmez). Referans fikirler: Box2D v3 "soft step",
Jolt (adalar, geniş faz), XPBD (Müller), Catto GDC sunumları.

**2026-10-29 güncelleme:** SamurayBalgami/Matoki sahibi kodunun alınmasına izin verdi (birlikte geliştirme). Fizik v2
sıfırdan yazılmaz: önce **P1_0** ile onların `src/phys/*` modülü (soft step, GJK/EPA, CCD, eklemler, XPBD, SIMD
renkli çözücü, dinamik BVH, araç, karakter, DEM, okyanus) bizim kurallara taşınır; P1_A..E bunun üstüne kurulur.
Kaynak DECISIONS'ta belirtilir.

| Görev | İçerik | Dosyalar | Model |
|---|---|---|---|
| P1_0 | SamurayBalgami fiziğini taşı: tipler/arena/isimler bizim kurallara, mevcut `src/phys` API'si (raycast, Kuyu/scene3d kullanımları) uyum katmanıyla korunur; testleri taşınır; bilinen hatalar düzeltilir (uyanan ada↔düzlem teması, karakter↔mesh + geniş faz, 16-bit çift anahtarı sınır denetimi); belirlenimcilik hash testi | `src/phys/*`, `tests/test_phys*`, `src/app/*` (yalnız API uyumu), DECISIONS | Argon high |
| P1_A | Rijit çözücü v2: soft step (alt adımlı, yumuşak kısıtlar), adalar + uyku, kısıt grafiği renklendirme, SIMD temas çözücü (skalerle bit-bit), iş parçacıklı renk grupları; kanonik çift sırası | `src/phys/world.*`, `solver*` (yeni), testler | Argon high |
| P1_B | Dar faz: GJK/EPA, kapsül/dışbükey gövde, mesh (BVH'li) her şekille, temas manifoldu azaltma, CCD (konservatif ilerleme) | `src/phys/collide.*`, `gjk.*` (yeni), `shapes.*`, testler | Argon high |
| P1_C | XPBD yumuşak cisimler: kumaş, jöle, ip; rijitle iki yönlü; öz-çarpışma; yırtılma | `src/phys/soft.*` (yeni), testler | Argon high |
| P1_D | Eklemler (menteşe, küresel, sabit, kayar, 6-DOF limitli + motor), ragdoll, raycast araç, karakter kontrolcüsü (mesh dahil, geniş fazlı) | `src/phys/joints.*`, `vehicle.*`, `character.*` (yeni) | Argon high (P1_A/B sonrası) |
| P1_E | Fizik benchmark + regresyon: yığın kuleleri, 10k kutu, kumaş, araç; belirlenimcilik hash'leri, 1T/8T | `tools/host/bench_phys.c`, `tests/test_phys_*` | Flash high |
| P1_F | GPU fizik (sonra): parçacık/akışkan (SPH/PBF) Matoki compute ile | F0_A + M15 sonrası | Argon high |
