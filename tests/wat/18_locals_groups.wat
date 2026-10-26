;; 18_locals_groups.wat — Farklı tiplerde çoklu local grupları ve local.tee
(module
  (func $locals_test (export "locals_test") (param $p0 i32) (param $p1 f32) (result i32)
    (local $a i32)
    (local $b i32)
    (local $c i64)
    (local $d f32)
    (local $e f64)
    (local $v v128)
    (local.set $a (i32.add (local.get $p0) (i32.const 1)))
    (local.set $b (local.tee $a (i32.mul (local.get $a) (i32.const 2))))
    (local.set $c (i64.extend_i32_s (local.get $b)))
    (local.set $d (f32.add (local.get $p1) (f32.const 4.0)))
    (local.set $e (f64.const 12.5))
    (local.set $v (i32x4.splat (local.get $a)))
    (i32.add
      (i32.add (local.get $b) (i32.wrap_i64 (local.get $c)))
      (i32.add
        (i32.trunc_sat_f32_s (local.get $d))
        (i32x4.extract_lane 2 (local.get $v))))
  )
)
