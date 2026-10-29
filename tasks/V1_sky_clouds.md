# V1 — Fiziksel atmosfer + hacimsel bulutlar + gün döngüsü (GPU)

Durum: IN_PROGRESS (agy gemini-4-argon-high)

## Amaç
Okyanus sahnesinin gökyüzünü "AAA" seviyeye çıkar (WebGPU yolunda; CPU yolunda basit yedek kalır):
- Hillaire 2020 atmosferi: transmittance + multi-scattering + sky-view LUT'ları (compute), hava perspektifi (aerial perspective) 3B LUT.
- Hacimsel bulutlar: Perlin-Worley 3B gürültü dokuları (compute ile üretilir), kümülüs katmanı, ışık yürüyüşü + Beer-Powder,
  çeyrek çözünürlük + zamansal yeniden yansıtma (temporal reprojection) ile ucuz.
- Gün döngüsü: güneş/ay konumu, gün batımı renkleri, gece yıldız alanı; okyanus yansıması atmosferden beslenir.
- Tuşlar: zaman ilerletme, bulut örtüsü ±. HUD: atmosfer/bulut GPU ms.

## Dosyalar
`src/gpu/*`, `web/gpu.js`, `shaders/sky.mtk`, `shaders/compute/*` (yeni çekirdekler), `src/app/scene_ocean.*`, `src/gfx3d/atmos.*` (yeni), `tests/*`

## Kabul kriterleri
- [ ] Öğle, gün batımı, gece için 3 ekran görüntüsü (1080p, Vulkan) build/ altında, gözle kontrol edildi
- [ ] Atmosfer + bulut toplamı 1080p'de < 3 ms GPU (timestamp-query)
- [ ] make check + make gpucheck temiz
