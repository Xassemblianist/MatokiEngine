;; 07_f32_math_cmp.wat — f32 matematik ve karşılaştırma işlemleri
(module
  (func $f32_ops (export "f32_ops") (param $a f32) (param $b f32) (result f32)
    (local $t f32)
    (local.set $t
      (f32.copysign
        (f32.max
          (f32.min
            (f32.div
              (f32.mul
                (f32.sub
                  (f32.add
                    (f32.sqrt (f32.abs (local.get $a)))
                    (f32.nearest (f32.trunc (local.get $b))))
                  (f32.floor (f32.ceil (f32.neg (local.get $a)))))
                (f32.const 2.5))
              (f32.const 1.25))
            (f32.const 100.0))
          (f32.const -100.0))
        (local.get $b)))
    (if (result f32)
      (i32.and
        (i32.or (f32.eq (local.get $a) (local.get $b)) (f32.ne (local.get $a) (local.get $b)))
        (i32.and
          (i32.or (f32.lt (local.get $a) (local.get $b)) (f32.gt (local.get $a) (local.get $b)))
          (i32.or (f32.le (local.get $a) (local.get $b)) (f32.ge (local.get $a) (local.get $b)))))
      (then (local.get $t))
      (else (f32.const 0.0)))
  )
)
