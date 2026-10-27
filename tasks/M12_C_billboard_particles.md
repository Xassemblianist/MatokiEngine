DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M12_C — 3D billboard + parçacık sistemi, sıfırdan

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turlardaki incelemelerde bozuk/düşmanca girdiler bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle. Ölçmeden "hızlandı" yazma.

## Hedef
Retro 3D oyunlarda parçacıklar/efektler (kıvılcım, duman, kan, alev) şu an tek tek küp mesh çizimi (her biri ayrı
`g3d_draw`). Genel bir modül:
1. **Billboard toplayıcı**: kameraya bakan dörtgenler (dünya konumu, boyut, dönüş, renk RGBA8, isteğe bağlı doku
   atlas karesi) tek bir dinamik `Mesh`'e (köşe rengiyle; `G3D_ATTR_COLOR`) yazılır → tek `g3d_draw`. Kamera
   sağ/yukarı vektörleri view matrisinden. Kapasite sabit (çağıran verir), taşarsa fazlası sayılıp atlanır.
2. **Parçacık sistemi**: sabit kapasiteli havuz (arena), yayıcılar (nokta/küre, hız aralığı, ömür, yerçekimi, sürtünme,
   boyut ve renk eğrisi başlangıç→bitiş), `particles_update(dt)`, `particles_emit_burst`, ölen parçacık O(1) silme
   (swap-remove). Deterministik (`core/rng`).
3. İsteğe bağlı sıralama: alfa harmanlı çizim için arkadan öne (kameraya uzaklık) — taban sayma/radix ile, malloc yok.
Shader olarak mevcut `vcolor` benzeri bir 3D varyant gerekiyorsa **yazma**, Notlar'a hangi varying'leri istediğini yaz
(shader'ları ben bağlayacağım); testler host'ta düz renkli test shader'ıyla yapılır (`tests/test_gfx3d.c`'deki gibi).

## Dosyalar (YALNIZCA bunlar)
- `src/gfx3d/billboard.h/.c`, `src/gfx3d/particles.h/.c`
- `tests/test_particles.c`
- `tasks/M12_C_billboard_particles.md`

## Kabul
- [x] `make check` hatasız/uyarısız (ASan/UBSan)
- [x] Billboard dörtgeni her kamera yönünde kameraya dik (normal · ileri ≈ -1), ekranda kare (en/boy ≈ 1, perspektifte)
- [x] Kapasite taşması güvenli (sayaç), sıfır/negatif/NaN boyut ve konumlar atlanır
- [x] Parçacık: aynı tohumla iki koşu bit-bit aynı; ömrü dolan sayısı analitik beklenenle tutarlı; havuz hiç taşmaz
- [x] Benchmark (host): 5000 parçacık update + billboard mesh üretimi (ms) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `src/raster/*`, `src/gfx3d/pipeline.*`, `shaders/*` değişmez. Öneriler Notlar'a.

## Notlar

