DURUM: DONE
HAT: Argon ajanı (paralel, kendi worktree'si)

# M12_A — Matoki derleyici iyileştirmeleri (M10_E P1–P5)

Önce `AGENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md` okunur. Dış kütüphane YOK, `src/` altında libc YOK, malloc YOK
(bellek `core/arena.h`). Tipler `core/types.h`, matematik `core/math.h`. Kod stili: Türkçe yorumlar, `modul_fonksiyon`, dosya başı açıklama.
Sabit nokta: önceki turlardaki incelemelerde bozuk/düşmanca girdiler bulundu — **her girdi sınırını, aşırı boyutu, özyineleme derinliğini
ve sonsuz döngüyü** baştan düşün; arena taşması `PANIC`tir, ayırmadan önce boyutu denetle. Ölçmeden "hızlandı" yazma.
Matoki'ye dokunduğun için `docs/MATOKI.md` de okunur. Ölçüm altyapısı hazır: `tools/host/run_matoki_vs_wat.sh`
(M10_E; elle yazılmış en iyi WAT ile karşılaştırma) ve `tasks/M10_E_matoki_vs_wat.md` Notlar (P1–P5 önerileri, ayrıntılı gerekçe).

## Hedef
Matoki JIT çıktısını elle yazılmış WAT'a yaklaştırmak. Öncelik sırası (her birini ayrı commit, önce/sonra ölçümüyle):
1. **P2** `sin/cos`: π indirgemesi + işaret biti XOR ile çağrı başına **tek polinom**; çok terimli toplamlarda yazmaç
   basıncını düşüren sıralama (plasma). Doğruluk: mevcut testlerdeki tolerans içinde kalmalı.
2. **P1** Quad-tekdüze (quad-uniform) analiz: `dFdx/dFdy` ve yalnız bunlara + uniform'lara bağlı değerler (mip seviyesi,
   `w>>level`, taban ofseti) skaler hesaplanır; 4-lane skalerleştirilmiş kaydırma (`emit_varying_shift`) bu yolda kalkar.
3. **P3** 0..255 maskeli texel kanallarında `f32x4.convert_i32x4_s`; bilinear'da 1/255 ölçeğini lerp sonrasına taşıma.
4. **P4** Basit, discard'sız ve kısa (<64 komut) çekirdeklerde 4× döngü açma + `v128.load32_splat`.
5. **P5** Sabit alfa eşiği karşılaştırmasını tamsayı alanında yapmak.
Zaman yetmezse P2 → P1 → P3 sırasıyla bitir, kalanı Notlar'a "ertelendi" yaz.

## Dosyalar (YALNIZCA bunlar)
- `src/matoki/*.c/.h` (lower, codegen, opt, ir ...)
- `tests/test_matoki_*.c` (yeni testler: tek polinom trig doğruluğu, quad-tekdüze sınıflandırma, convert_s eşdeğerliği)
- `tools/host/bench_matoki_vs_wat.mjs`, `tools/host/run_matoki_vs_wat.sh` (yalnız gerekiyorsa)
- `tasks/M12_A_matoki_opt.md`

## Kabul
- [x] `make check` hatasız/uyarısız (ASan/UBSan)
- [x] `run_matoki_vs_wat.sh`: 4 shader'da doğruluk testleri GEÇTİ; vcolor/sprite/plasma/lit3d Matoki/WAT verimi önce-sonra tablosu
  (aynı makinede, nöbetleşe medyan) Notlar'a; plasma ve lit3d'de ölçülebilir artış
- [x] Tüm gömülü shader'lar (`shaders/*.mtk`, pbr3d ve dungeon3d dahil) hâlâ derleniyor; C referansıyla karşılaştıran
  mevcut testler geçiyor
- [x] Optimize edilmiş IR/wasm'ın `mdis` iç döngü komut sayıları önce/sonra Notlar'a

## Yasaklar
`docs/*`, `Makefile`, `src/app/*`, `src/raster/*`, `shaders/*`, `web/*` değişmez. Dil semantiği değişmez (yalnız kod üretimi).
Öneriler Notlar'a.

## Notlar

### 1. Yapılan İyileştirmeler (P2 → P1 → P3 → P4 → P5)

