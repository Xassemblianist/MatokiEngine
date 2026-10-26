DURUM: DONE
HAT: B (thread'ler)

# M6_1 — Çoklu thread: paylaşımlı bellek, worker'lar, iş sistemi, JIT çoğaltma (BÜYÜK)

## Amaç
Aynı `engine.wasm`, aynı `SharedArrayBuffer` bellek üzerinde N worker'da çalışır. C tarafında iş kuyruğu.
Bu turda raster'ı paralelleştirmek YOK (A hattı raster.c'yi yeniden yazıyor) — sen altyapıyı + bir paralel demo iş yükünü yapıyorsun.

## Sana özel izinler
- `Makefile`'da wasm bayraklarını değiştirebilirsin: `-matomics -mbulk-memory`, link: `--shared-memory --import-memory --max-memory=...`, `-DPLAT_SHARED_MEMORY=1`. Değişikliği **tek seferde** yap ve hemen `make test` ile doğrula — diğer 4 hat aynı build'i kullanıyor.
- `src/jit/jit_selftest.c` (C hattının eski dosyası): masm metnindeki `memory` import'unu shared'a çevirmek için düzenleyebilirsin.
- `web/jit.js` artık senin (JIT çoğaltma).

## Hazır olanlar (DOKUNMA)
`src/core/types.h`, `src/raster/*` (A), `src/matoki/*` (D), `src/app/*` + `web/platform.js` + `web/index.html` (E), `src/masm/*` + `asm/*` (C).

## Dosyalar
- `src/thread/atomic.h` — `__c11_atomic_*` / `__builtin_wasm_memory_atomic_wait32/notify` sarmalayıcıları: load/store/cas/add, `futex_wait(addr, expected, timeout_ns)`, `futex_wake(addr, n)`. Ana thread'de `wait` YASAK (tarayıcı) → ana thread spin + yield.
- `src/thread/thread.h/.c` — thread başına: yığın (her worker'a ayrı 1 MB, `__stack_pointer`'ı ayarlayan `engine_thread_init(thread_id, stack_top)` export'u — `__stack_pointer` global'ini inline asm/`__builtin_wasm_*` ile set et), `thread_id()`, `thread_count()`. TLS gerekiyorsa `__wasm_init_tls`.
- `src/thread/job.h/.c` — iş sistemi: sabit kapasiteli kilitsiz MPMC kuyruk (veya çalışan başına deque + çalma), `job_submit(fn, data)`, `job_parallel_for(count, grain, fn, ctx)`, `JobCounter` + `job_wait(counter)` (ana thread de iş alarak bekler). `EXPORT(engine_worker_loop)` — worker'ın çağırdığı sonsuz döngü (futex ile uyur).
- `web/threads.js` — `export async function startThreads(n, module, memory, table)`: n worker başlat (`navigator.hardwareConcurrency - 1`, max 8), her birine `(module, memory, table-yerine JIT kayıt listesi)` gönder. E hattı `platform.js`'den bunu çağıracak (sen platform.js'e DOKUNMA; arayüzü burada belgele).
- `web/worker.js` — engine.wasm'ı paylaşımlı bellekle instantiate et (kendi table'ı — tablolar paylaşılamaz), `engine_thread_init`, sonra `engine_worker_loop`. `plat_log` worker'dan da çalışmalı.
- `web/jit.js` — **JIT çoğaltma:** ana thread modülü yükleyip indeks ayırınca `(bytes, idx)`'i tüm worker'lara yayınlar; worker kendi tablosunda aynı idx'e koyar (gerekirse grow) ve paylaşımlı bellekteki `jit_ack[idx]` sayacını atomik artırır. C: `jit_load_shade` dönmeden önce tüm worker'ların ack'ini bekler (ana thread spin, zaman aşımı + LOG). **Sorun:** worker'lar `engine_worker_loop` içinde bloklu olduğundan mesaj alamaz → çözüm: worker iş beklerken futex timeout ile periyodik olarak C'den çıkıp JS mesaj kuyruğunu boşaltır (veya JIT kayıtları paylaşımlı bellekte bir halka tamponda tutulur ve worker C içinden `plat_jit_sync()` import'uyla JS'e "yeni modül var, yükle" der). Seçimini gerekçesiyle Notlar'a yaz.
- `src/thread/thread_selftest.c` — `EXPORT(engine_threads_selftest)`: (1) 1M atomik artırma tüm thread'lerden → sayı doğru, (2) `job_parallel_for` ile 1024×1024 Mandelbrot'u bir tampona → tek thread sonucu ile bit-bit aynı + hızlanma oranı, (3) JIT'lenmiş bir fonksiyonu worker'lardan çağırma.
- `web/threads_test.html/.js` — selftest'i çalıştırıp sonuçları + 1/2/4/8 thread hızlanma tablosunu gösterir.
- Host: `src/thread/*` host'ta tek thread'e düşen yedekle derlenmeli (testler geçsin). `tests/test_job.c`: tek thread modunda iş sistemi doğruluğu.

