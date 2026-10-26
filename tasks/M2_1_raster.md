DURUM: DONE
HAT: A (raster)

# M2 — Referans rasterizer + 2D batch + sprite demo (BÜYÜK)

## Amaç
Skaler ama DOĞRU ve hızlı bir yazılım rasterizer. Shader'ları `ShadeFn` ABI'si üzerinden çağırır
(şimdilik C ile yazılmış referans shader'lar; M4'te aynı yere JIT'lenmiş Matoki girecek).

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*` (B'nin math/fixed/rng/profiler'ı dahil), `src/raster/shade_abi.h`, `src/plat/platform.h`, `tests/test.h`.

## Dosyalar
- `src/raster/raster.h/.c` — çekirdek:
  - Girdi: `RasterVert { f32 x, y; f32 z; f32 inv_w; f32 var[SHADE_MAX_VARYINGS]; }` (x,y piksel uzayında).
  - `raster_begin(Rasterizer*, Framebuffer*, Arena* frame)`, `raster_set_shader(r, ShadeFn, uniforms, textures, varying_count, blend)`, `raster_triangle(r, v0, v1, v2)`, `raster_flush(r)`.
  - Kurulum: 24.8 sabit nokta, top-left fill kuralı, arka yüz ayıklama bayrağı, kenar fonksiyonları artımlı.
  - Karo binning: 64×64 karo; üçgenler önce bin'lenir (frame arena), `raster_flush`'ta karo karo işlenir. Karo içinde 8×8 blok trivial-reject / trivial-accept.
  - Quad üretimi: 2×2 quad'lar, `QuadHeader.mask`; varyingler barisentrik (2D'de affine, 3D'ye hazırlık için `inv_w` ile perspektif-doğru yol da yaz ama 2D'de inv_w=1).
  - Batch: 256 quad dolunca veya karo bitince `ShadeBatch` doldur → `ShadeFn` çağır → blend → framebuffer'a yaz (mask'e uyarak).
  - Blend modları: `BLEND_NONE`, `BLEND_ALPHA` (src-over, düz alpha), `BLEND_ADD`. f32 → RGBA8 dönüşümü: clamp, `*255 + 0.5`, trunc.
- `src/raster/texture.h/.c` — `Texture` (shade_abi.h) yaratma, prosedürel test dokuları (dama, gradyan, gürültü), 2'nin kuvveti zorunluluğu.
- `src/raster/ref_shaders.h/.c` — C referans `ShadeFn`'leri: `ref_shade_solid` (uniform renk), `ref_shade_vcolor` (varying renk), `ref_shade_sprite` (doku × tint uniform, alpha<0.01 discard). Örnekleme `shade_abi.h` semantiğine BİREBİR uyacak (Matoki ile karşılaştırılacak).
- `src/gfx2d/batch2d.h/.c` — 2D sprite API: `b2d_begin(camera Mat3)`, `b2d_sprite(tex, src_rect, dst_pos, size, rot, tint)`, `b2d_end()` → raster'a üçgen besler; doku değişiminde flush.
- `src/app/main.c` — demo: 10.000 zıplayan/dönen sprite (dokular prosedürel), 1-9 tuşlarıyla sprite sayısı 1k..100k, B tuşu blend modu, P tuşu profiler raporu.
- `tests/test_raster.c` — host testleri:
  - Paylaşılan kenar testi: rastgele 1000 üçgen çifti ortak kenarlı → hiçbir piksel iki kez veya sıfır kez boyanmaz (quad ızgarasını kapsayan dikdörtgen içinde).
  - Top-left kuralı: tam piksel sınırına oturan dikdörtgen tam olarak w*h piksel boyar.
  - Varying interpolasyonu: köşelerde verilen değerler piksel merkezinde doğru (tol 1e-4).
  - Blend doğruluğu (bilinen değerler).

## Kabul kriterleri
- `make` ve `make test` uyarısız geçer.
- Tarayıcıda 10k sprite demosu akıcı. **Benchmark tablosu (Notlar'a):** 1k/10k/50k sprite için ms/frame, Mpiksel/s, profiler bölgeleri (setup / bin / raster / shade / blend). Tarayıcı ve CPU modelini yaz.
- Ölçmeden optimizasyon iddiası yok. En pahalı bölgeyi tespit et ve Notlar'a yaz (M5'te SIMD'lenecek).

## Notlar

### Yapılanlar
1. **`src/raster/raster.h/.c`**:
   - 24.8 sabit nokta (`Fx8`) kurulumu, piksel merkezi `(px + 0.5, py + 0.5)` (`FX8_HALF = 128`) ofseti ve **top-left** doldurma kuralı (`(a > 0) || (a == 0 && b > 0)` için `bias = 0`, diğer kenarlar için `bias = -1`) gerçeklendi.
   - `raster_set_cull` ile arka yüz ayıklama (`cull_back`) desteği eklendi.
   - 64×64 karo binleme (`TileBinChunk`), karo içi 8×8 bloklarda köşe ofsetleriyle `trivial-reject` ve `trivial-accept` (`mask = 0xFu`), kısmi bloklarda ise 2×2 quad artımlı kenar adımlaması gerçeklendi.
   - Barisentrik varying interpolasyonu için hem 2D afin (`inv_w == 1`) hem de 3D perspektif-doğru (`inv_w != 1`) yol yazıldı. Kısmî (`count < 256`) `ShadeBatch` boşaltımlarında `varyings[(v * count + q) * 4 + lane]` SoA adımının `shade_abi.h` ile birebir uyumu sağlandı.
   - `BLEND_NONE`, `BLEND_ALPHA` (src-over düz alpha) ve `BLEND_ADD` harmanlama modları (`clamp(0..1) * 255.0f + 0.5f` → `trunc`) gerçeklendi.
2. **`src/raster/texture.h/.c`**:
   - `IS_POW2(w) && IS_POW2(h)` zorunluluğu ve 16-byte hizalı `tex_init`/`tex_create` ile dama (`tex_gen_checker`), 4-köşe gradyan (`tex_gen_gradient`) ve PCG32 gürültü (`tex_gen_noise`) üreteçleri yazıldı.
3. **`src/raster/ref_shaders.h/.c`**:
   - `shade_abi.h` örnekleme semantiğine birebir uyan `ref_sample_tex` (nearest + bilinear, wrap + clamp) ve `ref_shade_solid`, `ref_shade_vcolor`, `ref_shade_sprite` (`alpha < 0.01f` discard + uniform tint) yazıldı.
4. **`src/gfx2d/batch2d.h/.c` & `src/app/main.c`**:
   - `b2d_begin`, `b2d_sprite`, `b2d_end` ile her sprite 2 üçgene dönüştürülüp doku değişiminde `raster_flush` çağrıldı.
   - `main.c` içinde 3 prosedürel dokulu (dama, gradyan, dairesel alpha maskeli gürültü) 10.000 zıplayan/dönen sprite demosu (`1-9`: 1k..100k sprite, `B`: blend modu, `P`: profiler raporu) kuruldu.
5. **`tests/test_raster.c`**:
   - Top-left kuralı (hem tam piksel sınırı hem piksel merkezinden geçen dikdörtgenlerde tam `w*h` piksel), 1000 rastgele dikdörtgen + 1000 rastgele eğik dışbükey dörtgen ortak kenar testi (0 boşluk, 0 çift boyama), piksel merkezinde `1e-4` toleransla afin + perspektif varying interpolasyonu, blend modları ve `batch2d` discard testleri eklendi.

### ÖNERİ (Dokunulmayan Dosyalar İçin)
- **ÖNERİ (`web/platform.js` ve `web/jit_test.js`):** Her iki JS dosyasında `new WebAssembly.Table({ element: "anyfunc", initial: 1 })` sabit kodlanmış durumda. C kodunda herhangi bir fonksiyonun adresi alındığında `wasm-ld`, `__indirect_function_table` import alt sınırını `1 + fonksiyon_sayısı` (`>= 2`) yapıyor ve `initial: 1` ile `LinkError` oluşuyor. Bunu `web/platform.js`'ye dokunmadan çözmek için `wasm32` derlemesinde yerleşik C referans shader'larına yüksek nöbetçi (sentinel) `ShadeFn` kimlikleri (`0xFFFFFFF1..3`) verildi; böylece tablo import alt sınırı `1` kaldı ve JIT shader indeksleri (`1..N`) `call_indirect` ile çalışmaya devam etti. İleride `web/platform.js`'de tablo oluştururken `WebAssembly.Module.imports(mod)` içinden minimum tablo boyutunu okumak veya `initial` değerini büyütmek faydalı olacaktır.

### Ölçülen Kod Optimizasyonları (Öncesi / Sonrası)
Aynı sahnede (10.000 sprite, büyük `10..16 px` boyutta ~1.55M piksel/frame, `BLEND_NONE`):
- `ref_shaders.c` içinde `trunc_sat_f32`'nin `__wasm__` üzerinde 3 dallanmalı C kontrolü yerine doğrudan `(i32)x` (`-mnontrapping-fptoint` → tek `i32.trunc_sat_f32_s` komutu) kullanması,
- `raster.c` içinde `BinnedTri` ve varying katsayılarının üçgen başına ayrı `arena_push` yerine tek blokta ayrılması,
- `emit_quad` içinde `varying_count == 2` (2D UV) ve `BLEND_NONE` içinde `mask == 0xFu` (tam kapsanan 2×2 quad) hızlı yollarının ayrılması:

| Metrik (10k sprite, 1.55M px/frame) | Optimizasyon Öncesi | Optimizasyon Sonrası | Değişim |
|---|---:|---:|---:|
| `setup` (Ort ms) | 1.492 ms | 0.891 ms | **-%40.3** |
| `shade` (Ort ms) | 31.700 ms | 24.078 ms | **-%24.0** |
| `raster` (Ort ms) | 19.721 ms | 17.886 ms | **-%9.3** |
| Toplam kare süresi | 75.337 ms | 65.688 ms | **-%12.8** |

### Benchmark Tablosu (1k / 10k / 50k Sprite)
- **CPU:** `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`
- **Tarayıcı / Wasm Motoru:** `Google Chrome 156.0.8078.47` (V8 `14.2.231.21`, wasm32 `-O2`)
- **Çözünürlük:** `640×360`, 3 prosedürel doku (`32×32`), sprite boyutu `4..8 px`

#### 1. `BLEND_NONE`
| Sprite Sayısı | FPS | ms/frame | Mpiksel/s | `setup` (ms) | `bin` (ms) | `raster` (ms) | `shade` (ms) | `blend` (ms) | Batch Çağrısı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **1.000 (1k)** | 529.3 | 1.89 ms | 17.7 | 0.079 | 0.021 | 0.443 | 0.645 | 0.491 | 180 |
| **10.000 (10k)** | 54.7 | 18.28 ms | 18.4 | 0.872 | 0.186 | 5.023 | 6.414 | 4.801 | 703 |
| **50.000 (50k)** | 10.7 | 93.67 ms | 17.9 | 4.529 | 0.946 | 32.765 | 33.514 | 24.625 | 3164 |

#### 2. `BLEND_ALPHA` (src-over düz alpha)
| Sprite Sayısı | FPS | ms/frame | Mpiksel/s | `setup` (ms) | `bin` (ms) | `raster` (ms) | `shade` (ms) | `blend` (ms) | Batch Çağrısı |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **1.000 (1k)** | 435.7 | 2.30 ms | 14.6 | 0.079 | 0.021 | 0.435 | 0.633 | 0.923 | 179 |
| **10.000 (10k)** | 47.4 | 21.09 ms | 15.9 | 0.808 | 0.169 | 4.696 | 6.175 | 8.502 | 705 |
| **50.000 (50k)** | 10.0 | 99.57 ms | 16.8 | 4.103 | 0.853 | 28.832 | 31.015 | 39.596 | 3159 |

### Darboğaz Analizi (M4 JIT ve M5 SIMD Hedefleri)
1. **`BLEND_NONE` modunda en pahalı bölge `shade` (%35.1 — 10k'da 6.41 ms):** Her 2×2 quad'ın 4 lane'i için ayrı skaler UV→texel hesabı, RGBA8→f32 açma (`* 1/255.0f`) ve tint çarpımı yapılıyor. M4'te Matoki JIT 4-lane `v128` SIMD kod üretimi ile doğrudan hızlanacak.
2. **`BLEND_ALPHA` modunda en pahalı bölge `blend` (%40.3 — 10k'da 8.50 ms):** Hedef piksellerin `u32` RGBA8'den 4 adet `f32`'ye açılması, `sr*sa + dr*(1-sa)` karışımı ve tekrar `[0,1]` clamp + `*255 + 0.5f` ile `RGBA8`'e paketlenmesi tamamen skaler çalışıyor. M5'te SIMD blend ve JIT blend füzyonu (`ARCHITECTURE.md §4`) için birincil hedeftir.
3. **Üçüncü bölge `raster` (%22–%27 — 10k'da 4.70–5.02 ms):** 2×2 quad kenar değerlendirmesi ve SoA varying (`f32x4`) yazımı M5'te `i32x4` / `f32x4` SIMD ile vektörleştirilecektir. `setup` (%4.5) ve `bin` (%0.9) toplam sürenin %5.5'inden azını almaktadır.
