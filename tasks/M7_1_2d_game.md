DURUM: DONE
HAT: 2D temel (tek ajan, 2026-10-27)

# M7_1..M7_3 — 2D motor temelleri, ses ve "Matoki Koşusu" platform oyunu

## Dosyalar
- M7_1: `src/gfx2d/tilemap.h/.c` (karo haritası, ASCII yükleme, çizim, AABB çarpışma), `camera2d.h/.c`
  (ölü bölge, üstel yumuşatma, sınır, piksel hizalama, paralaks), `anim.h/.c`; `tests/test_gfx2d.c`
- M7_2: `src/audio/synth.h/.c`, `src/audio/audio.h/.c`, `web/audio.js`, `web/audio_worklet.js`,
  `web/platform.js` (ses kurulumu çağrısı), `src/app/main.c` (`audio_pump`); `tests/test_audio.c`
- M7_3: `src/app/plat_art.h/.c` (prosedürel pixel-art), `src/app/scene_platformer.h/.c`, `src/app/main.c` (6. sahne)

## Kabul kriterleri
- [x] `make check` uyarısız, 79/79 test
- [x] Çarpışma: zemin/duvar/tavan konumları tam, 20 karo/kare hızda tünelleme yok, tek yönlü platform
      (alttan geç / üstüne kon / aşağı+zıpla ile in), 20 rastgele harita × 10k+ hareket fuzz → 0 ihlal
- [x] Kamera: ölü bölge, sınır kırpma, 60×(1/60) ile 30×(1/30) yumuşatma aynı noktaya (<0.01 px), ters dönüşüm
- [x] Synth: 4 dalga türünde frekans (441 Hz → 878..886 sıfır geçişi), attack zarfı, 2 oktav kayma,
      24 ses tam güçte [-1,1] ve NaN yok, desen ayrıştırıcı, müzik deterministik
- [x] Halka tampon: tüketici simülasyonuyla 4+ kez sarılan akış doğrudan render ile örnek-örnek aynı
- [x] Tarayıcı: ses 48 kHz'de çalışıyor (gerçek tıklama ile), worklet ~1.6 s'de 78.848 kare tüketti,
      C 79.360 kare yazdı, başlangıç sonrası yeni underrun yok, müzik sinyali tepe 0.11
- [x] Oyun: koşma, zıplama, basamaklara çıkma, altın toplama ekran görüntüsüyle doğrulandı;
      ~3600 karelik rastgele girdi fuzz'unda panic yok

## Oyun
128×20 karo seviye (ASCII, `scene_platformer.c`), 33 altın, 8 slime, dikenler, 5 çukur, tek yönlü
platformlar, bayrak hedefi. Fizik 1/120 s sabit adım: coyote time 90 ms, zıplama tamponu 120 ms,
değişken zıplama yüksekliği (bırakınca kes), tepe noktasında hafif süzülme, düşmana basınca sekme
(basılı tutarsan daha yüksek), hasar sonrası geri tepme + 1.6 s dokunulmazlık. Kamera ölü bölge +
yumuşatma + bakış yönüne öne kayma. Ses: zıplama, altın (iki ton), ezme, hasar, iniş, zafer + 3 parçalı
müzik (kare lead, üçgen bas, gürültü hi-hat).

## Ölçümler
- Rastgele girdi fuzz'u: ~3600 kare / 4.5 s = ~1.25 ms/kare (engine_frame + canvas kopyası, 9T, JIT, i7-10750H, Chrome 156).
  Ayrıntılı aşama profili alınmadı (sahne hafif; darboğaz yok).

## Notlar / ertelenenler
- Batch2D doku değişiminde tüm raster'ı boşaltıyor → tek atlas kuralı (D-093, Ö-6).
- Ses render'ı ana thread'de (Ö-7). Stereo/pan, efekt zinciri (reverb/filtre) yok.
- Seviye editörü, birden çok seviye, kayıt/skor tablosu yok.
- Gizli bölmede (pane hidden) rAF durduğu için tarayıcı ölçümleri `engine_frame` doğrudan çağrılarak alındı.
