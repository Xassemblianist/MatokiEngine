;; 08_conversions.wat — Tüm tip dönüşümü ve doygun (saturating) dönüşüm komutları
(module
  (func $conv (export "conv") (param $i i32) (param $l i64) (param $f f32) (result i32)
    (local $acc i32)
    (local.set $acc (i32.wrap_i64 (local.get $l)))
    (local.set $acc (i32.add (local.get $acc) (i32.trunc_f32_s (local.get $f))))
    (local.set $acc (i32.add (local.get $acc) (i32.trunc_f32_u (f32.abs (local.get $f)))))
    (local.set $acc (i32.add (local.get $acc) (i32.trunc_sat_f32_s (local.get $f))))
    (local.set $acc (i32.add (local.get $acc) (i32.trunc_sat_f32_u (local.get $f))))
    (local.set $acc
      (i32.add
        (local.get $acc)
        (i32.reinterpret_f32
          (f32.add
            (f32.add
              (f32.convert_i32_s (local.get $i))
              (f32.convert_i32_u (local.get $i)))
            (f32.add
              (f32.convert_i64_s (i64.extend_i32_s (local.get $i)))
              (f32.convert_i64_u (i64.extend_i32_u (local.get $i))))))))
    (i32.add
      (local.get $acc)
      (i32.trunc_sat_f32_s (f32.reinterpret_i32 (local.get $i))))
  )
)
