;; 05_i32_compare.wat — Tüm i32 karşılaştırma komutları
(module
  (func $cmp (export "cmp") (param $x i32) (param $y i32) (result i32)
    (i32.add
      (i32.add
        (i32.add
          (i32.eqz (local.get $x))
          (i32.eq (local.get $x) (local.get $y)))
        (i32.add
          (i32.ne (local.get $x) (local.get $y))
          (i32.lt_s (local.get $x) (local.get $y))))
      (i32.add
        (i32.add
          (i32.add
            (i32.lt_u (local.get $x) (local.get $y))
            (i32.gt_s (local.get $x) (local.get $y)))
          (i32.add
            (i32.gt_u (local.get $x) (local.get $y))
            (i32.le_s (local.get $x) (local.get $y))))
        (i32.add
          (i32.le_u (local.get $x) (local.get $y))
          (i32.add
            (i32.ge_s (local.get $x) (local.get $y))
            (i32.ge_u (local.get $x) (local.get $y))))))
  )
)
