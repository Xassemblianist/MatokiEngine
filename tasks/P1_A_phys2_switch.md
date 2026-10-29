# P1_A — phys → phys2 geçişi

Durum: TODO

## Amaç
P1_0 ile `src/phys2/` geldi (92 test geçiyor); eski `src/phys/` hâlâ kullanılıyor (sim, scene_ocak, scene_kuyu, test_phys,
test_sim, Makefile). Tüm kullanıcıları phys2'ye taşı, `src/phys/` dizinini sil, `src/phys2` → `src/phys` olarak yeniden
adlandır (include yolları, Makefile). Belirlenimcilik hash testleri (make detcheck) geçmeli; davranış değişirse beklenen
hash'leri güncelle ve Notlar'a gerekçeyi yaz.

## Dosyalar
`src/phys/*`, `src/phys2/*`, `src/sim/*`, `src/app/scene_ocak.c`, `src/app/scene_kuyu.c`, `tests/test_phys*.c`, `tests/test_sim.c`, `Makefile`, `docs/ARCHITECTURE.md`

## Kabul kriterleri
- [ ] `src/phys2` kalmadı, tek fizik modülü `src/phys`
- [ ] Ocak/Kuyu sahneleri tarayıcıda çalışıyor (webgpu_check.mjs ekran görüntüsü)
- [ ] make check + make detcheck temiz
