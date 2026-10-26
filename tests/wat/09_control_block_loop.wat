;; 09_control_block_loop.wat — Düz block, loop, br, br_if ve $etiket çözümleme
(module
  (func $sum_to (export "sum_to") (param $n i32) (result i32)
    (local $acc i32)
    i32.const 0
    local.set $acc
    block $exit
      loop $top
        local.get $n
        i32.eqz
        br_if $exit
        local.get $acc
        local.get $n
        i32.add
        local.set $acc
        local.get $n
        i32.const 1
        i32.sub
        local.set $n
        br $top
      end $top
    end $exit
    local.get $acc
  )
)