1. **P2 — Tek polinomlu $\pi$-indirgemeli `sin`/`cos` ve canlılık sıralaması (`d0b4a99`)**:
   - `src/matoki/lower.c`: `emit_trig` içinde $\pi/2$ indirgemesi + çift polinom (`sin_poly` + `cos_poly` + `bitselect`, ~56 komut) yerine $\pi$ indirgemesi ($k = \text{nearest}(x \cdot (1/\pi))$, $r = x - k\pi \in [-\pi/2, \pi/2]$), işaret biti XOR'u (`xr = bitcast_i2f((k << 31) ^ bitcast_f2i(r))`) ve 7. derece minimax tek polinom ($c_1=1.0$, $c_3=-0.166666546$, $c_5=0.008332161$, $c_7=-0.000195153$, maks hata $< 1.6 \times 10^{-4}$) üretildi. `cos(x) = sin(x + \pi/2)` faz kayması `x = A \pm B` argümanlarında uniform/sabit terime (`B + \pi/2` veya `B - \pi/2`) katılarak LICM ile döngü önüne taşındı.
   - `src/matoki/opt.c` ve `src/matoki/codegen.c`: `ir_opt_schedule_accum` geçişiyle çok terimli toplamlar (`v1 + v2 + v3 + v4`) her iki operand hazır olur olmaz birleştirildi; `codegen.c` içinde araya çok-kullanımlı döngü komutları girdiğinde `>= 2` döngü local'inin son kullanımını tutan alt ağaçların inline edilmesi (`count_dying_loop_locals`) engellendi. `plasma` içindeki toplam `v128` local sayısı `44`'ten `25`'e (döngü içi `v128` geçicileri `19`'dan `6`'ya) düştü.
   - Test: `tests/test_matoki_front.c` içine `matoki_trig_single_poly_accuracy` eklendi.

2. **P1 — Quad-tekdüze (`quad-uniform`) analiz ve skaler türev/mipmap yolu (`b0cf6a9`)**:
   - `src/matoki/codegen.c`: `CgCtx` içine `is_simd[CG_MAX_INSTS]` sınıflandırması eklendi. `IR_DERIV_X` ve `IR_DERIV_Y` (`f32x4.extract_lane + f32.sub`) quad içindeki 4 lane'de aynı değeri ürettiğinden `is_simd = FALSE` olarak işaretlendi; yalnızca en az bir `is_simd` operanda bağlı olan komutlar SIMD (`v128`) üretildi. Böylece `lit3d`'de türevlerden türeyen `rho2`, `level`, `wk = w >> level`, `hk = h >> level` ve `base_k` hesaplarının tamamı döngü içinde skaler (`f32`/`i32`) üretildi; 3 adet 4-lane skalerleştirilmiş kaydırma (`66` Wasm komutu) ve türev `i8x16.shuffle` komutları tamamen elendi. Tek kullanımlı skaler döngü değerlerinin (`w_f`, `h_f`) yığında doğrudan splat edilmesi sağlandı.
   - `src/matoki/ir.c`: `ir_unpack_texel` içinde `base + wy * w_i` satır ofseti paranteze alınarak yatay texel çiftlerinde CSE paylaşımı sağlandı; `ir_emit_mip_level` içinde `px2` terimleri gruplandı.
   - Test: `tests/test_matoki_mip.c` içine `mip_quad_uniform_scalar_codegen` eklendi.

