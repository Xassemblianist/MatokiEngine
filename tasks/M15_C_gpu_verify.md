# M15_C — WebGPU demosunu gerçek GPU'da doğrula ve düzelt

Durum: DONE

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
- [x] `make check` ve `make gpucheck` hatasız
- [x] SwiftShader ve Vulkan'da piksel denetimi geçer; ekran görüntüsü `build/webgpu_shot.png`
- [x] Hata halkası boş (bir kare boyunca hiçbir GPU hatası yok)

## Notlar
- **Kök neden analizi**:
  - `src/gpu/gpu_test.c` içindeki 15 GPU kaynağının (640×360 `depth24plus` derinlik dokusu, 64×64 `rgba8unorm` dama dokusu, vertex/index/uniform tamponları, WGSL shader modülleri, boru hatları ve bind group'lar) tamamı hem Vulkan (RTX 2060) hem de SwiftShader üzerinde sıfır hatayla oluşturulmaktadır.
  - Başsız Chrome (`--headless=new`, Linux Ozone headless platformu) altında `canvas.getContext("webgpu")` → `context.getCurrentTexture()` çağrıldığında Chromium'un `WebGPUSwapBufferProvider` katmanı yazılım birleştiriciyle paylaşmak üzere dış bellek (`VkExportMemoryAllocateInfo` / DMA-BUF `ExternalVkImageBacking`) tahsis etmeye çalışır. Başsız yüzeyde DRM/GBM cihazı olmadığından Dawn/Vulkan tarafında `vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY (VulkanError.cpp:106)` ile cihaz kaybedilir (`device.lost`), SwiftShader'da ise dış bellek takas zinciri desteklenmediği için `Device was destroyed.` hatası oluşur.
  - Ayrıca ilk yüklemede `web/gpu_test.html` içinde `WebAssembly.Memory` döngüsü `tryShared = false` ile başlayıp `--shared-memory` derlemesinde gereksiz bir 256 MB bellek tahsisi/istisnası üretiyordu (`[true, false]` sırasına alındı).
- **Çözüm ve piksel geri okuma (`web/gpu.js`, `src/gpu/rhi.h`, `src/gpu/gpu_test.c`, `web/gpu_test.html`)**:
  - `web/gpu.js` içinde varsayılan yüzey hedefi (`color_texture == 0`) için doğrudan GPU üzerinde `surfaceTexture` (`640×360`, `rgba8unorm`, `RENDER_ATTACHMENT | COPY_SRC | TEXTURE_BINDING`) ve 256-bayt satır hizalı `readbackBuffer` (`COPY_DST | MAP_READ`) oluşturuldu.
  - `RHI_CMD_PRESENT` aşamasında bekleyen bir okuma yoksa (`!readbackInFlight`) aynı `GPUCommandEncoder` içine `copyTextureToBuffer` eklenip `queue.submit` sonrasında `readbackBuffer.mapAsync(GPUMapMode.READ)` ile pikseller geri okunur; wasm tarafındaki `engine_gpu_test_readback_buf()` tamponuna yazılıp `engine_gpu_test_analyze_readback()` tetiklenir ve tuvale `putImageData` ile basılır.
  - Bölge sınıflandırması ve doğrulama mantığı tamamen C tarafında (`src/gpu/gpu_test.c`, `GpuTestSummary`) gerçekleştirilir: arka plan (`~(18, 20, 31)`), sol bölgedeki dönen renkli üçgen (`x < 260`, kırmızı/yeşil/mavi köşe bölgeleri) ve sağ bölgedeki yönlü ışıklı 3D küp (`x >= 260`, turuncu ve mavi dama texelleri) ile hata halkası (`gpu_errors == 0`) denetlenir.
- **Otomasyon (`tools/host/serve_coi.py`, `tools/host/webgpu_check.mjs`, `Makefile`)**:
  - `tools/host/serve_coi.py`: COOP/COEP (`same-origin` / `require-corp`) ve `no-store` başlıklı, `/web/*` ve `/build/*` yollarını da çözen HTTP sunucusu eklendi.
  - `tools/host/webgpu_check.mjs`: Varsayılan doğrulama ifadesiyle hem `gpu_test.html` piksel/hata halkası özetini hem de M15_A'nın ürettiği 10 WGSL shader'ının (`build/wgsl/*.wgsl`) GPU `createShaderModule` + `getCompilationInfo()` derlemesini denetler; geçici Chrome profil dizinini temizler ve başarısızlıkta `1` koduyla çıkar.
  - `Makefile`: `make gpucheck` hedefi sunucuyu 8095 portunda başlatıp SwiftShader'da (`make gpucheck GPU=1` ile gerçek GPU/Vulkan'da) doğrulamayı koşar.
- **Gözlenen sonuçlar (640×360 = 230.400 piksel)**:
  - **SwiftShader (`make gpucheck`)**: `ok: true`, `FPS: 60.0`, `readbacks: 238`, hata halkası `{write: 0, read: 0, dropped: 0}`, `bgPixels: 168253`, `triPixels: 7379` (`R: 1385, G: 1517, B: 1414`), `cubePixels: 54768` (`warm: 27359, cool: 27409`), `wgsl: 10/10 derlendi (0 hata)`.
  - **NVIDIA RTX 2060 Vulkan (`make gpucheck GPU=1`)**: `ok: true`, `FPS: 60.0`, `readbacks: 238`, hata halkası `{write: 0, read: 0, dropped: 0}`, `bgPixels: 166491`, `triPixels: 7379` (`R: 1386, G: 1520, B: 1412`), `cubePixels: 56530` (`warm: 28053, cool: 28040`), `wgsl: 10/10 derlendi (0 hata)`.

