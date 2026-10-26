;; 10_control_if_else.wat — Düz ve katlanmış if/else, select, return, nop
(module
  (func $branchy (export "branchy") (param $a i32) (param $b i32) (result i32)
    nop
    local.get $a
    i32.const 0
    i32.lt_s
    if $neg
      i32.const -1
      return
    end $neg
    (if (result i32)
      (i32.gt_s (local.get $a) (local.get $b))
      (then
        (select (local.get $a) (local.get $b) (i32.const 1)))
      (else
        local.get $b
        local.get $a
        i32.sub))
  )
)
