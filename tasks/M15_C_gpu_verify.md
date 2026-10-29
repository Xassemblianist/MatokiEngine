# M15_C — WebGPU demosunu gerçek GPU'da doğrula ve düzelt

Durum: TODO

## Bulgular (yönetici, 2026-10-29)
`node tools/host/webgpu_check.mjs <url> <ifade.js> [chrome bayrakları]` ile başsız Chrome'da denendi:
- RTX 2060 (`--enable-features=Vulkan --use-angle=vulkan`) ve SwiftShader (`--use-webgpu-adapter=swiftshader`)
  adaptörleri var; M15_A'nın 10 WGSL'i GPU derleyicisinden hatasız geçiyor.
- `gpu_test.html` 60 FPS raporluyor ama **tuval tamamen siyah** (iki adaptörde de). Vulkan'da hata halkasına
  `vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY`, SwiftShader'da `Device was destroyed` düşüyor.
  (GPU belleği boş: 47/6144 MiB → büyük ihtimalle yanlış hesaplanmış dev bir buffer/doku boyutu.)

## Amaç
1. Hatanın kök nedenini bul ve düzelt; üçgen + dokulu küp iki adaptörde de görünsün.
2. **Piksel doğrulaması**: demo, son kareyi `copyTextureToBuffer` + `mapAsync` ile geri okuyup wasm'a (ya da
   sayfaya) özet versin; `webgpu_check.mjs` ile beklenen renk bölgelerini (üçgen, küp, arka plan) denetle.
3. `make gpucheck` hedefi: sunucuyu kendisi açıp (python http.server + COOP/COEP başlıkları) demo + WGSL
   derleme denetimini SwiftShader'da koşar, başarısızlıkta sıfırdan farklı çıkar. (Gerçek GPU: isteğe bağlı bayrak.)

## Dosyalar
- `src/gpu/*`, `web/gpu.js`, `web/gpu_test.html`, `tools/host/webgpu_check.mjs`, `tools/host/serve_coi.py` (yeni),
  `Makefile` (yalnız `gpucheck` hedefi), `tasks/M15_C_gpu_verify.md`

## Kabul kriterleri
- [ ] `make check` ve `make gpucheck` hatasız
- [ ] SwiftShader ve Vulkan'da piksel denetimi geçer; ekran görüntüsü `build/webgpu_shot.png`
- [ ] Hata halkası boş (bir kare boyunca hiçbir GPU hatası yok)
