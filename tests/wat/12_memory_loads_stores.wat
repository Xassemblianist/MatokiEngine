;; 12_memory_loads_stores.wat — Tüm skaler bellek yükleme/saklama ve offset/align
(module
  (memory $mem (export "mem") 1 4)
  (func $mem_ops (export "mem_ops") (param $base i32) (result i32)
    (i32.store offset=0 align=4 (local.get $base) (i32.const 0x12345678))
    (i64.store offset=8 align=4 (local.get $base) (i64.const 0x1122334455667788))
    (f32.store offset=16 align=2 (local.get $base) (f32.const 3.5))
    (f64.store offset=24 align=8 (local.get $base) (f64.const 7.25))
    (i32.store8 offset=32 (local.get $base) (i32.const 0xfe))
    (i32.store16 offset=34 align=1 (local.get $base) (i32.const 0xabcd))

    (i32.add
      (i32.add
        (i32.load offset=0 align=2 (local.get $base))
        (i32.wrap_i64 (i64.load offset=8 (local.get $base))))
      (i32.add
        (i32.add
          (i32.trunc_sat_f32_s (f32.load offset=16 (local.get $base)))
          (i32.wrap_i64 (i64.load offset=24 (local.get $base))))
        (i32.add
          (i32.add
            (i32.load8_s offset=32 (local.get $base))
            (i32.load8_u offset=32 (local.get $base)))
          (i32.add
            (i32.load16_s offset=34 align=1 (local.get $base))
            (i32.load16_u offset=34 (local.get $base))))))
    (f64.load offset=24 (local.get $base))
    drop
  )
)
