;; 20_simd_i32x4_arith.wat — i32x4 aritmetik, kaydırma, karşılaştırma ve bitmask
(module
  (func $i32x4_ops (export "i32x4_ops") (param $x i32) (result i32)
    (local $a v128)
    (local $b v128)
    (local $c v128)
    (local.set $a (i32x4.splat (local.get $x)))
    (local.set $b (v128.const i32x4 10 -20 30 -40))
    (local.set $a
      (i32x4.max_u
        (i32x4.min_u
          (i32x4.max_s
            (i32x4.min_s
              (i32x4.mul
                (i32x4.sub
                  (i32x4.add (i32x4.abs (local.get $b)) (i32x4.neg (local.get $a)))
                  (i32x4.shl (local.get $a) (i32.const 1)))
                (i32x4.shr_u (i32x4.shr_s (local.get $b) (i32.const 1)) (i32.const 1)))
              (local.get $b))
            (local.get $a))
          (local.get $b))
        (local.get $a)))
    (local.set $c
      (v128.or
        (v128.and (i32x4.eq (local.get $a) (local.get $b)) (i32x4.ne (local.get $a) (local.get $b)))
        (v128.or
          (v128.or
            (v128.and (i32x4.lt_s (local.get $a) (local.get $b)) (i32x4.lt_u (local.get $a) (local.get $b)))
            (v128.and (i32x4.gt_s (local.get $a) (local.get $b)) (i32x4.gt_u (local.get $a) (local.get $b))))
          (v128.or
            (v128.and (i32x4.le_s (local.get $a) (local.get $b)) (i32x4.le_u (local.get $a) (local.get $b)))
            (v128.and (i32x4.ge_s (local.get $a) (local.get $b)) (i32x4.ge_u (local.get $a) (local.get $b)))))))
    (local.set $a (i32x4.replace_lane 3 (local.get $a) (i32x4.bitmask (local.get $c))))
    (i32.add
      (i32x4.extract_lane 3 (local.get $a))
      (i32.add (v128.any_true (local.get $a)) (i32x4.all_true (local.get $a))))
  )
)