### Yapılanlar
1. **3D Billboard Toplayıcı (`src/gfx3d/billboard.h`, `src/gfx3d/billboard.c`)**:
   - `billboard_init(b, arena, capacity)` ayırma yapmadan önce `arena_can_push_seq` ile `Mesh` (SoA köşe akışları + `color` + `indices`), `Billboard` dizisi ve radix sıralama tamponlarının tamamının verilen `Arena`'ya hizalamalarıyla sığıp sığmadığını denetler (`capacity == 0` veya `> BILLBOARD_MAX_CAPACITY` (65536) veya yetersiz arena durumunda `PANIC` tetiklemeden `FALSE` döner).
   - Her dörtgenin 6 üçgen indeksi (`0,1,2, 0,2,3` CCW ön yüz) `billboard_init` sırasında bir kez doldurulur; kare başında yalnız aktif köşeler güncellenir.
   - `billboard_extract_camera` view matrisinin üst 3×3 satırlarından kamera `right`, `up`, `forward` birim vektörlerini (Gram–Schmidt dikleştirme + bozuk/tekil matriste birim yedek ile) ve öteleme sütunundan kamera dünya konumu `eye = -R^T * t` değerini çıkarır. Dörtgen yüzey normali `normal = -cam_forward = cam_right × cam_up` olarak atanır (`normal · cam_forward = -1`).
   - `billboard_push`, `billboard_push_ex`, `billboard_push_atlas` ve `billboard_push_item`: sıfır/negatif/NaN/Inf boyutları, NaN/Inf/aşırı (`> 1e15`) konumları ve NaN/Inf dönüş/UV değerlerini `skipped_count` sayacını artırarak güvenli şekilde atlar; kapasite doluyken gelen geçerli dörtgenleri `overflow_count` sayacını artırarak atlar. Büyük sonlu dönüş açıları `m_sin`/`m_cos` (`reduce_pio2`) içinde `i32` taşması olmaması için `[-2π, 2π]` aralığına indirgenir.
   - `billboard_sort` / `billboard_sort_by_eye`: kamera göz konumuna olan karesel uzaklık `d²`'yi IEEE-754 bit terslemesiyle (`~bits`) 32-bit anahtara çevirir ve 4 geçişli (8-bit basamak, 256 kova) kararlı LSD radix sıralama ile **arkadan öne** (büyük uzaklıktan küçüğe) sıralar. Tek kovaya düşen basamak geçişleri otomatik atlanır; `malloc` kullanılmaz.
   - `billboard_build_mesh` / `billboard_end`: aktif dörtgenlerin 4 köşesini (`rot == 0` için trigonometrisiz hızlı yol ile) ve tam AABB/küre sınırlarını (`p ± (|rx| + |ry|)`) tek geçişte üretir. `billboard_draw` birim model matrisiyle tek `g3d_draw` çağrısı yapar.

2. **3D Parçacık Sistemi (`src/gfx3d/particles.h`, `src/gfx3d/particles.c`)**:
   - Sabit kapasiteli Arena havuzu (`particles_init`, `particles_reset`) ve deterministik `Rng` (`core/rng.h`).
   - `ParticleEmitter`: nokta (`EMIT_SHAPE_POINT`) ve küre (`EMIT_SHAPE_SPHERE`, Archimedes silindir projeksiyonu ile sabit 2 RNG çağrısında düzgün birim küre yönü) yayıcıları, kutusal hız aralığı (`vel_min..vel_max`) + radyal hız aralığı (`speed_min..speed_max`), ömür aralığı (`life_min..life_max`), yerçekimi (`gravity`), örtük Euler doğrusal sürtünme (`1 / (1 + drag * dt)`), başlangıç→bitiş boyut (`size_start -> size_end`) ve renk (`color_start -> color_end`) eğrileri, açısal hız ve isteğe bağlı doku atlası kare animasyonu.
   - Renk eğrisi `lerp_rgba8` içinde 2 kanalı paralel (SWAR: `0x00FF00FF` maskesiyle R+B ve G+A) sabit nokta aritmetiğiyle karıştırır.
   - `particles_update(dt)`: ömrü dolan (`age >= life`) parçacıkları **O(1) swap-remove** ile siler ve silinen sayıyı döner (`expired_last`, `expired_total`).
   - `particles_to_billboards` ve `particles_build_billboards`: aktif parçacıkları tek çağrıda `BillboardBatch`'e aktarıp (isteğe bağlı arkadan öne radix sıralamayla) dinamik `Mesh*` üretir.

