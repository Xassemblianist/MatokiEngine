;; 24_comments_and_literals.wat — Blok yorumları, onaltılık ve kayan nokta sabitleri
(; Üst seviye blok yorumu
   (; İç içe blok yorumu ;)
   devam ediyor ;)
(module
  (func $literals (export "literals") (result i32)
    (local $f f32)
    (local $d f64)
    (local.set $f (f32.add (f32.const 0x1.8p+2) (f32.const -3.25e+1)))
    (local.set $d (f64.const 0x1.0p-3))
    (i32.add
      (i32.add (i32.const 0x10_00) (i32.const -0x20))
      (i32.add
        (i32.trunc_sat_f32_s (local.get $f))
        (i32.wrap_i64 (i64.const -1_000_000))))
  )
)
