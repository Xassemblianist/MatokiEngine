;; 03_add_folded.wat — Katlanmış (folded) S-ifadesi formunda aritmetik
(module
  (func $calc (export "calc") (param $x i32) (param $y i32) (result i32)
    (i32.sub
      (i32.mul
        (i32.add (local.get $x) (i32.const 10))
        (local.get $y))
      (i32.const 3))
  )
)
