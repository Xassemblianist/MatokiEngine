# G1_B — GPU'da yaşayan sahne + gfx3d WebGPU arka ucu (H5)

Durum: DONE (agy gemini-4-argon-high)

## Amaç
ARCH_V2 H5: "GPU'da yaşayan sahne CPU'yu boşaltır". G1_A'nın altyapısını genelleştir:
- Örnek (instance) tamponu GPU'da kalıcı; CPU yalnız değişen dönüşümleri yükler (dirty list).
- Compute ile frustum + Hi-Z oklüzyon kırpma → dolaylı çizim argümanları (multi-draw-indirect varsa kullan, yoksa döngü).
- Kümelenmiş ileri aydınlatma (clustered forward), PBR (pbr3d.mtk WGSL), gölge haritası (CSM 3 kademe).
- "5:3D" sahnesi ve Kuyu/Ocak WebGPU varsa GPU'da çizilir; yoksa CPU yolu.
- 100k örnek benchmark sahnesi (tuşla): CPU gönderim süresi ölç.

## Dosyalar
`src/gpu/*`, `web/gpu.js`, `src/gfx3d/*`, `src/app/scene3d.*`, `src/app/r3d.*`, `src/app/scene_kuyu.c`, `src/app/scene_ocak.c`, `src/app/main.c`, `shaders/*.mtk`, `tests/*`

## Kabul kriterleri
- [x] 100k örnek: CPU gönderimi < 0.5 ms, GPU kare süresi Notlar'da (H5 sonucu ARCH_V2'ye işlenir)
- [x] 3D/Kuyu/Ocak 1080p ekran görüntüleri GPU yolunda doğru
- [x] make check + make gpucheck temiz

## Notlar

### Yapılanlar
- **RHI Dolaylı Çizim (`src/gpu/rhi.h`, `src/gpu/rhi.c`, `web/gpu.js`):**
  - `RHI_BUF_INDIRECT = 1u << 6` (`GPUBufferUsage.INDIRECT = 0x0100`) ve `RHI_CMD_DRAW_INDEXED_INDIRECT = 25` komutu eklendi.
  - `web/gpu.js` tarafında adaptör destekliyorsa `'indirect-first-instance'` özelliği isteniyor; `draw_count` adet 20 baytlık `DrawIndexedIndirectArgs` (`index_count`, `instance_count`, `first_index`, `base_vertex`, `first_instance`) döngüyle `passEncoder.drawIndexedIndirect(buf, offset + i * 20)` üzerinden yürütülüyor.
- **GPU'da Yaşayan Sahne ve WebGPU 3D Arka Ucu (`src/gpu/gpu_scene.h`, `src/gpu/gpu_scene.c`):**
  - Kalıcı örnek tamponu (`102,400` kapasiteli `GpuInstance`, örnek başına 64 bayt: 3×4 dünya matrisi + paketli materyal/bayraklar + çevre küresi yarıçapı) GPU storage buffer'da yaşar.
  - **Kirli Aralık Birleştirme (Dirty List):** `gpu_scene_set_instance` bit haritasında kirli örnekleri işaretler; `gpu_scene_flush_dirty` ardışık kirli örnekleri tek `rhi_write_buffer` çağrısında birleştirir.
  - **WGSL Compute Kırpma + Hi-Z Oklüzyon + Dolaylı Çizim (`cs_init_100k`, `cs_reset_hiz`, `cs_hiz_down_a/b`, `cs_cull_inst`):**
    - 100k modunda `cs_init_100k` ilk karede 100,000 küp örneğini (500×200 ızgara) doğrudan GPU üzerinde oluşturur.
    - Önceki karenin yakın oklüder derinliği 8 mip seviyeli (256×128 → 2×1) muhafazakâr min-depth Hi-Z piramidine indirgenir.
    - `cs_cull_inst` her aktif örnek için 6 düzlemli frustum kırpma + Hi-Z oklüzyon testi + 3 kademeli CSM gölge kırpması yapar; görünür örnekleri mesh kovalarına (`atomicAdd(&indirect_args[bucket].instance_count, 1u)`) yazar.
  - **Kümelenmiş İleri Aydınlatma (`cs_cluster_lt`):** Görüş frustumu `16×8×8` (1,024) logaritmik derinlikli 3D kümeye bölünür; nokta ışıklar compute aşamasında kümelere atanır.
  - **Çoklu Gölgelendirme + 3 Kademeli CSM (`vs_shadow`, `fs_shadow`, `vs_scene`, `fs_scene`):**
    - `3072×1024` gölge atlasında 3 CSM kademesi (yakın/orta/uzak) ve 2×2 bilineer PCF.
    - `pbr3d.mtk` ile uyumlu Cook-Torrance GGX PBR + ACES ton eşleme, `lit3d.mtk` uyumlu Blinn-Phong, `dungeon3d.mtk` uyumlu 8:Kuyu zindan aydınlatması (Bayer 4×4 dither + tarama çizgisi + ters-Z viewmodel derinlik koruma) ve yumuşak parçacık gölgelendirmesi.
