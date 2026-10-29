# V2 — Prosedürel ada: arazi + bitki örtüsü (GPU, yeni sahne "10:Ada")

Durum: TODO

## Amaç
Okyanusla çevrili büyük prosedürel ada (≥ 4×4 km):
- Arazi: GPU compute ile gürültü + hidrolik erozyon yükseklik haritası, CDLOD/clipmap LOD, üçlü düzlemsel (triplanar) malzeme karışımı (kum/çimen/kaya/kar, eğim + yükseklik).
- Bitki örtüsü: GPU'da yerleştirme (yoğunluk haritası), örneklenmiş ağaç/çimen, rüzgârda sallanma, uzakta impostor.
- Kıyıda okyanus ile birleşim (köpük şeridi, sığ su rengi). G1_B GPU sahnesi + V1 atmosfer kullanılır.
- Fizik: arazi yükseklik alanı çarpıştırıcısı, karakter yürüyebilir (phys karakter kontrolcüsü), tuşla serbest kamera/yürüme.

## Dosyalar
`src/gfx3d/terrain.*`, `src/gfx3d/foliage.*` (yeni), `src/app/scene_island.*` (yeni), `src/app/main.c`, `shaders/*`, `shaders/compute/*`, `src/gpu/*`, `web/gpu.js`, `src/phys/*` (yalnız yükseklik alanı şekli), `tests/*`

## Kabul kriterleri
- [ ] 1080p Vulkan ekran görüntüleri (kıyı, dağ, orman) build/ altında, gözle kontrol
- [ ] 60 fps hedef: GPU kare ms Notlar'da
- [ ] make check + make gpucheck temiz
