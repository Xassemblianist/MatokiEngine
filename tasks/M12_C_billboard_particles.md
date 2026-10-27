DURUM: TODO
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
- [ ] `make check` hatasız/uyarısız (ASan/UBSan)
- [ ] Billboard dörtgeni her kamera yönünde kameraya dik (normal · ileri ≈ -1), ekranda kare (en/boy ≈ 1, perspektifte)
- [ ] Kapasite taşması güvenli (sayaç), sıfır/negatif/NaN boyut ve konumlar atlanır
- [ ] Parçacık: aynı tohumla iki koşu bit-bit aynı; ömrü dolan sayısı analitik beklenenle tutarlı; havuz hiç taşmaz
- [ ] Benchmark (host): 5000 parçacık update + billboard mesh üretimi (ms) Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `src/raster/*`, `src/gfx3d/pipeline.*`, `shaders/*` değişmez. Öneriler Notlar'a.

## Notlar