3. **Testler (`tests/test_particles.c`)**:
   - 50 rastgele kamera yönü/konumu ve dönüş açısında `normal · ileri ≈ -1` ve perspektif izdüşümde ekran karesi (`en/boy ≈ 1`, hem analitik köşe izdüşümü hem `g3d_draw` + `Framebuffer` piksel kutusu üzerinde) doğrulandı.
   - Köşe rengi (`G3D_ATTR_COLOR`) ve doku atlası (`G3D_ATTR_UV`) doğrulandı.
   - Kapasite aşımı, küçük arena ve bozuk/düşmanca girdiler (`0`, negatif, `NaN`, `±Inf`, `U32_MAX`) test edildi.
   - Radix sıralamanın arkadan öne monotonluğu, eşit uzaklıkta kararlılığı ve `BLEND_ALPHA` ile yarı saydam üst üste binme rengi doğrulandı.
   - Aynı tohumla iki koşunun bit-bit (`memcmp == 0`) aynılığı, analitik ömür sönümü (3 ayrık ömür grubu + 2000 parçacık düzgün dağılım) ve kanarya korumalı havuz sınırı doğrulandı.

### Benchmark (5000 Aktif Parçacık: `particles_update` + `particles_build_billboards`)
- **Donanım / Ortam**: Intel Core i7-10750H @ 2.60 GHz (Linux), Debian Clang 19.1.7.
- **Yöntem**: 5000 aktif parçacık (20.000 köşe, 10.000 üçgen); 10 ısınma turu + 51 ölçüm turu medyanı (`plat_time_ms`).

| Aşama (5000 Parçacık → 20.000 Köşe) | Host (`-O1 -fsanitize=address,undefined`) | Host Release (`-O2`) |
|---|---:|---:|
| `particles_update(dt)` | **0.102 ms** | **0.045 ms** |
| `particles_build_billboards` (sırasız Mesh üretimi) | **0.539 ms** | **0.118 ms** |
| **Toplam (update + sırasız Mesh)** | **0.645 ms** | **0.167 ms** |
| `particles_build_billboards` (4 geçişli radix sıralı Mesh üretimi) | **0.671 ms** | **0.195 ms** |
| **Toplam (update + radix sıralı Mesh)** | **0.779 ms** | **0.248 ms** |

### Shader / Entegrasyon Notları
- **3D Parçacık Shader Varying Sözleşmesi**:
  - **Düz renkli (dokusuz) parçacıklar / kıvılcımlar**: Mevcut `shaders/vcolor.mtk` (`uniform tint: vec4; in v_color: vec4;`) doğrudan kullanılabilir! Materyalde `mat.attrs[0] = G3D_ATTR_COLOR; mat.attr_count = 1;` seçilmesi yeterlidir (`G3D_ATTR_COLOR` 4 bileşenli `vec4` varying üretir).
  - **Dokulu / atlaslı parçacıklar (duman, alev, kan)**: `sprite.mtk`'nın köşe rengi (`v_color`) alan bir varyantı (ör. `particle3d.mtk`) bağlanmak istenirse şu bildirim sırası önerilir:
    - Uniform'lar: `uniform tint: vec4; uniform tex: texture2d;` (isteğe bağlı sis için `uniform fog: vec4;`)
    - Varying'ler (`in` sırası `G3dMaterial.attrs` ile birebir):
      1. `in uv: vec2;` → `attrs[0] = G3D_ATTR_UV`
      2. `in v_color: vec4;` → `attrs[1] = G3D_ATTR_COLOR`
      3. (İsteğe bağlı mesafe sisi istenirse) `in depth: f32;` → `attrs[2] = G3D_ATTR_VIEW_DEPTH`
  - **Harmanlama ve Derinlik**: Opak/alfa-testli parçacıklarda `depth_test = TRUE, depth_write = TRUE`; yarı saydam (`BLEND_ALPHA` veya `BLEND_ADD`) parçacıklarda `depth_test = TRUE, depth_write = FALSE` ve `BLEND_ALPHA` için `particles_build_billboards(&ps, &bb, view, TRUE)` (arkadan öne radix sıralama açık) kullanılmalıdır.


### İnceleme (Claude)
- Main ile (M13_B paralel ön uç dahil) birleşik `make check` 163/163. `billboard_draw` için ertelenmiş g3d modunda
  mesh ömrü uyarısı başlığa eklendi. Sahne entegrasyonu (kıvılcım/duman) ayrı iş.
