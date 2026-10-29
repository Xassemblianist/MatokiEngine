# R0 — Çözünürlük seçimi (hızlı kazanç)

Durum: TODO

## Amaç
CPU yazılım yolu sabit 640×360 (FB_DEFAULT_W/H) çiziyor, büyütülünce bulanık. Çalışma anında çözünürlük ölçeği:
`R` tuşu 640×360 → 960×540 → 1280×720 → 1920×1080 → ekran (devicePixelRatio dahil) döngüsü; HUD'da çözünürlük + ms.
Çerçeve tamponu, derinlik, binler, tile tamponları arena'dan yeniden boyutlanır (sızıntısız: arena sıfırlama/yeniden ayırma).
Canvas CSS boyutu sabit kalır, `image-rendering` ölçekleme net olsun. Seçim localStorage'da hatırlansın (JS, try/catch).
Okyanus/3D sahnelerde 720p'de 8T ms değerlerini ölç ve Notlar'a yaz.

## Dosyalar
`src/app/main.c`, `src/raster/*` (yalnız yeniden boyutlama gerekiyorsa), `web/index.html`, `web/platform.js`

## Kabul kriterleri
- [ ] Tüm sahneler her çözünürlükte doğru çiziliyor (webgpu_check.mjs ile 1280×720 ekran görüntüsü, gözle kontrol)
- [ ] 50 kez çözünürlük değiştirince bellek büyümüyor (test veya ölçüm)
- [ ] make check temiz
