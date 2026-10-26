DURUM: DONE
HAT: D (Matoki arka uç)

# M4_2 — IR kurucu, referans yorumlayıcı, optimizasyon geçişleri, SIMD codegen (BÜYÜK)

## Oku
`docs/MATOKI.md`, `src/matoki/ir.h` ve `src/matoki/codegen.h` (sözleşme — DOKUNMA), `src/raster/shade_abi.h`, `src/mbin/mbin.h`, `src/mbin/mbin_ops.h`.

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/mbin/*`, `src/matoki/ir.h`, `src/matoki/codegen.h`, `src/raster/shade_abi.h`, `tests/test.h`.
B hattı aynı anda ön ucu (lexer/parser/sema/lower/matoki.c) yazıyor; onların dosyalarına dokunma. Senin testlerin IR'ı elle kurar.

## Gereksinim
`node` (≥18) kurulu olmalı (`sudo apt install nodejs`). Yalnızca test aracı olarak; projeye npm/bağımlılık girmez.

## Dosyalar
- `src/matoki/ir.c` — `ir.h`'deki tüm kurucular, tip/tekdüzelik çıkarımı, `ir_verify`, `ir_print`, `ir_emit_sample` (shade_abi.h örnekleme semantiği BİREBİR; bilinear dahil).
- `src/matoki/ir_eval.c` — `ir_eval`: skaler, lane lane yorumlayıcı. Maskeli IF semantiği, discard, OUTPUT. f32 işlemleri wasm ile bit-uyumlu olmalı (min/max NaN kuralları, trunc_sat, nearest = çifte yuvarlama).
- `src/matoki/opt.c` — `ir_specialize_uniform_f32`, `ir_opt_constfold` (tüm op'lar; NaN güvenli cebir: `x*1→x`, `x+0→x` YALNIZCA -0 güvenliyse; `select(const)`, `bnot(bnot x)`, boş IF bölgesi), `ir_opt_cse` (bölge farkında), `ir_opt_dce`, `ir_compact`, `ir_optimize`.
- `src/matoki/codegen.c` — IR → wasm (mbin ile):
  - Fonksiyon: `(param $b i32)`; başta ShadeBatch alanlarını local'lere yükle; `q = 0..count` döngüsü.
  - **Uniform değerler skaler local** (f32/i32), **varying değerler v128 local**. Uniform → varying kullanımı ilk ihtiyaçta `splat` (tekrar etmeden, önbellekli).
  - Basit register tahsisi: her IR değeri için local; aynı tipteki local'ler canlılık analiziyle (son kullanım) yeniden kullanılır. Notlara: local sayısı öncesi/sonrası.
  - Stack makinesi optimizasyonu: tek kullanımlı ve hemen ardından tüketilen değerleri local'e yazmadan yığında bırak (tree-ification).
  - Maske: aktif lane maskesi local'i (başta `QuadHeader.mask` bitlerinden i32x4 maskeye açılır). Varying IF: `then` bölgesine yalnız `v128.any_true(cond & aktif)` ise gir; bölge içinde aktif maske daralır; ELSE için `andnot`. Uniform IF: düz `if/else`.
  - `IR_TEXEL_LOAD` varying: 4 lane extract → 4 `i32.load` → `replace_lane` (gather). RGBA8 açma: SIMD shift/and + `f32x4.convert_i32x4_u`'ya eşdeğer + `*1/255`.
  - DISCARD: maske bitlerini `QuadHeader.mask`'tan temizle (i32x4.bitmask ile).
  - OUTPUT: 4 kanal `v128.store` → `out_rgba[(c*count+q)*4]`.
  - `opts->own_memory`: test modu (kendi belleği + export "memory").
- `tools/host/run_shade.mjs` — node test koşucusu: `node run_shade.mjs modül.wasm girdi.bin çıktı.bin`. girdi.bin = ShadeBatch'in serileştirilmiş hali (sen tanımla: başlık + quad'lar + varyingler + uniform + dokular), modülün belleğine (own_memory) yerleştirir, işaretçileri düzeltir, `shade` çağırır, out_rgba + maskeleri çıktı.bin'e yazar.
- `tests/test_matoki_back.c`:
  - `ir_verify` olumlu/olumsuz durumlar; `ir_print` altın çıktı.
  - Constfold/CSE/DCE: elle kurulan IR'da beklenen komut sayısı; uniform özelleştirme sonrası `tint=1` çarpımının yok olduğu.
  - **Diferansiyel test (ana kriter):** rastgele IR üreteci (tip-doğru, iç içe IF, discard, sample dahil) → 500 rastgele program × rastgele batch → `ir_eval` sonucu == codegen'lenmiş wasm'ın node'daki sonucu (f32 bit-bit, yalnızca NaN'lar NaN olarak eşleşsin). `ir_optimize` öncesi ve sonrası da aynı sonucu vermeli.
  - Tüm üretilen modüller `wasm-validate --enable-all`'dan geçer.

## Kabul kriterleri
- `make`, `make test` uyarısız; diferansiyel testte 0 uyumsuzluk.
- Notlar: örnek bir sprite IR'ının (elle kurulmuş) wasm boyutu, local sayısı; `wasm2wat` çıktısından iç döngünün bir kesiti.

## Notlar (D hattı)

### 1. Yapılanlar
- **`src/matoki/ir.c`**: Tüm SSA IR kurucuları, otomatik tip ve `IR_FLAG_VARYING` çıkarımı, `ir_emit_sample` (nearest ve bilinear filtreleme, wrap ve clamp modları `shade_abi.h` semantiğine birebir uygun), `ir_verify` (tanımdan önce kullanım, tip/tekdüzelik uyumu, IF/ELSE/IF_END dengesi ve 4 kanallı OUTPUT kontrolü) ve `ir_print` yazıldı.
- **`src/matoki/ir_eval.c`**: WebAssembly IEEE-754 ve SIMD kurallarına (`f32.min`/`max` NaN ve `-0.0` davranışı, `f32.nearest` çift sayıya yuvarlama, `i32.trunc_sat_f32_s` doygun dönüşüm, maskeli varying IF ve uniform IF atlama semantiği) bit-bit uyumlu 4-lane referans yorumlayıcı yazıldı.
- **`src/matoki/opt.c`**:
  - `ir_specialize_uniform_f32`: Belirtilen byte ofsetindeki uniform okumalarını sabite dönüştürür (`tint = 1` özelleştirmesinde tüm çarpımlar elenir).
  - `ir_opt_constfold`: Tüm tekli/ikili işlemlerin sabit katlaması ve NaN/`-0.0` güvenli cebirsel sadeleştirmeler (`x * 1.0 -> x`, `x / 1.0 -> x`, `x + (-0.0) -> x`, `x - (+0.0) -> x`, `bnot(bnot(x)) -> x`, `select(const, t, e)` ve sabit koşullu IF dallarının budanması).
  - `ir_opt_cse`: Yapısal IF/ELSE bölge ağacı üzerinde kapsama (dominance) denetimli ortak alt-ifade eleme ve değişmeli işlemlerde kanonik sıralama.
  - `ir_opt_dce` & `ir_compact`: `OUTPUT` ve `DISCARD` köklerinden geriye doğru canlılık analizi, boş IF bloklarının silinmesi ve NOP sıkıştırma.
- **`src/matoki/codegen.c`**:
  - Uniform değerler skaler (`i32`/`f32`), varying değerler `v128` local'lerde tutulur; uniform → varying geçişleri ilk kullanımda önbellekli `splat` ile yapılır.
  - **Canlılık analizi (local havuzu) ve Tree-ification**: Tek kullanımlı ve hemen ardından gelen komutta ilk işlenen olarak tüketilen ifadeler (`inlined`) hiç local'e yazılmadan wasm yığınında bırakılır. Kalan değerler son kullanım indeksine göre (`LocalPool`) yeniden kullanılır ve `MB_VAL_I32`, `MB_VAL_F32`, `MB_VAL_V128` olarak 3 tekil local grubunda toplanır.
- **`tools/host/run_shade.mjs` & `tests/test_matoki_back.c`**:
  - 500 rastgele IR programı (iç içe uniform/varying IF, SELECT, DISCARD, nearest/bilinear sample dahil) × rastgele `ShadeBatch` üzerinde hem optimize edilmemiş hem optimize edilmiş (toplam 1000 `.wasm` modülü) `wasm-validate --enable-all` ve Node.js üzerinde çalıştırılıp `ir_eval` ile karşılaştırıldı: **0 uyumsuzluk (bit-bit eşleşme)**.

### 2. Örnek Sprite Shader (`docs/MATOKI.md §2`) Ölçümleri ve `wasm2wat` Kesiti
- **Kapsam**: `uniform tint: vec4`, `uniform tex: texture2d`, `in uv: vec2`, `sample(tex, uv)` + `if c.a < 0.01 { discard; }` + `return c * tint`.
- **Üretilen Wasm Boyutu**: **783 byte** (`build/test_matoki/sprite.wasm`).
- **Local Sayısı (Öncesi vs Sonrası)**:
  - Optimizasyon öncesi (her IR değerine + her uniform→varying splat'ına ayrı local): **56 IR local'i** (+ 14 sabit döngü/maske local'i = **70 toplam local**).
  - Tree-ification (10 ara değer yığında bırakıldı) + Canlılık analiziyle local yeniden kullanımı sonrası: **27 IR havuz local'i** (+ 14 sabit döngü/maske local'i = **41 toplam local**, 16 `i32`, 7 `f32`, 18 `v128`).
- **`wasm2wat` İç Döngü Kesiti (Varying Texel Gather + RGBA8 Açma + Maskeli IF Discard + Tint Çarpımı)**:
```wat
        ;; UV -> texel indeksi (wrap) ve 4-lane gather
        local.get 33
        local.get 36
        f32x4.mul
        f32x4.floor
        local.set 33
        local.get 34
        local.get 37
        f32x4.mul
        f32x4.floor
        local.set 34
        local.get 33
        i32x4.trunc_sat_f32x4_s
        local.set 36
        local.get 34
        i32x4.trunc_sat_f32x4_s
        local.set 33
        local.get 36
        local.get 38
        v128.and
        local.set 34
        local.get 33
        local.get 39
        v128.and
        local.get 35
        i32x4.mul
        local.set 33
        local.get 34
        local.get 33
        i32x4.add
        local.set 35
        ;; RGBA8 açma (tree-ification ile shift+and ve i2f+mul zincirlenmiş)
        local.get 33
        local.get 12
        i32x4.shr_u
        local.get 40
        v128.and
        local.set 35
        ...
        local.get 34
        f32x4.convert_i32x4_s
        local.get 41
        f32x4.mul
        local.set 33
        ;; if (c.a < 0.01) { discard; } — v128.any_true(cond & active_mask)
        local.get 36
        local.get 37
        f32x4.lt
        local.set 38
        local.get 24
        local.set 27
        local.get 38
        local.tee 28
        local.get 27
        v128.and
        local.tee 24
        v128.any_true
        if  ;; label = @3
          local.get 8
          local.get 8
          i32.load
          local.get 38
          local.get 24
          v128.and
          i32x4.bitmask
          i32.const -1
          i32.xor
          i32.and
          i32.store
        end
```

