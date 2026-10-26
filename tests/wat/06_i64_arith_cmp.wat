;; 06_i64_arith_cmp.wat — i64 aritmetik, bit ve karşılaştırma işlemleri
(module
  (func $i64_ops (export "i64_ops") (param $a i64) (param $b i64) (result i64)
    (local $c i64)
    (local.set $c
      (i64.add
        (i64.clz (local.get $a))
        (i64.add (i64.ctz (local.get $b)) (i64.popcnt (local.get $a)))))
    (i64.shr_u
      (i64.shr_s
        (i64.shl
          (i64.xor
            (i64.or
              (i64.and
                (i64.mul (i64.sub (local.get $a) (local.get $b)) (local.get $c))
                (i64.div_s (local.get $a) (i64.const 3)))
              (i64.div_u (local.get $b) (i64.const 5)))
            (i64.add
              (i64.rem_s (local.get $a) (i64.const 7))
              (i64.rem_u (local.get $b) (i64.const 9))))
          (i64.const 2))
        (i64.const 1))
      (i64.const 1))
    (i64.extend_i32_u
      (i32.add
        (i32.add
          (i64.eqz (local.get $a))
          (i32.add (i64.eq (local.get $a) (local.get $b)) (i64.ne (local.get $a) (local.get $b))))
        (i32.add
          (i32.add
            (i32.add (i64.lt_s (local.get $a) (local.get $b)) (i64.lt_u (local.get $a) (local.get $b)))
            (i32.add (i64.gt_s (local.get $a) (local.get $b)) (i64.gt_u (local.get $a) (local.get $b))))
          (i32.add
            (i32.add (i64.le_s (local.get $a) (local.get $b)) (i64.le_u (local.get $a) (local.get $b)))
            (i32.add (i64.ge_s (local.get $a) (local.get $b)) (i64.ge_u (local.get $a) (local.get $b)))))))
    i64.add
  )
)
