DURUM: TODO
HAT: Argon ajanı (paralel, kendi worktree'si)

# M9_B — 3D çarpışma ve basit fizik (sıfırdan)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, libc YOK (`src/` altında).
Tipler `core/types.h`, vektör/matris `core/math.h` (Vec3, Mat4, m_sqrt...), bellek `core/arena.h` (malloc yok).

## Dosyalar (YALNIZCA bunlar — başka dosyaya dokunma)
- `src/phys/shapes.h/.c` — Sphere, Aabb, Capsule, Ray, Plane, Obb (isteğe bağlı) tipleri
- `src/phys/collide.h/.c` — çakışma + temas (normal, derinlik, nokta) çiftleri:
  küre-küre, küre-AABB, küre-kapsül, kapsül-kapsül, AABB-AABB, küre/kapsül/AABB-düzlem
- `src/phys/raycast.h/.c` — ışın-küre/AABB/kapsül/düzlem/üçgen (Möller–Trumbore); `raycast_mesh`
  (`gfx3d/mesh.h` Mesh'i salt okunur kullanır) en yakın isabet, t + normal + üçgen indeksi
- `src/phys/bvh.h/.c` — üçgen mesh için statik BVH (SAH veya ortanca bölme), arena'dan; raycast_mesh bunu kullanır
- `src/phys/world.h/.c` — PhysWorld: dinamik gövdeler (küre/kapsül), statik şekiller + statik mesh,
  sabit adım (1/120 s) yarı-örtük Euler, yerçekimi, kayma ile temas çözümü (karakter kontrolcüsü için
  "move_and_slide": basamak yok, eğim sınırı var, yerde mi bilgisi), restitution/sürtünme basit
- `tests/test_phys.c`
- `tasks/M9_B_physics3d.md` (bu dosya: durum + Notlar)

## Kabul kriterleri
- [ ] `make check` hatasız, uyarısız (ASan/UBSan altında)
- [ ] Her çakışma çifti için en az 3 analitik test (temas yok / değiyor / iç içe), normal yönü ve derinlik doğrulanır
- [ ] Işın testleri: kaçırma, teğet, içeriden başlayan ışın, t sıralaması; raycast_mesh BVH'li ve BVH'siz
      kaba kuvvet sonucu 10.000 rastgele ışında birebir aynı (aynı t ve üçgen)
- [ ] Simetri/özellik testleri: A-B ve B-A normalleri ters, derinlikler eşit; rastgele 100.000 çift fuzz'da NaN yok
- [ ] Dünya: düzleme düşen küre durur ve ~derinlik 0 ile yerde kalır (1000 adım sonra kayma < 1e-3);
      eğime konan kapsül eğim sınırı altında kaymaz, üstünde kayar; duvara koşan karakter duvarı geçmez (tünelleme yok, hız 50 m/s)
- [ ] Benchmark (host, `-O1` yeterli, test içinde printf ile): 100k üçgenlik mesh'e (gfx3d `mesh_sphere`) 100k ışın, BVH kurma süresi ve ışın/s; sonuç Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, başka modüller, `src/app/*` değişmez. Mimari öneri gerekiyorsa bu dosyanın Notlar'ına yaz.
- Ölçmeden "hızlı" yazma.
