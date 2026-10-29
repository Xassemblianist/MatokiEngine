# T1 — Matoki diferansiyel fuzz testi (ir_eval kahin)

DURUM: DONE

## Amaç
Rastgele ama geçerli Matoki programları (pixel + compute) üreten tohumlu üreteç; her programı `ir_eval` (kahin) ile wasm
JIT (SIMD + relaxed build) karşısında çalıştır, çıktıları ulp toleransıyla karşılaştır. Fark bulunca en küçük programa
indirgeyip `tests/fuzz_cases/` altına regresyon olarak kaydet. Bulunan derleyici hatalarını düzelt (src/matoki/*).
`make fuzz N=…` hedefi; make check içinde küçük sabit tohumlu sürüm (< 10 s).
Fikir kaynağı: SamurayBalgami/Matoki ir_eval kahin yaklaşımı (izinli).

## Dosyalar
`tests/test_matoki_fuzz.c` (yeni), `tests/fuzz_cases/*` (yeni), `src/matoki/*` (yalnız hata düzeltme), `Makefile`

## Kabul kriterleri
- [x] 100 000 program fuzz çalıştırması; bulunan/düzeltilen hata sayısı Notlar'da
- [x] make check temiz

## Notlar
### Yapılanlar
1. **Tohumlu Rastgele IR Üreteci (`tests/test_matoki_fuzz.c`)**:
   - Hem piksel (varying, uniform, doku, blend füzyonu `REPLACE/ALPHA/ADD/NONE`, kontrol akışı `IF/ELSE/END`) hem de compute (buffer load/store, atomic_add, global_id, if/else) gövdelerini geçerli SSA kurallarına uygun biçimde rastgele üreten üreteç geliştirildi.
   - Her program bağımsız tohumlandı (`seed ^ (p * PHI)`), böylece istenen program `FUZZ_START` ile doğrudan izole şekilde yeniden üretilebilir hale getirildi.
2. **Hızlı Toplu Yürütme ve Diferansiyel Doğrulama**:
   - `build/fuzz_runner.mjs` koşucusu test başlangıcında otomatik oluşturulur. 1.000'lik ikili paketler halinde Node/V8 JIT derlemesi yapılarak `ir_eval` / `ir_eval_compute` kahin sonuçlarıyla `ulp <= 4` toleransında ve bit-bit framebuffer/buffer doğrulaması yapıldı.
3. **`Makefile` Hedefleri**:
   - `make fuzz N=...` eklendi (varsayılan N=100.000).
   - `make check` için 500 programlık hızlı sürüm bağlandı (< 0.3 saniyede tamamlanır, < 10 s kriteri rahatça karşılanır).

### Bulunan ve Düzeltilen Derleyici Hataları (2 adet)
1. **`src/matoki/opt.c` (IR_SELECT Varying Çökmesi)**:
   - `ir_opt_constfold`: `if (cond)` bloğu içinde `select(cond, b, c)` komutu görüldüğünde koşulun varying olup olmadığına bakılmaksızın doğrudan `b` koluna katlanıyordu. SIMD yürütmede aktif olmayan lane'ler için `c` değerine ihtiyaç duyulduğundan katlama `!(f->insts[in->a].flags & IR_FLAG_VARYING)` şartına bağlandı.
   - Regresyon testi: `tests/fuzz_cases/case_00001_opt_select_varying.ir`.
2. **`src/matoki/ir.c` (Compute Shader'da IR_DISCARD Doğrulaması)**:
   - `ir_verify`: Compute shader içinde `IR_DISCARD` kullanımını reddetmiyordu. `if (f->is_compute)` kontrolü eklenerek doğrulama hatası üretmesi sağlandı.
   - Regresyon testi: `tests/fuzz_cases/case_00002_compute_discard.ir`.

### Ölçümler ve Sistem Yükü
- **Fuzz 100.000 Program**:
  - `make fuzz N=100000`: 100.000 program başarıyla tamamlandı (hata: 0).
  - Süre: `real 1m18.183s`, `user 1m24.198s`, `sys 0m21.470s` (~1.280 program/saniye derleme + JIT + kahin doğrulama).
  - Sistem yükü (`uptime`): `21:59:15 up 8:27, 1 user, load average: 3,00, 2,24, 3,47`.
- **Birim Testler (`make check`)**:
  - 311 test geçti, 0 kaldı. `check: tamam`.
- **GPU Doğrulaması (`make gpucheck GPU=1`)**:
  - Headless Chrome + Vulkan RTX 2060 üzerinde tüm kontroller (üçgen, küp, WGSL) sıfır GPU hatasıyla geçti (`build/webgpu_shot.png` görsel olarak doğrulandı).