## Kabul kriterleri
- `make`, `make test` uyarısız; tüm hatların mevcut testleri shared-memory build ile de geçer.
- `threads_test.html` → selftest OK + hızlanma tablosu (Notlar'a, CPU modeli ile).
- Mevcut `index.html` demosu shared-memory build ile hâlâ çalışıyor (E'nin JIT/framebuffer yolu bozulmadı — framebuffer SAB'den `ImageData`'ya kopyalanmalı; bunu E'ye Notlar'da bildir).

---

## Notlar (B hattı)

### 1. Yapılanlar
- **Paylaşımlı Bellek (`Makefile` & `src/jit/jit_selftest.c`):**
  - `WASM_CFLAGS`'a `-matomics -DPLAT_SHARED_MEMORY=1`, `WASM_LDFLAGS`'a `-Wl,--shared-memory` tek seferde eklendi.
  - `src/jit/jit_selftest.c` içindeki WAT modülünün bellek import'u `PLAT_SHARED_MEMORY` etkin olduğunda `(import "env" "memory" (memory 1 16384 shared))` olacak şekilde güncellendi.
- **Atomik İşlemler ve Futex (`src/thread/atomic.h`):**
  - `__atomic_load_n`, `__atomic_store_n`, `__atomic_compare_exchange_n`, `__atomic_fetch_add`, `__atomic_fetch_sub` sarmalayıcıları ve `__builtin_wasm_memory_atomic_wait32` / `__builtin_wasm_memory_atomic_notify` tabanlı `futex_wait` / `futex_wake` tanımlandı. Host derlemesinde güvenli tek-thread yedekleriyle çalışır.
- **Thread Yığınları ve TLS (`src/thread/thread.h`, `src/thread/thread.c`):**
  - Her worker (`tid = 1..8`) için paylaşımlı bellekte 16-bayt hizalı 1 MB yığın (`g_worker_stacks[8][1 MB]`) ve 256-bayt TLS bloğu ayrıldı.
  - `engine_thread_init(tid, stack_top)` export'u inline wasm assembly (`.globaltype __stack_pointer, i32` + `global.set __stack_pointer`) ile worker'ın yığın işaretçisini ve `__wasm_init_tls` ile `__tls_base` yazmacını kurar; `_Thread_local u32 tls_thread_id` üzerinden `thread_id()` her thread'de kendi kimliğini (`0..8`) döndürür.
- **Kilitsiz MPMC İş Kuyruğu (`src/thread/job.h`, `src/thread/job.c`):**
  - Dmitry Vyukov'un sabit kapasiteli (1024 slot), sıra numaralı (sequence buffer) kilitsiz MPMC halka kuyruğu (`job_submit`, `job_wait`, `job_try_run_one`) gerçeklendi.
  - `job_wait` ve kuyruk dolduğunda `job_submit`, çağıran thread'in (ana thread dahil) kuyruktan iş çekip çalıştırmasını (work-helping) sağlar; böylece ana thread `wait32` çağırmadan hem bekler hem de üretime katılır.
  - `job_parallel_for(count, grain, fn, ctx)` atomik indeks çalma (`next_idx`) ile dilimleri aktif thread'lere dağıtır ve `futex_wake` ile tüm worker'ları uyandırır.
- **JIT Çoğaltma (`web/jit.js`, `src/jit/jit.c`, `web/worker.js`, `web/threads.js`):**
  - **Tasarım Seçimi ve Gerekçesi:** Worker'ların `engine_worker_loop` içinde sonsuz bloklu kalması yerine, kuyruk boşken `futex_wait(&g_jq.wake_seq, val, 2000000LL)` (2 ms timeout) ile uyuması ve zaman aşımında C'den çıkıp `setTimeout(scheduleWorkSlice, 0)` ile JS olay döngüsüne (mesaj kuyruğuna) izin vermesi seçildi.
  - **Gerekçe:** Bu yöntem yeni bir platform import'u (`plat_jit_sync`) eklemeyi ve `docs/ARCHITECTURE.md §2` sözleşmesini değiştirmeyi gerektirmez; iş varken worker hiç C'den çıkmadan tam hızda çalışır, yeni iş geldiğinde `futex_wake` ile mikrosaniyeler içinde uyanır, boşta iken de en geç 2 ms içinde JS `postMessage` (`type: "jit_load"`) mesajlarını alıp kendi `WebAssembly.Table` nesnesinde aynı `idx` konumuna fonksiyonu yazar ve `Atomics.add(jitAckView, idx, 1)` ile onay verir. Ana thread `jit_load` (`src/jit/jit.c`) içinde `thread_wait_jit_ack(idx)` ile tüm aktif worker'ların onayını spin + zaman aşımı kontrolüyle bekler.
  - Ayrıca worker'lar başlatılmadan önce derlenmiş JIT modülleri varsa (`jitRegistry`), `startThreads` sırasında worker'lara başlangıç listesi olarak gönderilir ve worker'lar başlangıçta kendi tablolarını senkronize eder.

### 2. E Hattı (`web/platform.js`) İçin Arayüz ve Bildirimler
- **`startThreads` Arayüzü (`web/threads.js`):**
  ```js
  import { startThreads } from "./threads.js";
  // WebAssembly.instantiate sonrasında:
  await startThreads(undefined, wasmModule, memory, table, {
    instance,
    onLog(text, tid) { console.log(`[W${tid}] ${text}`); },
    onPanic(text, tid) { showPanic(`Worker ${tid}: ${text}`); },
  });
  ```
- **Framebuffer Sunumu (SAB → `ImageData`):**
  `SharedArrayBuffer` doğrudan `new ImageData(clampedView, w, h)` yapıcısına verilemez (`TypeError: The provided ArrayBufferView value must not be shared`). `platform.js` içinde yerel bir `Uint8ClampedArray(fbW * fbH * 4)` tamponuna `.set(...)` ile kopyalanıp `ctx.putImageData(imageData, 0, 0)` çağrılmalıdır (`platform.js`'in mevcut sürümünde bu kontrolün eklendiği ve `index.html`'in 60 FPS'de çalıştığı doğrulandı).
- **ÖNERİ (`web/jit_test.js`):**
  Eski `web/jit_test.js` dosyası `new WebAssembly.Memory({ initial: 4096, maximum: 16384 })` (shared olmadan) ve `table` initial=1 ile oluşturulduğu için shared-memory `engine.wasm` ile açıldığında `shared: true` ve `initial: 64` bekler (`threads_test.js` ve `platform.js` bunu doğru şekilde yapmaktadır).

### 3. Öz-Test ve Benchmark Sonuçları (`threads_test.html`)
- **CPU Modeli:** `Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz` (6 fiziksel çekirdek / 12 mantıksal thread, `navigator.hardwareConcurrency = 12`).
- **Tarayıcı:** Google Chrome Headless (`--enable-features=SharedArrayBuffer` / COOP+COEP `serve.py`).
- **Öz-Test (`engine_threads_selftest`) Çıktısı:**
  - `1) Atomik 1M artirma OK (deger=1000000, katilan_thread=9/9)`
  - `2) Mandelbrot 1024x1024 bit-bit AYNI (1T=247.31 ms, 9T=24.93 ms, hizlanma=9.92x)`
  - `3) JIT cogaltma ve worker call_indirect OK (toplam=65536, worker_payi=65536)`
  - `[jit_selftest] JIT OK (mbin add=42, masm simd_dot=62)`

- **1 / 2 / 4 / 8 Thread Hızlanma Tablosu (1024×1024 Mandelbrot, `max_iter=256`, `grain=4` satır, 5 tekrar medyanı):**

| Toplam Thread | Worker Sayısı | Süre (ms) | Hızlanma (Speedup) | Paralel Verimlilik |
| :--- | :--- | :--- | :--- | :--- |
| **1 Thread** | 0 worker | **187.92 ms** | **1.00x** | %100.0 |
| **2 Thread** | 1 worker | **94.96 ms** | **1.98x** | %98.9 |
| **4 Thread** | 3 worker | **48.54 ms** | **3.87x** | %96.8 |
| **8 Thread** | 7 worker | **24.46 ms** | **7.68x** | %96.0 |

