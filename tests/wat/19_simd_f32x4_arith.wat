;; 19_simd_f32x4_arith.wat — f32x4 aritmetik, yuvarlama ve karşılaştırma
(module
  (func $f32x4_ops (export "f32x4_ops") (param $x f32) (result f32)
    (local $v0 v128)
    (local $v1 v128)
    (local $m v128)
    (local.set $v0 (f32x4.splat (local.get $x)))
    (local.set $v1 (v128.const f32x4 1.25 -2.5 3.75 4.0))
    (local.set $v0
      (f32x4.max
        (f32x4.min
          (f32x4.div
            (f32x4.mul
              (f32x4.sub
                (f32x4.add (local.get $v0) (local.get $v1))
                (f32x4.sqrt (f32x4.abs (local.get $v1))))
              (f32x4.neg (local.get $v0)))
            (v128.const f32x4 2.0 2.0 2.0 2.0))
          (f32x4.ceil (local.get $v1)))
        (f32x4.floor (f32x4.trunc (f32x4.nearest (local.get $v1))))))
    (local.set $m
      (v128.or
        (v128.and (f32x4.eq (local.get $v0) (local.get $v1)) (f32x4.ne (local.get $v0) (local.get $v1)))
        (v128.xor
          (v128.and (f32x4.lt (local.get $v0) (local.get $v1)) (f32x4.gt (local.get $v0) (local.get $v1)))
          (v128.or (f32x4.le (local.get $v0) (local.get $v1)) (f32x4.ge (local.get $v0) (local.get $v1))))))
    (local.set $v0 (v128.bitselect (local.get $v0) (local.get $v1) (local.get $m)))
    (local.set $v0 (f32x4.replace_lane 1 (local.get $v0) (f32.const 99.5)))
    (f32x4.extract_lane 1 (local.get $v0))
  )
)