- **Sahne Entegrasyonu (`src/app/r3d.c`, `src/app/scene3d.c`, `src/app/scene_ocak.c`, `src/app/scene_kuyu.c`, `src/app/main.c`):**
  - `5:3D`, `7:Ocak` ve `8:Kuyu` sahneleri `engine_gpu_available()` aktifken WebGPU arka ucunu kullanır; WebGPU yoksa mevcut CPU rasterizer yoluna düşer.
  - `5:3D` sahnesinde `'H'` tuşu (veya `engine_gpu_scene_set_100k`) 100k örnek benchmark modunu açıp kapatır.
- **Ek Dosya Notu:** Kabul kriteri gereği H5 doğrulama sonucu `docs/ARCH_V2.md` tablosuna işlendi.

### Ölçümler (H5 Doğrulaması — 1920×1080, NVIDIA GeForce RTX 2060, Headless Chrome + Vulkan)
- Sistem yükü (`uptime`): `load average: 1.95, 1.56, 2.02` (paylaşımlı makine).
- **5:3D — 100k Örnek Benchmark (`activeCount = 100,004`, kare başına `18` hareketli/kirli örnek → `2` birleşik `writeBuffer` çağrısı, `1,152` bayt):**
  - **CPU gönderim süresi (`cpu_submit_ms`):** medyan **`0.020 ms`** (`20 µs`), minimum **`0.010 ms`** (`10 µs`), p95 **`0.036 ms`** — `< 0.5 ms` hedefinin **25× altında**.
  - **GPU kare süresi (`timestamp-query`):** medyan **`2.174 ms`**, minimum **`2.144 ms`** (compute kırpma + 3×CSM gölge + 4×MSAA PBR + Hi-Z dahil).
- **1080p Sahne Ekran Görüntüleri ve Kare Süreleri (`build/` altında doğrulandı):**
  - `5:3D` (`build/shot_3d_gpu.png` ve `build/shot_3d_100k_gpu.png`): normal modda CPU gönderimi `0.330 ms` (ilk yükleme sonrası `0.020 ms`), GPU kare süresi `0.93 ms` – `2.47 ms`.
  - `7:Ocak` (`build/shot_ocak_gpu.png`): CPU gönderimi `0.040 ms`, GPU kare süresi `1.392 ms`.
  - `8:Kuyu` (`build/shot_kuyu_gpu.png`): CPU gönderimi `0.110 ms`, GPU kare süresi `1.955 ms`.

### Ertelenenler
- WebGPU'da `multiDrawIndexedIndirect` standart bir özellik olmadığı için çizim kovaları (24 kova) JS RHI yürütücüsünde tek komuttan (`RHI_CMD_DRAW_INDEXED_INDIRECT`) döngüyle `passEncoder.drawIndexedIndirect` çağırarak yürütülüyor; `'indirect-first-instance'` özelliği mevcutsa doğrudan kova ofseti kullanılıyor.
