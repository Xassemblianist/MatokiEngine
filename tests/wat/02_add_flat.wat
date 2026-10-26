;; 02_add_flat.wat — Düz (flat) formda toplama fonksiyonu
(module
  (func $add (export "add") (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    i32.add
  )
)
