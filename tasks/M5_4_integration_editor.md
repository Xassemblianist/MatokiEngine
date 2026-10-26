DURUM: DONE
HAT: E (entegrasyon + motor içi shader editörü)

# M5_4 — Matoki'yi motora bağla, canlı shader editörü, debug overlay, ekran görüntüsü (BÜYÜK)

## Amaç
Bütün parçaları tek bir demoda birleştirmek: tarayıcıda, **motorun kendi UI'ıyla çizilmiş bir metin editöründe**
Matoki kodu yaz → Ctrl+Enter → motor içinde derlenir → JIT → ekranda anında değişir. Hata varsa satırın altı çizilir.

## Sende (bu turda sahibi sensin)
`src/app/*`, `web/platform.js`, `web/index.html`, `src/plat/input.h`, `src/ui/*`, `src/img/*`, `src/gfx2d/*`.
`src/plat/platform.h`'e **yalnızca ekleme**: `plat_download(name, name_len, ptr, len)` ve gerekirse `plat_clipboard_*`.

## Hazır olanlar (DOKUNMA)
`Makefile` (shaders/*.mtk artık otomatik gömülü: `src/core/embed.h`), `src/raster/*` (A — API'sini kullan), `src/matoki/*` (D), `src/thread/*` + `web/jit.js` + `web/threads.js` (B), `src/masm/*` (C).

## İşler
1. **platform.js tablo düzeltmesi:** `new WebAssembly.Table({initial: 1})` sabit değeri yerine `WebAssembly.Module.imports(module)`'dan tablo import'unun minimumunu oku (veya güvenli büyük başlangıç). Sonra A hattına ÖNERİ bırak: `ref_shaders.c`'deki sentinel `ShadeFn` kimlikleri (`0xFFFFFFF1..3`) hack'i kaldırılabilir (A bu turda raster.c'yi yazıyor; sentinel'leri A kaldıracak — sen yalnızca JS'i düzelt ve Notlar'a yaz).
2. **Shader yöneticisi** `src/app/shaders.h/.c`: açılışta `g_embedded_shaders`'ı `mtk_compile_wasm` + `jit_load_shade` ile derle, isimle bul, derleme süresini ölç (ms, shader başına — Notlar'a). Derleme hatasında C referans shader'a düş. Füzyon: D'nin `fuse_blend` desteği geldiğinde blend moduna göre varyant seç (`MtkCodegenOpts.fuse_blend` + A'nın `fused` parametresi); gelmediyse NONE ile çalış.
3. **Sahne seçici:** 1) 10k sprite (artık `sprite.mtk` JIT ile), 2) tam ekran `plasma.mtk` (tek büyük quad), 3) `branchy.mtk` gösterimi, 4) editör sahnesi. Tab ile geçiş. Her sahnede C referans ↔ JIT geçişi (J tuşu) — ve ekranda ms karşılaştırması.
4. **Motor içi metin editörü** `src/ui/editor.h/.c`: kendi fontumuzla; imleç, seçim, kaydırma, satır numaraları, Matoki sözdizimi renklendirme (B'nin lexer'ını kullan: `src/matoki/lexer.h`), Ctrl+Enter derle, hata satırı kırmızı alt çizgi + mesaj paneli (`MtkDiag`), Ctrl+Z/Y geri al/yinele (sabit boyutlu halka), Ctrl+C/V (`plat_clipboard_*` veya şimdilik iç pano).
   Metin girişi: `InputState`'e `u32 chars[32]; u32 char_count;` ekle (JS `keydown`/`input` ile UTF-32 kod noktası yazar), tuş tekrarı.
   Önizleme: editörün yanında (bölünmüş ekran) derlenen shader canlı çalışır; `uniform time: f32;` ve `uniform resolution: vec2;` otomatik sağlanır.
5. **Debug overlay** (F1): FPS + frame süresi grafiği, profiler tablosu, JIT bilgisi (shader başına wasm boyutu, IR komut sayısı, derleme ms).
6. **Ekran görüntüsü** (F12): framebuffer → E'nin kendi PNG kodlayıcısı → `plat_download("matoki.png", ...)`.
7. **SAB uyumu:** B hattı belleği shared yapacak; `ImageData` SAB kabul etmez → framebuffer'ı ayrı bir `Uint8ClampedArray`'e kopyala (B'nin build değişikliği gelince test et; gelmezse kodu iki duruma da hazır yaz: `memory.buffer instanceof SharedArrayBuffer`).

## Kabul kriterleri
- `make`, `make test` uyarısız.
- Tarayıcıda: editörde `plasma.mtk`'yi değiştir, Ctrl+Enter → anında görünür; bilinçli sözdizimi hatası → kırmızı alt çizgi + doğru satır:sütun mesajı; motor çökmez.
- Notlar: her gömülü shader için derleme süresi (ms) ve wasm boyutu; sahne başına JIT vs C referans ms tablosu.

---

## Notlar (E Hattı)

### 1. Yapılanlar
- **`web/platform.js` (Tablo, SAB, İndirme, Metin Girişi ve Worker Entegrasyonu):**
  - `WebAssembly.Module.imports(mod)` taranarak `env.__indirect_function_table` import'unun `minimum`/`initial` boyutu (en az 64) dinamik okunacak şekilde güncellendi. Artık C fonksiyon işaretçilerinin wasm tablosuna girmesi `LinkError` üretmiyor.
  - **ÖNERİ (A hattına):** `web/platform.js` artık tablo import alt sınırını modülden dinamik okuduğu için `src/raster/ref_shaders.h` ve `src/raster/raster.c` içindeki `0xFFFFFFF1..3` nöbetçi (sentinel) `ShadeFn` kimlikleri kaldırılıp doğrudan C fonksiyon adresleri (`ref_shade_solid`, `ref_shade_vcolor`, `ref_shade_sprite`) kullanılabilir.
  - **SAB uyumu:** B hattının `--shared-memory` (`SharedArrayBuffer`) değişikliğiyle tam uyumlu hale getirildi. `memory.buffer instanceof SharedArrayBuffer` olduğunda framebuffer her karede yerel bir `Uint8ClampedArray` tamponuna kopyalanarak `ImageData`'ya verilir (`readCString` ve `plat_download` da SAB dilimini kopyalayarak okur). Ayrıca `web/threads.js` mevcutsa `startThreads()` otomatik çağrılır.
  - `plat_download(name, name_len, ptr, len)` import'u `src/plat/platform.h`, `web/platform.js` ve `tools/host/plat_host.c`'ye eklendi.
  - `src/plat/input.h` içindeki `InputState` yapısına `u32 chars[32]; u32 char_count;` eklendi (toplam 412 byte) ve JS `keydown` dinleyicisinde basılan karakterlerin UTF-32 kod noktaları bu tampona yazıldı.
- **Shader Yöneticisi (`src/app/shaders.h/.c`):**
  - Açılışta `g_embedded_shaders` içindeki 5 gömülü `.mtk` kaynağını `mtk_compile_ir` + `ir_optimize` + `mtk_codegen` + `jit_load_shade` ile derler; derleme süresini (`compile_ms`), optimize edilmiş IR komut sayısını (`ir_inst_count`) ve üretilen wasm boyutunu (`wasm_bytes`) kaydeder.
  - D hattının `fuse_blend` (`MTK_FUSE_REPLACE`, `MTK_FUSE_ALPHA`, `MTK_FUSE_ADD`) desteğini otomatik yoklar; füzyonlu varyant üretildiğinde A hattının `raster_set_shader_ex(..., fused=TRUE)` ve `b2d_set_shader_ex(..., fused=TRUE)` API'leriyle çalıştırır, henüz yoksa `MTK_FUSE_NONE` ile sorunsuz çalışır.
  - `plasma.mtk` ve `branchy.mtk` için C referans karşılıkları (`ref_shade_plasma`, `ref_shade_branchy`) eklendi.
- **Motor İçi Canlı Metin Editörü (`src/ui/editor.h/.c`):**
  - Kendi 8×8 bitmap fontumuzla (`src/ui/font.h`) çizilen, satır numaralı, `src/matoki/lexer.h` üzerinden canlı Matoki sözdizimi renklendirmeli (anahtar kelimeler, tipler, yerleşik fonksiyonlar, sayılar, operatörler, yorumlar) metin editörü yazıldı.
  - İmleç hareketi, Shift+ok ve fare sürükleme ile metin seçimi, otomatik kaydırma, otomatik girinti (auto-indent), tuş tekrarı, 16 derinlikli sabit halka tamponlu `Ctrl+Z` / `Ctrl+Y` (geri al / yinele) ve `Ctrl+C` / `Ctrl+X` / `Ctrl+V` iç pano desteği eklendi.
  - `Ctrl+Enter` veya üst çubuktaki `Derle [Ctrl+Ent]` butonu ile anında yeniden derler; hata durumunda ilgili satırları vurgular, hata sütununa kırmızı dalgalı alt çizgi çizer ve alt panelde `MtkDiag` (`satır:sütun: hata: ...` + `^`) mesajını gösterir; son geçerli shader çalışmaya devam eder, motor çökmez.
- **4 Sahne, F1 Debug Overlay ve F12 PNG Ekran Görüntüsü (`src/app/main.c`):**
  - `Tab` (veya üst çubuktaki tıklanabilir sekmeler) ile 4 sahne arasında geçiş: **1) 10k Sprite (`sprite.mtk`)**, **2) Tam Ekran Plasma (`plasma.mtk`)**, **3) Branchy (`branchy.mtk`)**, **4) Bölünmüş Ekran Canlı Editör (`editor.mtk` + `time` ve `resolution` otomatik uniform)**.
  - `J` tuşu ile her sahnede anlık **Matoki JIT ↔ C Referans** geçişi ve üst çubukta canlı ms karşılaştırması.
  - `F1` ile açılan Debug Overlay: FPS, 120 karelik frame süresi grafiği, `core/profiler.h` bölge tablosu (`setup`, `bin`, `raster`, `shade`, `blend`) ve JIT shader tablosu (wasm byte boyutu, IR komut sayısı, derleme ms).
  - `F12` ile aktif framebuffer'ı kendi PNG kodlayıcımızla (`png_encode`) kodlayıp `plat_download("matoki.png", ...)` üzerinden indirme.

### 2. Gömülü Shader Derleme Süresi ve Wasm Boyutu Tablosu
Ölçüm ortamı: **Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz**, Node.js v22.21.1 (V8 Liftoff/TurboFan Wasm SIMD128, `--shared-memory` etkin). Süreler `mtk_compile_ir` + `ir_optimize` + `mtk_codegen` + `WebAssembly.Module/Instance` (`plat_jit_load`) toplamıdır:

| Shader | Kaynak (Byte) | IR Komut Sayısı (Opt Sonrası) | Wasm Boyutu (Byte) | Derleme + JIT Yükleme (ms) | Uniform (Byte) | Varying (Skaler) |
|---|---:|---:|---:|---:|---:|---:|
| `solid.mtk` | 131 B | 8 | 304 B | **0.329 ms** | 16 B | 0 |
| `vcolor.mtk` | 156 B | 16 | 411 B | **0.335 ms** | 16 B | 4 |
| `sprite.mtk` | 250 B | 56 | 786 B | **0.789 ms** | 16 B | 2 |
| `branchy.mtk` | 550 B | 38 | 633 B | **4.398 ms** *(ilk JIT soğuk başlangıç)* | 16 B | 2 |
| `plasma.mtk` | 646 B | 332 | 2885 B | **1.482 ms** | 16 B | 2 |
| `editor.mtk` *(canlı plasma+res)* | 741 B | 337 | 2926 B | **1.139 ms** | 16 B | 2 |

### 3. Sahne Başına Matoki JIT vs C Referans Performans Tablosu (640×360)
Her sahnede 40 kare ısınma (warmup) sonrasında 100 kare ortalaması (Intel i7-10750H, V8 Wasm SIMD128):

| Sahne | Matoki JIT Frame (ms) | C Referans Frame (ms) | Matoki JIT `shade` (ms) | C Referans `shade` (ms) | `shade` Hızlanması | Toplam Frame Hızlanması |
|---|---:|---:|---:|---:|---:|---:|
| **1) 10k Sprite (`sprite.mtk`)** | **10.85 ms** (92.2 FPS) | 14.04 ms (71.2 FPS) | **3.17 ms** | 6.32 ms | **1.99×** | **1.29×** |
| **2) Tam Ekran Plasma (`plasma.mtk`)** | **5.59 ms** (178.9 FPS) | 16.58 ms (60.3 FPS) | **4.63 ms** | 15.54 ms | **3.36×** | **2.97×** |
| **3) Branchy (`branchy.mtk`)** | **1.48 ms** (677.0 FPS) | 1.41 ms (707.2 FPS) | **0.37 ms** | 0.37 ms | **1.01×** | **0.96×** |
| **4) Editör Önizleme (`editor.mtk`)** | **3.34 ms** (299.4 FPS) | 8.68 ms (115.3 FPS) | **2.33 ms** | 7.67 ms | **3.29×** | **2.60×** |

- **Gözlem:** Aritmetik ve trigonometri yoğun `plasma.mtk` ve `editor.mtk` sahnelerinde 4-lane SPMD SIMD vektörleştirme sayesinde `shade` aşaması **3.3×**, tüm frame **2.6×–3.0×** hızlanmaktadır. `sprite.mtk` (10k sprite) sahnesinde JIT `shade` süresi 6.32 ms'den **3.17 ms**'ye (**2.0×** hızlanma) inmiştir. `branchy.mtk` sahnesinde ise C referansı piksel başına skaler `if/else` ile tek dal çalıştırırken, SPMD JIT maskeli yürütme yaptığından ve sahne çok hafif (~0.37 ms shade) olduğundan iki mod başa baş (~0.37 ms) çalışmaktadır.
- **Ekran Görüntüsü (F12):** 640×360 framebuffer `png_encode` ile ~42.3 ms'de 85.5 KB geçerli PNG'ye kodlanıp `plat_download("matoki.png", ...)` ile indirilmektedir.

