DURUM: DONE
HAT: B (Matoki ön uç)

# M4_1 — Matoki ön ucu: lexer → parser → sema → IR'a indirme (BÜYÜK)

## Oku
`docs/MATOKI.md` (dil), `src/matoki/ir.h` (hedef IR — DOKUNMA), `src/raster/shade_abi.h`.

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/*`, `src/matoki/ir.h`, `src/matoki/codegen.h`, `src/raster/shade_abi.h`, `tests/test.h`.
`ir.c`, `ir_eval.c`, `opt.c`, `codegen.c` D hattının — paralel yazılıyor. Sen IR'ı yalnızca `ir.h` kurucularıyla üretirsin.
D bitmeden linklemen gerekirse: testlerini `ir_verify`/`ir_eval` yokken derlenebilir tut veya D'nin dosyaları gelene kadar bekle; ASLA onların dosyalarını yazma.

## Dosyalar
- `src/matoki/lexer.h/.c` — token'lar, satır/sütun, sayı literalleri (`1`, `1.0`, `.5`, `1e-3`, `0x1F`), yorumlar, hata token'ı.
- `src/matoki/ast.h` — AST düğümleri (arena'da), ifade + deyim + bildirim.
- `src/matoki/parser.h/.c` — elle yazılmış özyinelemeli iniş + Pratt (öncelik tırmanma) ifadeler. Hata kurtarma: `;` / `}`'ye kadar atla, en fazla 20 hata.
- `src/matoki/sema.h/.c` — isim çözümleme (kapsamlar), tip denetimi, örtük dönüşüm yok, swizzle doğrulama, yerleşik fonksiyon imzaları (aşırı yükleme: `f32/vec2/vec3/vec4` için `abs min max clamp mix step smoothstep floor fract sqrt rsqrt dot cross length normalize sin cos pow exp2 log2`), **uniform/varying çıkarımı**, özyineleme tespiti, `frag` giriş noktası kontrolü.
- `src/matoki/lower.h/.c` — AST → IR:
  - Vektörleri skalerlere böl (vec3 = 3 IR değeri). Swizzle = değer kimliklerini yeniden dizmek (sıfır maliyet).
  - Yardımcı fonksiyonlar inline edilir.
  - `let`/`var` SSA: değişken → güncel IR kimliği haritası. `if/else` içinde atanan değişkenler `IR_IF_END` sonrası `ir_select(cond, then_val, else_val)` ile birleşir (iç içe if'ler dahil).
  - `sin/cos/exp2/log2/pow` → IR'da polinom açılımı (katsayıları `src/core/math.c`'den al, aynı hassasiyet). `normalize` → `x * (1/sqrt(dot))`.
  - `sample(tex, uv)` → `ir_emit_sample`. Örnekleyici bayrakları: v0'da uniform bildiriminde ek açıklama: `uniform tex: texture2d @bilinear @clamp;` (MATOKI.md'ye bu sözdizimini sen ekle — tek izinli doküman değişikliği).
  - `discard` → aktif bölge maskesiyle `ir_discard`. `return` → `ir_output` ×4.
  - Uniform yerleşimi: bildirim sırasıyla, her uniform 16 byte hizalı başlar (f32/i32 4 byte, vec4 16, mat4 64); dokular ayrı slot sayacı.
- `src/matoki/matoki.h/.c` — genel API:
  ```c
  typedef struct { char name[32]; u32 kind; /* 0 uniform,1 varying,2 texture */
                   u32 type; u32 offset_or_index; u32 components; } MtkSymbol;
  typedef struct { MtkSymbol syms[64]; u32 sym_count; u32 uniform_bytes; u32 varying_count; u32 texture_count; } MtkLayout;
  typedef struct { char text[4096]; u32 len; u32 count; } MtkDiag;   // "dosya:satır:sütun: hata: ..." + kaynak satırı + ^
  b32 mtk_compile_ir(const char* src, u32 len, const char* filename, Arena* a, IrFunc* out, MtkLayout* layout, MtkDiag* diag);
  /* mtk_compile_ir + ir_optimize + mtk_codegen → wasm byte'ları */
  u32 mtk_compile_wasm(const char* src, u32 len, const char* filename, const MtkCodegenOpts* o, Arena* a,
                       u8* out, u32 out_cap, MtkLayout* layout, MtkDiag* diag);
  ```
- `tools/host/tool_matc.c` — `build/host/matc girdi.mtk [-o çıktı.wasm] [--ir] [--ir-opt] [--layout]`.
- `shaders/*.mtk` — en az: `solid.mtk`, `vcolor.mtk`, `sprite.mtk`, `plasma.mtk` (sin/cos yoğun), `branchy.mtk` (iç içe varying if + discard).
- `tests/test_matoki_front.c` — lexer token akışları; parser hata mesajları (konum doğru mu); sema hataları (tip uyuşmazlığı, bilinmeyen isim, yanlış swizzle, özyineleme); lowering: üretilen IR `ir_verify`'dan geçer; **anlamsal testler `ir_eval` ile** (D'nin yorumlayıcısı): sabit bir ShadeBatch üzerinde shader çıktısı elle hesaplanan değere eşit.

## Kabul kriterleri
- `make`, `make test` uyarısız. `matc shaders/*.mtk --ir` hepsi için okunur IR basar.
- 25+ ayrı hata durumu testi; her hata mesajı doğru satır:sütun gösterir.
- `plasma.mtk` için `ir_eval` sonucu, aynı formülün `src/core/math.c` ile C hesaplamasına 1e-5 içinde eşit.
- Notlar: her shader için IR komut sayısı (opt öncesi / sonrası — opt D'den gelince).

## Notlar (B hattı)
- **Yapılanlar:**
  - `src/matoki/lexer.h/.c`: Yorumları (`//` ve `/* */`), anahtar kelimeleri, tamsayı/hex (`0x1F`) ve ondalık/bilimsel kayan nokta (`1.0`, `.5`, `1e-3`) literallerini, `..` aralık operatörünü ve `@` nitelik önekini ayrıştıran, libc bağımsız lexer.
  - `src/matoki/ast.h` ve `src/matoki/parser.h/.c`: Arena tabanlı AST; özyinelemeli iniş (bildirim/deyim) ve Pratt öncelik tırmanma (ifade) çözümleyicisi; `;`/`}` sınırlarında hata kurtarma ile ilk hatada durmadan en fazla 20 hata toplama.
  - `src/matoki/sema.h/.c`: Kapsamlı isim çözümleme, katı tip denetimi (örtük dönüşüm yok), swizzle doğrulama (uzunluk, bileşen sınırı, küme karışımı, sol taraf tekrarlı bileşen kontrolü), yerleşik fonksiyon aşırı yüklemeleri, uniform/varying tekdüzelik çıkarımı, 3-renk DFS ile doğrudan ve dolaylı özyineleme tespiti, `fn frag() -> rgba` giriş noktası doğrulaması ve 16-byte hizalı `MtkLayout` hesabı.
  - `src/matoki/lower.h/.c`: Vektör/matris SoA skalerleştirme, sıfır maliyetli swizzle yeniden eşlemesi, yardımcı fonksiyon inline etme, sabit sınır `for` döngüsü açma, `if/else` dallarında değişen değişkenleri `IR_IF_END` sonrası `ir_select` ile birleştirme, aktif bölge maskesiyle `ir_discard`, `src/core/math.c` ile birebir aynı Cody-Waite + minimax polinom katsayılarıyla `sin/cos/exp2/log2/pow` açılımı.
  - `src/matoki/matoki.h/.c` ve `tools/host/tool_matc.c`: `mtk_compile_ir` ve `mtk_compile_wasm` genel API'si ve çoklu girdi destekleyen `build/host/matc` komut satırı derleyicisi.
  - `docs/MATOKI.md`: `uniform tex: texture2d @bilinear @clamp;` sözdizimi eklendi.
  - `tests/test_matoki_front.c`: Lexer akışı, 41 ayrı hata durumu + çoklu hata kurtarma (her birinde tam `dosya:satır:sütun` doğrulaması), `ir_verify` ve `ir_eval` ile anlamsal doğrulama (`plasma.mtk` C referansıyla `< 1e-5` içinde birebir uyumlu).
- **Shader IR Komut Sayıları (`ir_optimize` öncesi / sonrası):**

| Shader | Opt Öncesi (Ham IR) | Opt Sonrası (`ir_optimize`) | Azalma |
|---|---:|---:|---:|
| `shaders/solid.mtk` | 8 komut | 8 komut | %0.0 |
| `shaders/vcolor.mtk` | 16 komut | 16 komut | %0.0 |
| `shaders/sprite.mtk` | 57 komut | 56 komut | %1.8 |
| `shaders/branchy.mtk` | 67 komut | 38 komut | %43.3 |
| `shaders/plasma.mtk` | 456 komut | 332 komut | %27.2 |

