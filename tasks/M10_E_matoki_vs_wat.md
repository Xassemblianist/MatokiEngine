DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M10_E — Matoki çıktısı vs elle yazılmış en iyi WAT (verim ölçümü)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turdaki incelemelerde bozuk/düşmanca girdi hataları bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle.
Matoki derleyicisine dokunmadan önce `docs/MATOKI.md` okunur. **Bu görevde derleyici DEĞİŞTİRİLMEZ** — yalnız ölçülür ve raporlanır.

## Hedef
"Matoki wasm'a yüzde kaç verimle dönüşüyor?" sorusunu ölçerek cevaplamak. `shaders/sprite.mtk`, `plasma.mtk`, `vcolor.mtk`,
`lit3d.mtk` için: (a) Matoki JIT çıktısı (`build/host/matc` ile .wasm; `--ir-opt` ile IR), (b) aynı ABI'yi (`src/raster/shade_abi.h`,
`ShadeBatch`) uygulayan **elle yazılmış, mümkün olan en hızlı WAT** (`asm/shade_*.wat`). Her ikisi de aynı girdilerde bit-bit
(veya belgelenmiş ≤1 LSB) aynı çıktıyı vermeli. Node'da (`tools/host/` altındaki mevcut .mjs kalıplarını izle) 1M quad üzerinde
Mquad/s, iç döngü komut sayısı ve kod boyutu. Sonuç: her shader için **Matoki / elle WAT oranı (%)** ve farkın nereden geldiği
(`wasm2wat` / `build/host/mdis` iç döngü karşılaştırması) — derleyiciye hangi optimizasyonların eklenmesi gerektiğine dair
somut, ölçüme dayalı öneri listesi (Notlar'a, öncelik sırasıyla, beklenen kazançla).

## Dosyalar (YALNIZCA bunlar)
- `asm/shade_sprite.wat`, `asm/shade_plasma.wat`, `asm/shade_vcolor.wat`, `asm/shade_lit3d.wat`
- `tools/host/bench_matoki_vs_wat.mjs`, `tools/host/run_matoki_vs_wat.sh` (derle + doğrula + ölç)
- `tasks/M10_E_matoki_vs_wat.md`

## Kabul
- [x] `make check` hatasız (bu görev src'ye dokunmaz)
- [x] 4 shader için doğruluk karşılaştırması (rastgele girdi, ≥10.000 quad) geçer
- [x] Ölçüm: ısınma + ≥15 tekrar, medyan; tablo Notlar'a; komut satırı tekrarlanabilir
- [x] Öneri listesi: her öneri için hangi shader'da ne kadar kazanç beklendiği (ölçülen farka dayanarak)

## Yasaklar
`src/*`, `docs/*`, `Makefile`, `shaders/*` değişmez. Uydurma sayı yazılmaz.

## Notlar

### 1. Tekrarlanabilir Çalıştırma Komutu
```sh
./tools/host/run_matoki_vs_wat.sh
# veya doğrudan:
node tools/host/bench_matoki_vs_wat.mjs --verify-quads 12345 --trials 21
```
Betik sırasıyla:
1. `build/host/matc` ile 4 `.mtk` dosyasını (ve `--ir` / `--ir-opt` çıktılarını) üretir.
2. `build/host/masm` ile 4 `asm/shade_*.wat` dosyasını `.wasm` ikilisine çevirir.
3. `build/host/mdis` → `build/host/masm` gidiş-dönüş (round-trip) ikili eşdeğerliğini ve iç döngü komut sayılarını doğrular.
4. Her `.wat` modülünü bozuk/düşmanca/sınır girdilere (`batch <= 0`, `count <= 0`, `count > 0x00FFFFFF` taşma koruması, `NULL` alt işaretçiler, `w/h <= 0`, `levels = 0 / 999`, `count = 1, 2, 3, 5, 7` kuyruk ve nöbetçi sınır kontrolü) karşı sınar.
5. `12.345` rastgele quad (`49.380` piksel, `197.520` `f32` kanal değeri; farklı mip seviyeleri, negatif/sarma UV'ler ve alfa eşik sınırları dahil) üzerinde doğruluk karşılaştırması yapar.
6. `1.000.000` quad (`250 batch × 4.000 quad`), `60` batch ısınma (`240.000` quad) + `21` nöbetleşe (interleaved) tekrar medyanı ile performans ölçer.

### 2. Doğruluk ve Performans Ölçüm Tabloları
- **Ortam**: `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`, Node `v22.21.1` (`linux x64`)

#### Tablo 1 — Doğruluk Karşılaştırması (`12.345` rastgele quad + bozuk girdi testleri)

| Shader | Quad Sayısı | Maske Eşitliği | Tam Bit Eşitliği (%) | Maks Mutlak Fark | Maks Fark (`LSB = 1/255`) | Ort. Fark (`LSB`) | Bozuk Girdi Testi | Durum |
|---|---:|---|---:|---:|---:|---:|---|---|
| `vcolor` | 12345 | Bit-bit aynı | %100.0 | `0.00e+0` | `0.00e+0 LSB` | `0.00e+0 LSB` | GEÇTİ | **GEÇTİ** |
| `sprite` | 12345 | Bit-bit aynı | %71.6 | `5.96e-8` | `1.52e-5 LSB` | `2.05e-6 LSB` | GEÇTİ | **GEÇTİ** |
| `plasma` | 12345 | Bit-bit aynı | %26.3 | `8.60e-5` | `2.19e-2 LSB` | `1.82e-3 LSB` | GEÇTİ | **GEÇTİ** |
| `lit3d` | 12345 | Bit-bit aynı | %73.8 | `1.79e-7` | `4.56e-5 LSB` | `2.43e-6 LSB` | GEÇTİ | **GEÇTİ** |

- `vcolor`: %100 bit-bit aynı (`0.00 LSB`).
- `sprite` ve `lit3d`: `1/255.0` sabitinin döngü öncesinde `tint` / `base` uniform'u ile birleştirilmesinden kaynaklanan en fazla `1–2 ULP` (`0.000015 – 0.000046 LSB`, yani `1 LSB` sınırının **20.000 kat altında**) yuvarlama farkı; `sprite` `discard` maskeleri %100 bit-bit aynıdır.
- `plasma`: `cos(x) = sin(x + pi/2)` dönüşümü ve $[-\pi/2, \pi/2]$ üzerinde tek 7. derece minimax polinom kullanımıyla maks fark `0.0219 LSB` (`1 LSB` sınırının **45 kat altında**).

#### Tablo 2 — Performans, İç Döngü Komut Sayısı ve Kod Boyutu (`1M` Quad, `21` Tekrar Medyan)

| Shader | IR (Ham → Opt) | İç Döngü Komutu (Matoki → Elle WAT) | Wasm Boyutu (Matoki → Elle WAT) | Matoki JIT (`Mquad/s`) | Elle WAT (`Mquad/s`) | Elle WAT Hızlanma | Matoki / Elle WAT Verimi (%) |
|---|---:|---:|---:|---:|---:|---:|---:|
| `vcolor` | `16 → 16` | `47 → 136 (4x=34/q)` | `299 B → 709 B` | **361.4** | **455.8** | **1.26×** | **%79.3** |
| `sprite` | `57 → 54` | `119 → 110` | `589 B → 677 B` | **78.0** | **86.2** | **1.11×** | **%90.4** |
| `plasma` | `456 → 332` | `683 → 395` | `2266 B → 1452 B` | **15.3** | **20.2** | **1.32×** | **%75.6** |
| `lit3d` | `266 → 238` | `670 → 545` | `2356 B → 1938 B` | **16.4** | **19.5** | **1.19×** | **%84.2** |

- **Özet**: Matoki JIT derleyicisi, elle yazılmış en iyi `.wat` karşısında dört shader genelinde **%75.6 – %90.4** (aritmetik ortalama **%82.4**, geometrik ortalama **%82.2**) derleme verimine ulaşmaktadır.

### 3. `mdis` İç Döngü Karşılaştırması — Fark Nereden Geliyor?

1. **`vcolor` (`47 komut/quad → 4x unroll ile 34 komut/quad`, Matoki verimi `%79.3`)**:
   - Matoki iç döngüde quad başına `47` Wasm komutu çalıştırır; bunun `6` komutu döngü sayacı ve geri dallanmadır (`q16 += 16; q16 != count; br_if`). Salt bellek bant genişliği ve 4 adet `f32x4.mul` ile sınırlı bu çekirdekte döngü kontrol yükü toplam komutların `%13`'ünü oluşturur.
   - Elle WAT, 4'lü döngü açma (`4x unroll`, adım `+64` bayt) ve sabit `offset=0 / 16 / 32 / 48` bellek anlıkları kullanarak döngü kontrolünü 4 quad'da bire indirir (`34 komut/quad`) ve döngü önündeki uniform yüklemelerinde `f32.load + f32x4.splat` yerine tek komutluk `v128.load32_splat` kullanır.

2. **`sprite` (`119 komut/quad → 110 komut/quad`, Matoki verimi `%90.4`)**:
   - Matoki `sample()` sonrasında her kanalı önce `(1/255.0)` ile çarpıp (`4 × f32x4.mul`), ardından `tint` uniform'u ile tekrar çarpar (`4 × f32x4.mul`). Elle WAT, döngü önünde `tint_scaled = tint * (1/255.0)` hesaplayarak döngü içinden quad başına `4 adet f32x4.mul` (8 Wasm komutu) eler.
   - Matoki `if (c.a < 0.01) discard;` ifadesini alfa kanalını `f32x4`'e çevirip `1/255.0` ile çarptıktan sonra `f32x4.lt(a, 0.01)` ile test eder. Elle WAT, `(px >>> 24) < 3` eşdeğerliğini doğrudan tam sayı (`i32x4.lt_s`) olarak sınar.

3. **`plasma` (`683 komut/quad → 395 komut/quad`, Matoki verimi `%75.6`)**:
   - **Çift polinom hesabı**: Matoki `emit_trig` içinde her `sin` ve `cos` çağrısı için $\pi/2$ indirgemesi yapıp **hem `sin_poly` hem `cos_poly`'yi** (2 polinom) hesaplar ve `v128.bitselect` ile seçer (`7 çağrı × 2 = 14 polinom/quad`). Elle WAT, `cos(x) = sin(x + \pi/2)` faz kaymasını döngü önündeki uniform/sabit terimlere katıp $\pi$ indirgemesi ($r \in [-\pi/2, \pi/2]$) ve işaret biti XOR'u (`(k << 31) ^ r`) kullanarak çağrı başına **yalnızca 1 polinom** (`7 polinom/quad`) hesaplar.
   - **Aşırı XMM yazmaç taşması (register spilling)**: Matoki `plasma`'da `44` adet `v128` local (`25` döngü önü + `19` döngü içi) tahsis eder ve 4 dalganın (`v1..v4`) indirgeme sonuçlarını aynı anda canlı tutar. x86-64'te yalnızca `16` donanım XMM yazmacı olduğundan V8 TurboFan döngü içinde yoğun yığın taşması (`stack spill/reload`) üretir. Elle WAT her dalgayı sıralı hesaplayıp `$acc` akümülatörüne ekleyerek toplam `15` `v128` local içinde kalır.

4. **`lit3d` (`670 komut/quad → 545 komut/quad`, Matoki verimi `%84.2`)**:
   - **Varying olarak işaretlenen quad-tekdüze mipmap hesabı (`~90` gereksiz komut/quad)**: `dFdx(uv)` ve `dFdy(uv)` 2×2 quad içindeki 4 lane'de aynıdır. Ancak Matoki `IR_DERIV_X/Y` sonucunu `IR_FLAG_VARYING` (`v128`) olarak işaretlediği için:
     - Türevlerde `8 adet i8x16.shuffle` + `4×` vektör aritmetiği üretir.
     - Mip seviyesine göre yapılan `w >> level`, `h >> level` ve `wh4 >> (2*level)` kaydırmalarında kaydırma miktarı `varying` sayıldığı için `emit_varying_shift` devreye girer ve **3 kez 4-lane skalerleştirilmiş kaydırma** (`3 × 22 = 66 komut`) üretir!
     - Elle WAT, 4 türevi `[dudx, dudy, dvdx, dvdy]` tek bir SIMD vektöründe paralel hesaplayıp `rho2`, dalsız (`select`) mip seviyesi, `wk`, `hk` ve `base_k` hesabını tamamen skaler (`f32`/`i32`) yapar.
   - **Bilinear `(1/255.0)` çarpanının ortak çarpan parantezine alınması**: Matoki 4 komşu texel × 4 kanal = `16 kez` `(1/255.0)` ile çarpıp bilinear lerp sonrasında `4 kez` de `base` uniform'u ile çarpar (`20 × f32x4.mul`). Elle WAT, `(1/255.0)` çarpanını döngü önünde `base` uniform'u ile birleştirip bilinear lerp sonrasında kanal başına 1 kez çarpar (`20 → 4 çarpma`, quad başına `16 adet f32x4.mul` tasarruf).
   - **x86-64 `f32x4.convert_i32x4_u` yerine `f32x4.convert_i32x4_s`**: Matoki `lit3d`'de texel kanallarını açarken `f32x4.convert_i32x4_u` kullanır (`src/matoki/codegen.c:1107`). x86-64 SSE/AVX'te işaretsiz `cvtdq2ps` komutu olmadığından V8 her `convert_i32x4_u` için ~6 makine komutu üretir (`16 × 5 = ~80` fazladan x86 komutu/quad). Oysa kanallar `0..255` aralığına maskelenmiştir (işaret biti 0'dır); işaretli `f32x4.convert_i32x4_s` bit-bit aynı sonucu tek bir `cvtdq2ps` komutuyla üretir.

### 4. Matoki Derleyicisi İçin Öncelik Sıralı Optimizasyon Önerileri

| Öncelik | Optimizasyon Önerisi | Dokunulacak Modül | Etkilenen Shader'lar | Beklenen Hız Kazancı (Ölçüme Dayalı) |
|---:|---|---|---|---|
| **P1** | **Quad-tekdüze (`quad-uniform`) analiz ve skaler mipmap seviyesi**: `IR_DERIV_X` / `IR_DERIV_Y` (`lane1 - lane0`, `lane2 - lane0`) 4 lane'de aynı değeri ürettiğinden `IR_FLAG_QUAD_UNIFORM` ile işaretlenmeli; yalnızca quad-uniform girdilere bağlı olan `rho2`, `level`, `wk = w >> level`, `hk = h >> level` ve `base_k` hesapları `v128` + 4-lane skalerleştirilmiş kaydırma (`emit_varying_shift`) yerine skaler `f32`/`i32` olarak üretilmeli. | `src/matoki/lower.c`, `src/matoki/codegen.c` | `lit3d` (ve tüm mipmap/türev kullanan shader'lar) | `lit3d`'de iç döngüden **~90 Wasm komutu** elenir; **+%8 – %10** hızlanma |
| **P2** | **Tek polinomlu $\pi$-indirgemeli `sin`/`cos` ve canlılık sıralaması**: `emit_trig` içinde $\pi/2$ indirgemesi + çift polinom (`sin_poly` + `cos_poly` + `bitselect`, ~56 komut) yerine $\pi$ indirgemesi ($k = \lfloor x/\pi + 0.5\rfloor$, `cos` için $+0.5$ faz kayması) ve işaret biti XOR'u (`(k << 31) ^ r`) ile **tek polinom** (~28 komut) üretilmeli; ayrıca çok terimli toplamlar (`v1 + v2 + v3 + v4`) derin ifade ağacı yerine akümülatörde toplanarak XMM yazmaç basıncı `<= 16` altında tutulmalı. | `src/matoki/codegen.c`, `src/matoki/opt.c` | `plasma` (ve tüm trigonometrik shader'lar) | `plasma`'da iç döngüden **~288 Wasm komutu** (`683 → ~395`) ve XMM stack spill'leri elenir; **+%30 – %35** hızlanma |
| **P3** | **`0..255` maskeli texel kanallarında `f32x4.convert_i32x4_s` kullanımı ve `(1/255.0)` ölçeğinin lerp sonrasına / uniform'a taşınması**: `codegen.c` içinde `& 0xFF` veya `>>> 24` sonrasında işaret biti 0 olduğundan `mb_f32x4_convert_i32x4_u` yerine x86'da tek `cvtdq2ps` komutuna derlenen `mb_f32x4_convert_i32x4_s` kullanılmalı; bilinear `sample()` içinde `(1/255.0)` çarpanı 4 komşu texel yerine yatay/dikey `lerp` sonrasına (veya sonraki uniform çarpanına) taşınmalı. | `src/matoki/lower.c`, `src/matoki/codegen.c` | `lit3d`, `sprite` (tüm dokulu shader'lar) | `lit3d`'de **16 `f32x4.mul` + ~80 x86 dönüşüm komutu** elenir (**+%8 – %10**); `sprite`'ta **4 `f32x4.mul`** elenir (**+%6 – %8**) |
| **P4** | **Basit (dokusuz / düşük komut sayılı) shader'larda `4x` döngü açma (loop unrolling) ve `v128.load32_splat`**: Döngü içi komut sayısı `< 64` olan ve `discard`/kontrol akışı içermeyen shader'larda ana döngü `4x` (`step = 64` bayt, sabit `offset=0/16/32/48`) açılarak kalan `count & 3` quad kuyruk döngüsüyle işlenmeli; döngü önündeki uniform yüklemelerinde `f32.load + f32x4.splat` yerine tek komutluk `v128.load32_splat` kullanılmalı. | `src/matoki/codegen.c` | `vcolor` (ve tüm basit/düz renk shader'ları) | `vcolor`'da quad başına komut `47 → 34`'e düşer; **+%25 – %30** hızlanma |
| **P5** | **Tam sayı alfa eşiği (`discard`) sabit katlama**: `sample()` çıktısının bir kanalı sabit bir eşikle karşılaştırıldığında (`c.a < K`), eşik döngü öncesinde `[0..255]` tam sayı aralığına (`ceil(K * 255)`) çevrilerek `f32` dönüşümü ve `1/255` çarpması yapılmadan doğrudan `i32x4.lt_s` ile sınanmalı. | `src/matoki/opt.c` | `sprite` (alfa testi kullanan shader'lar) | `sprite`'ta quad başına **4-5 SIMD komutu** elenir; **+%3 – %4** hızlanma |


### İnceleme (Claude)
- `run_matoki_vs_wat.sh` ayrı koşumda yeniden üretildi: verim %84.2 / %90.6 / %75.6 / %84.7 (Argon ölçümüyle ±%5 içinde).
- P1–P5 önerileri Matoki derleyici iyileştirme görevine (M11 sonrası) aday; en büyük kazanç P2 (tek polinomlu sin/cos) ve P1 (quad-tekdüze mip).
