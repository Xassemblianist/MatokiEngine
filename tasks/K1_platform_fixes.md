# K1 — Küçük platform düzeltmeleri (ARCH_V2_REVIEW §9 madde 8)

Durum: TODO

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

## Kabul kriterleri
- [ ] `make check` hatasız; üç wasm (`engine`, `engine_relaxed`, `engine_st`) `wasm-validate` geçer
- [ ] COOP/COEP'siz sunucuda sayfa ST yoluyla açılır (başsız Chrome ile doğrula, Notlar'a yaz)
