DURUM: DONE
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
- [x] `make check` hatasız, uyarısız (ASan/UBSan altında)
- [x] Her çakışma çifti için en az 3 analitik test (temas yok / değiyor / iç içe), normal yönü ve derinlik doğrulanır
- [x] Işın testleri: kaçırma, teğet, içeriden başlayan ışın, t sıralaması; raycast_mesh BVH'li ve BVH'siz
      kaba kuvvet sonucu 10.000 rastgele ışında birebir aynı (aynı t ve üçgen)
- [x] Simetri/özellik testleri: A-B ve B-A normalleri ters, derinlikler eşit; rastgele 100.000 çift fuzz'da NaN yok
- [x] Dünya: düzleme düşen küre durur ve ~derinlik 0 ile yerde kalır (1000 adım sonra kayma < 1e-3);
      eğime konan kapsül eğim sınırı altında kaymaz, üstünde kayar; duvara koşan karakter duvarı geçmez (tünelleme yok, hız 50 m/s)
- [x] Benchmark (host, `-O1` yeterli, test içinde printf ile): 100k üçgenlik mesh'e (gfx3d `mesh_sphere`) 100k ışın, BVH kurma süresi ve ışın/s; sonuç Notlar'a

## Yasaklar
- `docs/*`, `Makefile`, başka modüller, `src/app/*` değişmez. Mimari öneri gerekiyorsa bu dosyanın Notlar'ına yaz.
- Ölçmeden "hızlı" yazma.

## Notlar

### Yapılanlar ve tasarım seçimleri
1. **Şekiller ve en yakın nokta yardımcıları (`src/phys/shapes.h/.c`)**:
   - `Sphere`, `Aabb`, `Capsule`, `Ray`, `Plane`, `Obb` ve etiketli `PhysShape` birliği tanımlandı.
   - Nokta-AABB, nokta-OBB, nokta-doğru parçası, doğru parçası-doğru parçası (paralel parçalarda örtüşen aralığın orta noktasını seçen kararlı yol), nokta-üçgen (Voronoi bölgeleri) ve doğru parçası-üçgen en yakın nokta algoritmaları sıfırdan yazıldı.
2. **Analitik çakışma ve simetri (`src/phys/collide.h/.c`)**:
   - Küre-küre, küre-AABB, küre-kapsül, kapsül-kapsül, AABB-AABB, kapsül-AABB, küre/kapsül/AABB-düzlem, küre-OBB ve küre/kapsül-üçgen çiftleri gerçeklendi.
   - Konvansiyon: `Contact.normal` her zaman **B'den A'ya** doğrudur (`pos_A += normal * depth` çakışmayı çözer). Tam değme anında (`depth == 0`) `hit = TRUE` döner.
   - Simetrik çiftlerde (`sphere_sphere`, `capsule_capsule`, `aabb_aabb`) kanonik sıralama kullanılarak `collide(A, B)` ve `collide(B, A)` çağrılarının kayan nokta yuvarlama farkı olmaksızın birebir eşit `depth` ve tam ters (`-normal`) üretmesi garanti altına alındı. 100.000 rastgele çift fuzz testinde 0 NaN ve 0 simetri hatası doğrulandı.
3. **Işın izleme ve statik BVH (`src/phys/raycast.h/.c`, `src/phys/bvh.h/.c`)**:
   - Küre, AABB (slab), kapsül (silindir gövde + uç yarım-küre kapakları), düzlem ve üçgen (iki yüzlü Möller–Trumbore) ışın kesişimleri yazıldı. Şeklin içinden başlayan ışınlarda yüzeyden ilk pozitif sınır kesişimi (`t >= 0`) döner.
   - `bvh_build`: `Arena` üzerinde 8 bölmeli Yüzey Alanı Sezgiseli (8-bin SAH) + ortanca yedek bölmesi ile çalışır (`BVH_LEAF_MAX = 4`). Geçici kurulum dizileri `arena_mark`/`arena_pop_to` ile kurulum sonunda geri bırakılır.
   - `raycast_mesh`: önden-arkaya (near-first) yığınlı BVH gezintisi yapar; eşit `t` durumunda küçük `tri_index` önceliği ve düğüm kutularında `1e-5f` sınır payı sayesinde 10.000 rastgele ışında kaba kuvvet (`raycast_mesh_brute`) ile bit-bit aynı `(t, tri_index)` sonucunu verir.
4. **Fizik dünyası ve karakter kontrolcüsü (`src/phys/world.h/.c`)**:
   - Sabit `1/120 s` adımlı yarı-örtük Euler, dinamik küre/kapsül gövdeler, statik şekiller ve BVH destekli statik `Mesh` çarpışmaları eklendi.
   - Düşük çarpma hızlarında mikro-zıplamayı kesen esneklik eşiği ve tam konum projeksiyonu ile düzleme düşen kürenin `depth ~ 0` ile yerde sabit kalması (1000 adım sonunda kayma `< 1e-3`) sağlandı.
   - `phys_move_and_slide` ve dinamik kapsül çözücüsünde `max_slope_cos` eğim sınırı uygulandı: eğim sınırı altındaki yüzeylerde dikey çıkarma ile yerçekimi kaynaklı kayma engellenir (`on_ground = TRUE`), eğim sınırı üstündeki dik yüzeylerde ise yüzey boyunca kayma gerçekleşir.
   - Yüksek hızlarda (50 m/s) tünellemeyi önlemek için yer değiştirme `0.4 * radius` adımlarına bölünerek (alt adımlama) çözülür.

### Benchmark sonuçları
- **Ortam**: Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz, Linux, `clang -std=c11 -O1 -g -fsanitize=address,undefined` (host test koşucusu, 3 koşunun medyanı)
- **Sahne**: `mesh_sphere(&a, 5.0f, 250, 200)` → **100.000 üçgen**, **100.000 rastgele ışın** (96.026 isabet)

| Ölçüm | Sonuç (Medyan) | Not |
|---|---:|---|
| 100k üçgen BVH kurulum süresi (`bvh_build`) | **92.81 ms** | 62.775 düğüm, 8-bin SAH, yaprak ≤ 4 üçgen |
| 100k ışın `raycast_mesh` (BVH açık) | **155.70 ms** (**0.64 M ışın/s**) | 96.026 / 100.000 isabet |
| Kaba kuvvet `raycast_mesh_brute` (100k üçgen) | **161 ışın/s** | BVH ile **~3900×** hızlanma |
