;; 04_i32_arith_bitwise.wat — Tüm i32 aritmetik ve bit işlemleri
(module
  (func $i32_ops (export "i32_ops") (param $a i32) (param $b i32) (result i32)
    (local $t i32)
    local.get $a
    i32.clz
    local.get $b
    i32.ctz
    i32.add
    local.get $a
    i32.popcnt
    i32.add
    local.set $t
    (i32.rotr
      (i32.rotl
        (i32.shr_u
          (i32.shr_s
            (i32.shl
              (i32.xor
                (i32.or
                  (i32.and (local.get $t) (i32.const 0xff))
                  (i32.rem_u (local.get $a) (i32.const 7)))
                (i32.rem_s (local.get $b) (i32.const 5)))
              (i32.const 1))
            (i32.const 1))
          (i32.const 1))
        (i32.const 2))
      (i32.const 1))
    (i32.div_u (i32.div_s (local.get $a) (i32.const 2)) (i32.const 3))
    i32.add
  )
)
