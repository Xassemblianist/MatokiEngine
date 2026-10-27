# M11_A — Kuyu: retro birinci şahıs nişancı

Durum: DONE (Claude)

## Amaç
Motorun 3D parçalarıyla (raster + Matoki JIT + fizik + ses + retro sunum + Matoki Sans) özgün, Quake
tadında hızlı bir FPS. Kendi seviyesi, düşmanları, silahları ve müziği; başka bir oyunun kopyası değil.

## Dosyalar
- `shaders/dungeon3d.mtk`, `src/app/shaders.c/.h` (C referansı)
- `src/app/scene_kuyu.h/.c`, `src/app/main.c` (sahne 8, fare kilidi export'ları)
- `web/platform.js` (pointer lock), `docs/ARCHITECTURE.md` §2, `docs/DECISIONS.md` D-103

## Kabul kriterleri
- [x] `make check` hatasız + uyarısız (149/149)
- [x] Tarayıcıda sahne 8 açılıyor; JIT ve C referansı görsel olarak aynı
- [x] Bot: başlangıçtan ilk odaları temizleyip mühürü alıp çıkışa ulaşabiliyor (ışınlama yardımıyla bölüm bölüm)
- [x] Kare süresi ölçümü Notlar'a (1 ve 8 worker)

## Notlar
- **Oyun:** 32×13 karoluk elle çizilmiş seviye (6 oda, lav gölü, 7 kapı + 1 mühürlü kapı, ışık bacası çıkışı).
  Düşmanlar: Gölge (yakın dövüş, 50 can, 5.2 m/s) ve Tükürgen (6–13 m mesafe korur, yeşil tükürük atar, 75 can).
  Silahlar: Çakmak (hitscan, sınırsız), Saçmalı (8 saçma), Kor Atar (patlayan mermi, alan hasarı, kendine de vurur).
  Oyuncu: kapsül + basamak çıkma (0.55 m), coyote, zıplama tamponu; lav 40 can/s yakar. Müzik özgün (Mi frig, 70 bpm).
- **Çizim:** `dungeon3d.mtk` (D-103), 4×4 karoluk parça ağları (dünya-uzayı UV), parça/nesne başına en güçlü 4 ışık
  (24 sabit mangal/lav/çıkış ışığı + namlu alevi, mermi ve patlama ışıkları). 320×180 → `retro_present` 640×360.
  Silah modeli ayrı derinlik geçişinde.
- **Doğrulama (tarayıcı, bot):** Bot oda 1 → Gölge'yi öldürüp Saçmalı'yı aldı → lav taşlarından zıplayıp mührü aldı
  (Tükürgen öldü, can 86) → mühürlü kapıdan ışık bacasına ulaştı: durum = KAZANDI, 14.5 sn.
  JIT ve C referansı (retro kapalı, aynı kare) bayt bayt aynı; çizim JIT 0.80 ms, C ref 1.39 ms (aynı görüş).
- **Ölçüm** (bot rotası, ~407 kare, i7-10750H, Chrome, 320×180 iç çözünürlük; çizim = dünya + silah geçişi):
  | Worker | medyan | p95 | maks | retro sunum |
  |---|---:|---:|---:|---:|
  | 0 (tek thread) | 4.11 ms | 5.18 ms | 5.99 ms | 0.16 ms |
  | 8 | 2.08 ms | 2.69 ms | 3.40 ms | 0.16 ms |
- **Ertelenenler:** köşe yakalama (vertex snap, pipeline kancası gerekir), düşman yol bulma (şimdilik doğrudan + duvarda
  yana kayma), ikinci bölüm, kayıt/ayar menüsü. Fare kilidi gerçek fareyle elle denenmedi (bot klavye + debug bakışıyla).
