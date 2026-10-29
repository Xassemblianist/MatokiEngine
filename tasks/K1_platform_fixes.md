# K1 — Küçük platform düzeltmeleri (ARCH_V2_REVIEW §9 madde 8)

Durum: DONE

## Amaç
1. `web/platform.js` worker sayısı formülü: `Math.max(hc-1, 7)` her zaman 7+ açıyor → `min(hc-1, THREAD_MAX_WORKERS)`, en az 1;
   sessiz yutulan hataları (`platform.js` ~150. satır civarı) konsola yaz.
2. **Tek iş parçacıklı yedek derleme** (H10): SharedArrayBuffer/COOP-COEP yoksa çalışan `build/engine_st.wasm`
   (atomics/shared memory yok, job sistemi seri); yükleyici `web/caps.js` sonucuna göre seçer (ARCHITECTURE §2 kuralı).
3. Girdi olay halkası: JS → wasm kare damgalı SPSC olay halkası (tuş/fare olayları kaybolmasın; ileride rollback girdisi).
   Mevcut `InputState` korunur, halka ek olarak gelir.

## Dosyalar
- `web/platform.js`, `web/caps.js`, `web/index.html`, `Makefile`, `src/plat/*`, `src/thread/*` (yalnız ST derleme koşulları),
  `docs/ARCHITECTURE.md` §2, `tests/test_input_ring.c` (yeni)
- Not: `web/engine_st.wasm` derleme çıktısının repo kirliliği yaratmaması için `.gitignore` dosyasına eklendi.

## Kabul kriterleri
- [x] `make check` hatasız; üç wasm (`engine`, `engine_relaxed`, `engine_st`) `wasm-validate` geçer
- [x] COOP/COEP'siz sunucuda sayfa ST yoluyla açılır (başsız Chrome ile doğrula, Notlar'a yaz)

## Notlar
1. **Worker sayısı ve hata yakalama:**
   - `web/platform.js` içindeki `Math.min(Math.max(hc - 1, 7), 8)` formülü `Math.max(1, Math.min(hc - 1, THREAD_MAX_WORKERS))` (THREAD_MAX_WORKERS=8) olarak düzeltildi.
   - Worker havuzu başlatma, filedrop ve audio modül yüklemelerindeki sessiz yutulan `catch (_e) {}` blokları `console.warn` ve `console.log` ile görünür kılındı.
   - COOP/COEP olmayan tarayıcılarda `SharedArrayBuffer` global nesnesi hiç tanımlı olmadığında ortaya çıkan `ReferenceError: SharedArrayBuffer is not defined` hatasını önlemek için `typeof SharedArrayBuffer !== "undefined"` korumaları eklendi.

2. **Tek iş parçacıklı yedek derleme (`build/engine_st.wasm`):**
   - `Makefile`'a `WASM_ST_CFLAGS` (`-DPLAT_SHARED_MEMORY=0`, `-matomics` yok) ve `WASM_ST_LDFLAGS` (`--shared-memory` yok) kuralları eklendi; `build/wasm_st/` altında derlenerek `build/engine_st.wasm` ve `web/engine_st.wasm` üretildi.
   - `src/thread/atomic.h` ve `src/thread/thread.c` tek iş parçacıklı wasm derlemesinde derleyici yerleşikleri ve döngüsüz yürütme ile sorunsuz linklendi; `job_parallel_for` ST kipinde tüm döngüleri tek iş parçacığında seri yürüttü.
   - `web/caps.js` `selectEngineWasmUrl` fonksiyonu SAB veya threads eksik olduğunda (veya URL'de `?st=1` / `?threads=0` verildiğinde) doğrudan `./engine_st.wasm` seçecek şekilde güncellendi.
   - `docs/ARCHITECTURE.md` §2 güncellenerek yükleyici seçim kuralına H10 ST yedeği eklendi.

3. **Girdi olay halkası (Input Event Ring):**
   - `src/plat/input_ring.h` ve `src/plat/input_ring.c` eklendi; 64-baytlık `InputEvent` yapısı (`type`, `frame`, `time_ms`, `mouse_x/y`, `mouse_dx/dy`, `key`, `buttons`, `wheel`, `char_code`, `modifiers`) ve 256 kapasiteli SPSC `InputRing` yapısı oluşturuldu.
   - `engine_input_ring` export'u wasm'a açıldı; `docs/ARCHITECTURE.md` §2'deki JS sözleşmesine işlendi.
   - `tests/test_input_ring.c` yazılarak temel push/pop, kapasite sınırı, düşürme sayacı (`dropped`), monoton indeks sarma (`wraparound`) ve temizleme (`clear`) senaryoları doğrulandı (4/4 test geçti).
   - `web/platform.js` girdi dinleyicileri mevcut `InputState` yapısını bozmadan paralel olarak tüm tuş, fare, tekerlek ve karakter olaylarını `InputRing`'e kare ve zaman damgalı olarak yazacak şekilde bağlandı.

4. **Kabul kriterleri ve test sonuçları:**
   - Makine yükü (`uptime`): `17:23:25 up 3:51, 1 user, load average: 3.51, 4.34, 4.63`.
   - `make check`: 194 host testi geçti, 0 kaldı; kural denetimi (src/ altında libc include yok) hatasız tamamlandı.
   - `wasm-validate --enable-all`: Üç wasm ikilisi de (`build/engine.wasm`, `build/engine_relaxed.wasm`, `build/engine_st.wasm`) hatasız geçti.
   - Başsız Chrome doğrulaması (`build/verify_st_chrome.mjs`):
     - COOP/COEP başlıkları olmayan HTTP sunucusunda `google-chrome --headless=new` ile test edildi.
     - `crossOriginIsolated: false`, `hasSAB: false`, `wasmUrl: "./engine_st.wasm"`, `isSt: true`, `workerRender: false`, `panicVisible: false` gözlendi.
     - ST yoluyla sayfa başarıyla açıldı, Matoki JIT shader'larını derledi ve 60 FPS (16.4 ms/frame, 1T) ile render etti.
     - Karşılaştırma amaçlı COOP/COEP'li sunucu testinde (`build/verify_mt_chrome.mjs`) ise `crossOriginIsolated: true`, `wasmUrl: "./engine_relaxed.wasm"`, `workerRender: true` olarak çoklu iş parçacığı yolunun korunduğu teyit edildi.