3. **P3 — `0..255` maskeli texel kanallarında `f32x4.convert_i32x4_s` ve bilinear `1/255` ölçeğinin `lerp` sonrasına/uniform'a taşınması (`86a5340`)**:
   - `src/matoki/codegen.c`: `emit_fused_blend_epilogue` içinde `& 0xFF` ve `>>> 24` sonrasında kalan 3 adet `mb_f32x4_convert_i32x4_u` çağrısı x86-64'te tek `cvtdq2ps` komutuna derlenen `mb_f32x4_convert_i32x4_s` ile değiştirildi (kod üretiminde hiçbir `convert_i32x4_u` kalmadı).
   - `src/matoki/ir.c`: Bilinear `ir_emit_sample_impl` içinde `1/255` ölçek çarpması 4 komşu texel'in ayrı ayrı kanallarından (`16` çarpma) yatay/dikey `lerp` sonrasına (`4` çarpma) taşındı (döngü içinden `12` adet `f32x4.mul` elendi).
   - `src/matoki/lower.c`: `emit_scalar_binop` içinde `(varying * uniform1) * uniform2 -> varying * (uniform1 * uniform2)` yeniden gruplaması eklenerek `sample(t, uv) * base` / `c * tint` ifadelerinde `(1/255) * base` çarpımının LICM ile döngü önüne taşınması sağlandı (`lit3d` ve `sprite`'ta döngü içinden `4` adet `f32x4.mul` daha elendi).
   - Test: `tests/test_matoki_mip.c` içine `mip_convert_s_and_bilinear_scale` eklendi.

4. **P4 — Basit çekirdeklerde 4× döngü açma ve `v128.load32_splat` (`68091b2`)**:
   - `src/matoki/codegen.c`: Preheader'da yalnızca vektör splat olarak kullanılan (`need_splat && val_loc == IR_NONE`) uniform yüklemeleri `f32.load + f32x4.splat` yerine tek komutluk `v128.load32_splat` ile üretildi.
   - `src/matoki/codegen.c`: `discard`, doku ve kontrol akışı içermeyen kısa (`<= 32` IR komutu) basit çekirdeklerde (`vcolor`) 4× açılmış ana döngü (`offset=0/16/32/48`, adım `64` bayt) ve kalan `count & 3` quad için 1× kuyruk döngüsü üretildi; her kanal için kaynak ve hedef taban adresleri 4 quad'da bir kez hesaplandı.
   - Test: `tests/test_matoki_back.c` içine `matoki_unroll4x_and_load32_splat` eklendi.

5. **P5 — Sabit alfa eşiği karşılaştırmasının tamsayı alanına katlanması (`5b49cf0`)**:
   - `src/matoki/opt.c`: `ir_opt_alpha_cmp` geçişi eklendi. `[0..255]` bayt aralığındaki kanallar (`& 255` veya `>>> 24`) üzerinde yapılan `(i2f(ch) * (1/255.0)) < / <= / > / >= K` karşılaştırmaları, `0..255` tam sayı alanında bit-bit eşdeğer eşik (`thresh`) hesaplanarak doğrudan `IR_ILT_S` / `IR_IGE_S` karşılaştırmasına katlandı. `sprite.mtk` içindeki `if (c.a < 0.01) discard;` testi `f32x4.mul + f32x4.lt` yerine doğrudan `i32x4.lt_s` (`thresh = 3`) ile yapıldı.
   - Test: `tests/test_matoki_back.c` içine `matoki_alpha_cmp_int_fold` eklendi.

---

### 2. Önce / Sonra Karşılaştırma Tabloları

- **Ortam**: `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz`, Node `v22.21.1` (`linux x64`)
- **Doğrulama**: `12.345` rastgele quad (`49.380` piksel, 4'ün katı olmayan kuyruk boyutu dahil) + bozuk/sınır girdi testleri
- **Ölçüm**: `1.000.000` quad (`250` batch × `4.000` quad), `60` batch ısınma + `21` tekrar (nöbetleşe) medyan (`./tools/host/run_matoki_vs_wat.sh`)

#### Tablo 1 — Önce (M12_A Başlangıcı) vs Sonra (P1–P5 Tamamlandıktan Sonra) Özeti

| Shader | IR Komutu (Önce → Sonra) | `mdis` İç Döngü Komutu (Önce → Sonra vs Elle WAT) | Wasm Boyutu (Önce → Sonra vs Elle WAT) | Matoki `Mquad/s` (Önce → Sonra) | Elle WAT `Mquad/s` | Matoki / Elle WAT Verimi (Önce → Sonra) |
|---|---:|---:|---:|---:|---:|---:|
| `vcolor` | `16→16` → **`16→16`** | `47` → **`135 (4x=34/q)`** (WAT: `136 (4x=34/q)`) | `299 B` → **`655 B`** (WAT: `709 B`) | `364.2` → **`472.3`** | `463.4` | `%78.2` → **`%101.9`** |
| `sprite` | `57→54` → **`61→54`** | `119` → **`111`** (WAT: `110`) | `589 B` → **`573 B`** (WAT: `677 B`) | `75.9` → **`91.5`** | `91.3` | `%89.7` → **`%100.2`** |
| `plasma` | `456→332` → **`273→197`** | `683` → **`398`** (WAT: `395`) | `2266 B` → **`1419 B`** (WAT: `1452 B`) | `15.2` → **`22.0`** | `21.8` | `%75.6` → **`%101.0`** |
| `lit3d` | `266→238` → **`258→224`** | `670` → **`571`** (WAT: `545`) | `2356 B` → **`1817 B`** (WAT: `1938 B`) | `16.3` → **`20.5`** | `21.0` | `%84.2` → **`%97.5`** |

- Dört shader genelinde **Matoki / Elle WAT aritmetik ortalama verimi `%81.9`'dan `%100.2`'ye** yükseldi.
- `plasma`'da iç döngü komutu `683 → 398` (`-285` komut, `-%41.7`), verim `%75.6 → %101.0` (**`+1.45×` hızlanma**).
- `lit3d`'de iç döngü komutu `670 → 571` (`-99` komut, `-%14.8`), verim `%84.2 → %97.5` (**`+1.26×` hızlanma**).
- `vcolor`'da quad başına iç döngü komutu `47 → 33.75`, verim `%78.2 → %101.9` (**`+1.30×` hızlanma**).
- `sprite`'ta iç döngü komutu `119 → 111`, verim `%89.7 → %100.2` (**`+1.21×` hızlanma**).

#### Tablo 2 — Adım Adım (`P2 → P1 → P3 → P4 → P5`) Verim ve İç Döngü Komutu İlerlemesi

| Adım (Commit) | `vcolor` İç Döngü / Verim | `sprite` İç Döngü / Verim | `plasma` İç Döngü / Verim | `lit3d` İç Döngü / Verim |
|---|---|---|---|---|
| **Başlangıç** | `47` (`%78.2`) | `119` (`%89.7`) | `683` (`%75.6`) | `670` (`%84.2`) |
| **P2** (`d0b4a99`) | `47` (`%77.5`) | `119` (`%91.8`) | **`400` (`%99.1`)** | `680` (`%83.8`) |
| **P1** (`b0cf6a9`) | `47` (`%76.9`) | `119` (`%91.0`) | `400` (`%99.9`) | **`603` (`%91.3`)** |
| **P3** (`86a5340`) | `47` (`%77.1`) | **`113` (`%97.1`)** | `398` (`%102.1`) | **`571` (`%97.1`)** |
| **P4** (`68091b2`) | **`135 (4x=34/q)` (`%100.5`)** | `113` (`%95.4`) | `398` (`%101.1`) | `571` (`%97.4`) |
| **P5** (`5b49cf0`) | **`135 (4x=34/q)` (`%101.9`)** | **`111` (`%100.2`)** | **`398` (`%101.0`)** | **`571` (`%97.5`)** |

#### Tablo 3 — Doğruluk Karşılaştırması (`12.345` Rastgele Quad = `49.380` Piksel + Bozuk Girdi Testleri)

| Shader | Quad Sayısı | Maske Eşitliği | Tam Bit Eşitliği (%) | Maks Mutlak Fark | Maks Fark (`LSB = 1/255`) | Ort. Fark (`LSB`) | Bozuk Girdi Testi | Durum |
|---|---:|---|---:|---:|---:|---:|---|---|
| `vcolor` | 12345 | Bit-bit aynı | **%100.0** | `0.00e+0` | `0.00e+0 LSB` | `0.00e+0 LSB` | GEÇTİ | **GEÇTİ** |
| `sprite` | 12345 | Bit-bit aynı | **%100.0** | `0.00e+0` | `0.00e+0 LSB` | `0.00e+0 LSB` | GEÇTİ | **GEÇTİ** |
| `plasma` | 12345 | Bit-bit aynı | **%81.1** | `2.38e-7` | `6.08e-5 LSB` | `4.93e-6 LSB` | GEÇTİ | **GEÇTİ** |
| `lit3d` | 12345 | Bit-bit aynı | **%100.0** | `0.00e+0` | `0.00e+0 LSB` | `0.00e+0 LSB` | GEÇTİ | **GEÇTİ** |

- P3 ve P5 sonrasında `vcolor`, `sprite` ve `lit3d` çıktıları elle yazılmış WAT ile **%100.0 bit-bit aynıdır** (`0.00 LSB` fark).
- `plasma` çıktısı elle yazılmış WAT ile **%81.1 bit-bit aynıdır** ve maksimum fark `6.08e-5 LSB` (`1 LSB` sınırının **16.000 kat altında**) düzeyindedir.
- Ertelenen madde yoktur; P1–P5 önerilerinin tamamı uygulanmış, doğrulanmış ve ayrı commit'lerle kaydedilmiştir.


### İnceleme (Claude)
- Main ile (M13_B + M12_B/C dahil) birleşik `make check` 168/168.
- `run_matoki_vs_wat.sh` yeniden koşuldu: vcolor %98.4, sprite %101.2, plasma %100.4, lit3d %97.8. İddia
  ölçüm gürültüsü içinde doğrulandı. vcolor/sprite/lit3d elle yazılmış WAT ile bit-bit aynı.
- Tarayıcı (Chrome) karşılaştırması, dt=0 ile sabit kare: scene3d PBR, scene3d lit ve Kuyu (dungeon3d) için JIT ile
  C referans arasındaki tek fark sağ üstteki HUD mod etiketi (52×7 piksel). 3D görüntü birebir aynı.
- Kare süresi (8T, JIT): scene3d PBR + gölge 7.1 → 6.38 ms; 1T 17.2 → 15.7 ms.
