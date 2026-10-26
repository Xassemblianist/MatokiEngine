;; 17_multi_func_calls.wat — Çoklu fonksiyon ve ileri yönlü (forward) $isim çağrısı
(module
  (func $main (export "main") (param $n i32) (result i32)
    (call $helper_b (call $helper_a (local.get $n)))
  )
  (func $helper_a (param $x i32) (result i32)
    (i32.mul (local.get $x) (i32.const 3))
  )
  (func $helper_b (param $y i32) (result i32)
    (i32.add (local.get $y) (i32.const 7))
  )
)
