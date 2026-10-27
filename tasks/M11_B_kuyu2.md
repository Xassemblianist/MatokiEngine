# M11_B — Kuyu: ikinci bölüm, yol bulma, Gözcü, köşe yakalama

Durum: DONE (Claude)

## Amaç
Kuyu'yu derinleştir: bölüm sistemi + 2. bölüm, düşmanlar duvarların etrafından dolaşsın,
yeni bir yaratık (uçan), retro köşe yakalama (vertex snap).

## Dosyalar
- `src/app/scene_kuyu.c/.h` (bölüm tablosu, seviye arenası, akış alanı, Gözcü, ortak `move_body`)
- `src/gfx3d/pipeline.h/.c` (`g3d_set_snap`), `tests/test_gfx3d.c` (`gfx3d_vertex_snap`)
- `docs/DECISIONS.md` D-104

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız (150/150)
- [x] Bot iki bölümü ölümsüzlük kapalı bitiriyor
- [x] Düşman duvar arkasındaki oyuncuya kapıdan dolaşıp ulaşıyor
- [x] Köşe yakalama birim testi + ölçüm

## Notlar
- **Bölümler:** `LevelDef` tablosu; seviye ağları/fizik/akış alanları 8 MB'lık seviye arenasında, her yüklemede
  `arena_reset`. Çıkışa varınca sonraki bölüm yüklenir (silah/cephane/can taşınır, +25 can), son bölümde kazanılır.
  2. bölüm "Derin Kuyu" 36×18: lav köprülü mağara, 4 Gözcü, yükseltilmiş yürüme yoluyla çevrili mühürlü çıkış
  kulübesi. Harita erişilebilirliği betikle denetlendi (çıkışa yalnız mühürle ulaşılıyor).
- **Yol bulma:** Oyuncu karosundan BFS akış alanı (yer: lav/duvar/mühürlü kapı engel, en fazla STEP_H çıkış;
  hava: yalnız duvar). Oyuncu karo değiştirince veya 0.5 s'de bir yeniden kurulur. Düşman en küçük adımlı komşu
  karonun merkezine yürür (köşe kesmesiz çapraz). Yerdekiler lava adım atmaz (`avoid_hazard`).
  Doğrulama: C odasında uyandırılan Gölgeler oyuncu duvar arkasına ışınlanınca (3,6) kapısından dolaşıp vurdu.
- **Gözcü:** Uçan göz (40 can, 4.6 m/s), oyuncu gözünün ~1 m üstünde süzülür, 5–10 m mesafe + yan kayma,
  0.13 s arayla 3'lü kızıl ok (7 hasar). Ölünce yere düşer. Kızıl iris saldırı öncesi parlar.
- **Basamak çıkma hatası düzeltildi:** yatay deneme hareketi en az kapsül yarıçapı kadar olmalı, yoksa alt yarım
  küre basamak kenarına oturur ve kenar normali zemin sayılmaz (1. bölümdeki 1122 merdiveni de çıkılamıyordu).
  Oyuncu ve yürüyen düşmanlar ortak `move_body` kullanır.
- **Köşe yakalama:** `G3dContext.snap` (D-104), `retro_snap_xy` ile; Kuyu'da varsayılan açık (G ile kapanır),
  silah modeline uygulanmaz. Maliyet ölçülemeyecek kadar küçük (aşağıda).
- **Ölçüm** (2. bölüm bot rotası, 514 kare, i7-10750H, Chrome, 320×180):
  | Worker | snap | medyan | p95 | maks |
  |---|---|---:|---:|---:|
  | 8 | açık | 2.73 ms | 3.54 ms | 6.60 ms |
  | 8 | kapalı | 2.77 ms | 3.60 ms | 5.87 ms |
  | 0 | açık | 8.10 ms | 10.11 ms | 11.24 ms |
  Bot tam oyun: 1. bölüm (2 öldürme, can 86) → 2. bölüm → kazandı, toplam 28.9 sn, 8/20 yaratık, can 65.
- **Ertelenenler:** akış alanı tüm düşmanlar için tek hedef (oyuncu); kanat çırpma sesi; kazanınca kamera ışık
  sütununun içinde kalıyor (ekran maviye boyanıyor — "ışığa çıktın" olarak bırakıldı).
