DURUM: DONE
HAT: C (mbin — bizim assembler'ımız)

# M3_1 — mbin: wasm binary üretici

## Amaç
Matoki JIT'inin arka ucu: C'den, byte byte, geçerli `.wasm` modülleri üretmek.
Bu görev diğer hatlardan bağımsızdır; sadece `core/types.h` ve `core/log.h` kullanır.

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/types.h`, `src/core/log.h/.c`, `tests/test.h`, `tools/host/*`.
Yeni `tests/*.c` dosyaları otomatik olarak `make test`'e girer. `tests/` host koddur, libc serbest.

## Referans
WebAssembly Core Spec §5 (Binary Format) + SIMD (0xFD prefix) opcode tablosu.

## Dosyalar
- `src/mbin/mbin.h/.c`
  - **Bayt tamponu:** `MbBuf { u8* p; u32 len, cap; }` — dışarıdan verilen bellek üzerine (`mb_buf_init(b, mem, cap)`), taşmada `PANIC`. `mb_u8`, `mb_bytes`, `mb_uleb(u64)`, `mb_sleb(i64)`, `mb_f32`, `mb_f64`, `mb_name(str, len)`.
  - **Modül kurucu:** `MbModule` (sabit kapasiteli diziler, malloc YOK, tüm depolama çağıranın verdiği bir `u8*` alanından):
    - `mb_type(m, params[], np, results[], nr) -> type_idx` (tekrar eden imzaları tekilleştir)
    - `mb_import_func(m, "env", "isim", type_idx) -> func_idx`
    - `mb_import_memory(m, "env", "memory", min, max, shared)`
    - `mb_import_table(m, "env", "__indirect_function_table", min)`
    - `mb_func_begin(m, type_idx) -> MbFunc*`, `mb_local(f, valtype, count)`, `mb_func_end(f)`
    - `mb_export_func(m, "isim", func_idx)`
    - `mb_module_finish(m, out_buf) -> u32 byte_len` (section sırası spec'e uygun: type, import, function, table, memory, global, export, start, element, code, data)
  - Bölüm boyutları için önce içeriği geçici tampona yaz, sonra uzunluğu LEB128 ile önüne ekle (veya 5-byte sabit genişlikli padded LEB — seçimini notlara yaz).
- `src/mbin/mbin_ops.h` — opcode yayıcıları, `static inline`:
  - Kontrol: `mb_block/mb_loop/mb_if(f, blocktype)`, `mb_else`, `mb_end`, `mb_br/mb_br_if(depth)`, `mb_return`, `mb_call`, `mb_call_indirect`
  - Yerel: `mb_local_get/set/tee`, `mb_global_get/set`
  - Bellek: `mb_i32_load/store`, `mb_f32_load/store`, `mb_i32_load8_u`, `mb_i32_load16_u` ... (align, offset parametreli)
  - Sayısal: i32/i64/f32 aritmetik, karşılaştırma, dönüşüm (`i32.trunc_sat_f32_s` dahil)
  - **SIMD (0xFD prefix, alt opcode LEB128):** `v128.load/store`, `v128.const`, `i32x4.splat/f32x4.splat`, `f32x4.add/sub/mul/div/min/max/sqrt`, `f32x4.eq/lt/le/gt/ge`, `v128.and/or/xor/not/andnot/bitselect`, `v128.any_true`, `i32x4.all_true`, `i32x4.add/sub/mul/shl/shr_s/shr_u`, `f32x4.extract_lane/replace_lane`, `i32x4.extract_lane`, `i8x16.shuffle`, `i32x4.trunc_sat_f32x4_s`, `f32x4.convert_i32x4_s`, `i8x16.narrow_i16x8_u`, `i16x8.narrow_i32x4_u`
  - Hata ayıklama için: blok derinliği takibi (`f->depth`), `mb_end` fazla çağrılırsa `PANIC`.
- `tests/test_mbin.c`
  - LEB128 kodlama/çözme sınır değerleri (0, 127, 128, 624485, -1, -64, -65, i64 min/max)
  - Modül üret → `build/test_out/*.wasm` yaz → `system("wasm-validate --enable-all ...")` dönüş kodu 0
  - Çalıştırma testleri `wasm-interp --run-all-exports` ile (çıktıyı popen ile oku, beklenen değerle karşılaştır):
    1. `add(i32,i32)->i32`
    2. döngülü `fact(i32)->i64`
    3. SIMD: iki `f32x4.const` topla, `f32x4.extract_lane 2` döndür
    4. memory import eden, belleğe yazıp okuyan fonksiyon (wasm-interp memory import'u desteklemiyorsa bu modül sadece validate edilsin, notlara yaz)

## Kabul kriterleri
- `make` ve `make test` uyarısız geçer; tüm üretilen modüller `wasm-validate`'ten geçer.
- `src/mbin/` libc kullanmaz, malloc kullanmaz.
- Notlara: desteklenen opcode listesi, üretilen `add` modülünün byte boyutu ve hex dökümü.

## Notlar
- **Bölüm (Section) ve Fonksiyon Gövdesi Boyutlandırma Seçimi:** Padded 5-byte LEB128 yerine, `mb_module_init` ile dışarıdan verilen `work_mem` çalışma tamponunun kalan kısmını geçici bölüm tamponu (`sec`) olarak kullanıp içeriği önce oraya yazma ve ardından kanonik (minimum uzunluklu) ULEB128 ile boyutu önüne ekleme yöntemi seçildi. Böylece üretilen `.wasm` ikilileri gereksiz dolgu byte'ı içermez ve `wat2wasm` çıktısıyla birebir aynı boyuttadır.
- **Bellek Import Testi Notu:** `wasm-interp` aracı dışarıdan `"env.memory"` import'unu sağlamadığı için (`error initializing module: invalid import "env.memory"`), `"env.memory"` ve `"env.__indirect_function_table"` import eden `build/test_out/mem_import.wasm` modülü `wasm-validate --enable-all` ile doğrulandı. Ayrıca bellek yükleme/saklama komutlarının çalışma zamanı doğruluğunu da sınamak için dahili bellek (`Section 5: Memory`, `mb_memory`) tanımlayan `build/test_out/mem_internal.wasm` modülü üretilip `wasm-interp --run-all-exports` ile çalıştırıldı (`test_mem_rw() => i32:100`).
- **Üretilen `add` Modülü Byte Boyutu ve Hex Dökümü:**
  - **Saf `add(i32, i32) -> i32` (`build/test_out/add_pure.wasm`):** **41 byte**
    ```
    00 61 73 6d 01 00 00 00  01 07 01 60 02 7f 7f 01
    7f 03 02 01 00 07 07 01  03 61 64 64 00 00 0a 09
    01 07 00 20 00 20 01 6a  0b
    ```
  - **Sarmalayıcılı `add` + `test_add() -> add(2, 3)` (`build/test_out/add.wasm`):** **66 byte**
    ```
    00 61 73 6d 01 00 00 00  01 0b 02 60 02 7f 7f 01
    7f 60 00 01 7f 03 03 02  00 01 07 12 02 03 61 64
    64 00 00 08 74 65 73 74  5f 61 64 64 00 01 0a 12
    02 07 00 20 00 20 01 6a  0b 08 00 41 02 41 03 10
    00 0b
    ```
- **Desteklenen Opcode Listesi (`src/mbin/mbin_ops.h`):**
  - **Kontrol & Parametrik:** `unreachable` (`0x00`), `nop` (`0x01`), `block` (`0x02`), `loop` (`0x03`), `if` (`0x04`), `else` (`0x05`), `end` (`0x0B`), `br` (`0x0C`), `br_if` (`0x0D`), `return` (`0x0F`), `call` (`0x10`), `call_indirect` (`0x11`), `drop` (`0x1A`), `select` (`0x1B`).
  - **Yerel & Global:** `local.get` (`0x20`), `local.set` (`0x21`), `local.tee` (`0x22`), `global.get` (`0x23`), `global.set` (`0x24`).
  - **Bellek:** `i32.load` (`0x28`), `i64.load` (`0x29`), `f32.load` (`0x2A`), `f64.load` (`0x2B`), `i32.load8_s` (`0x2C`), `i32.load8_u` (`0x2D`), `i32.load16_s` (`0x2E`), `i32.load16_u` (`0x2F`), `i32.store` (`0x36`), `i64.store` (`0x37`), `f32.store` (`0x38`), `f64.store` (`0x39`), `i32.store8` (`0x3A`), `i32.store16` (`0x3B`).
  - **Sabitler:** `i32.const` (`0x41`), `i64.const` (`0x42`), `f32.const` (`0x43`), `f64.const` (`0x44`).
  - **Karşılaştırma:** `i32.eqz/eq/ne/lt_s/lt_u/gt_s/gt_u/le_s/le_u/ge_s/ge_u` (`0x45..0x4F`), `i64.eqz/eq/ne/lt_s/lt_u/gt_s/gt_u/le_s/le_u/ge_s/ge_u` (`0x50..0x5A`), `f32.eq/ne/lt/gt/le/ge` (`0x5B..0x60`).
  - **Aritmetik & Bit İşlemleri:** `i32.clz/ctz/popcnt/add/sub/mul/div_s/div_u/rem_s/rem_u/and/or/xor/shl/shr_s/shr_u/rotl/rotr` (`0x67..0x78`), `i64.clz/ctz/popcnt/add/sub/mul/div_s/div_u/rem_s/rem_u/and/or/xor/shl/shr_s/shr_u` (`0x79..0x88`), `f32.abs/neg/ceil/floor/trunc/nearest/sqrt/add/sub/mul/div/min/max/copysign` (`0x8B..0x98`).
  - **Dönüşüm:** `i32.wrap_i64` (`0xA7`), `i32.trunc_f32_s/u` (`0xA8..0xA9`), `i64.extend_i32_s/u` (`0xAC..0xAD`), `f32.convert_i32_s/u` (`0xB2..0xB3`), `f32.convert_i64_s/u` (`0xB4..0xB5`), `i32.reinterpret_f32` (`0xBC`), `f32.reinterpret_i32` (`0xBE`), `i32.trunc_sat_f32_s/u` (`0xFC 0x00..0x01`).
  - **SIMD (`0xFD` + ULEB128 alt-opcode):** `v128.load` (`0x00`), `v128.store` (`0x0B`), `v128.const` (`0x0C`, `f32x4_const` ve `i32x4_const` yardımcılarıyla), `i8x16.shuffle` (`0x0D`), `i8x16/i16x8/i32x4/i64x2/f32x4/f64x2.splat` (`0x0F..0x14`), `i32x4.extract_lane/replace_lane` (`0x1B..0x1C`), `f32x4.extract_lane/replace_lane` (`0x1F..0x20`), `i32x4.eq/ne/lt_s/lt_u/gt_s/gt_u/le_s/le_u/ge_s/ge_u` (`0x37..0x40`), `f32x4.eq/ne/lt/gt/le/ge` (`0x41..0x46`), `v128.not/and/andnot/or/xor/bitselect/any_true` (`0x4D..0x53`), `i8x16.narrow_i16x8_s/u` (`0x65..0x66`), `f32x4.ceil/floor/trunc/nearest` (`0x67..0x6A`), `i16x8.narrow_i32x4_s/u` (`0x85..0x86`), `i32x4.abs/neg/all_true/bitmask/shl/shr_s/shr_u/add/sub/mul/min_s/min_u/max_s/max_u` (`0xA0..0xB9`), `f32x4.abs/neg/sqrt/add/sub/mul/div/min/max` (`0xE0..0xE9`), `i32x4.trunc_sat_f32x4_s/u` (`0xF8..0xF9`), `f32x4.convert_i32x4_s/u` (`0xFA..0xFB`).

