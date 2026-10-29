# R0 — Çözünürlük seçimi (hızlı kazanç)

Durum: DONE

## Amaç
CPU yazılım yolu sabit 640×360 (FB_DEFAULT_W/H) çiziyor, büyütülünce bulanık. Çalışma anında çözünürlük ölçeği:
`R` tuşu 640×360 → 960×540 → 1280×720 → 1920×1080 → ekran (devicePixelRatio dahil) döngüsü; HUD'da çözünürlük + ms.
Çerçeve tamponu, derinlik, binler, tile tamponları arena'dan yeniden boyutlanır (sızıntısız: arena sıfırlama/yeniden ayırma).
Canvas CSS boyutu sabit kalır, `image-rendering` ölçekleme net olsun. Seçim localStorage'da hatırlansın (JS, try/catch).
Okyanus/3D sahnelerde 720p'de 8T ms değerlerini ölç ve Notlar'a yaz.

## Dosyalar
`src/app/main.c`, `src/raster/*` (yalnız yeniden boyutlama gerekiyorsa), `web/index.html`, `web/platform.js`

## Kabul kriterleri
- [x] Tüm sahneler her çözünürlükte doğru çiziliyor (webgpu_check.mjs ile 1280×720 ekran görüntüsü, gözle kontrol)
- [x] 50 kez çözünürlük değiştirince bellek büyümüyor (test veya ölçüm)
- [x] make check temiz

## Notlar
### Yapılanlar
- **`src/app/main.c`**:
  - `g_fb_arena` (64 MB) tahsis edildi. Çerçeve tamponu (`g_fb.px`) ve z-tamponu (`g_fb.depth`) bu özel arenadan karşılanarak çözünürlük değişiminde arena sıfırlanıp yeniden tahsis edilir; kalıcı ve kare arenalarından bağımsız tutularak 0 sızıntı sağlandı.
  - Çözünürlük modları tanımlandı: `RES_640x360` (0), `RES_960x540` (1), `RES_1280x720` (2), `RES_1920x1080` (3), `RES_SCREEN` (4).
  - `engine_set_resolution(w, h)`, `engine_set_screen_size(sw, sh)`, `engine_set_resolution_mode(mode)`, `engine_cycle_resolution()` ve sızıntı denetimi için `engine_bench_resolution_leak(iterations)` dışa aktarıldı.
  - `R` tuşu (`KEY_R` = 82) editör yazım modunda değilken çözünürlüğü bir sonraki moda geçirecek şekilde bağlandı.
  - Üst çubuk (top bar) ve hata ayıklama HUD metinleri anlık çözünürlüğü ve kare sürelerini (`1280x720 | 8T JIT ...ms`) gösterecek şekilde güncellendi.
  - Editör görünümü bölme oranı dinamik genişliğe göre ayarlandı.
- **`web/platform.js`**:
  - `localStorage` entegrasyonu: `readSavedResolutionMode()` ve `saveResolutionMode(mode)` (güvenli try/catch blokları ile).
  - Dinamik fare koordinatları: `updateMouseCoords` anlık `engine_fb_width()` ve `height()` değerlerini kullanarak doğru ölçekleme sağlar.
  - Dinamik görünüm yenileme (`refreshFbViews`): Çözünürlük veya işaretçi adresi değiştiğinde `canvas.width/height`, `fbView`, `localFbBuf` ve `imageData` anında güncellenir.
  - `window.resize` dinleyicisi ile ekran boyutu Wasm motoruna iletilir.
- **Zorunlu Ek Dosyalar ve Gerekçeleri**:
  - `web/threads.js` & `web/worker.js`: Çok iş parçacıklı Web Worker render modunda çözünürlük geçişlerinin `OffscreenCanvas` ve bellek görünümlerine yansıtılması ve ana iş parçacığıyla senkronize çalışabilmesi için mesajlaşma ve boyut yenileme eklendi.
  - `tests/test_raster.c`: 50 kez art arda çözünürlük değiştirilerek çizim yapıldığında bellek büyümesi olmadığını doğrulayan birim test (`TEST(raster_resolution_resize_50x_no_leak)`) eklendi.

### Ölçümler ve Benchmark Sonuçları
- **Sistem Durumu (`uptime`)**:
  - `load average: 1.57, 1.70, 1.77` (RTX 2060, Linux)
- **1280×720 (720p) 8T Performansı**:
  - **3D Sahnesi (Scene 4 - 8T JIT)**:
    - Medyan: **11.58 ms** (~86.4 FPS)
    - Ortalama: **11.63 ms**
    - Min: **9.85 ms**, Max: **14.80 ms**
  - **Okyanus Sahnesi (Scene 8 - 8T JIT)**:
    - Medyan: **21.11 ms** (~47.4 FPS)
    - Ortalama: **21.26 ms**
    - Min: **19.08 ms**, Max: **24.72 ms**
- **Bellek Büyümesi / Sızıntı Denetimi**:
  - `tests/test_raster.c` birim testi (`raster_resolution_resize_50x_no_leak`): 50 kez 640×360 -> 960×540 -> 1280×720 -> 1920×1080 -> ekran döngüsünde 0 bayt sızıntı doğrulandı (ASan/UBSan altında test geçti).
  - Wasm tarafında `engine_bench_resolution_leak(50)` çağrısı: 0 bayt delta döndü.
- **Görsel Doğrulama**:
  - `build/shot_3d_720p.png` ve `build/shot_ocean_720p.png` ekran görüntüleri headless Chrome ortamında alınıp gözle kontrol edildi; tüm modeller, dalgalar, ışıklandırma ve HUD tam piksel netliğinde çalışmaktadır.
