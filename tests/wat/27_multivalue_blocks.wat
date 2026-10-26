(module
  (func $swap (param $a i32) (param $b i32) (result i32 i32)
    local.get $b
    local.get $a
  )
  (func $multi_block (param $x i32) (param $y i32) (result i32)
    local.get $x
    local.get $y
    block (param i32 i32) (result i32 i32)
      call $swap
    end
    i32.sub
  )
  (func $multi_if_folded (param $c i32) (param $a i32) (param $b i32) (result i32)
    (if (result i32 i32) (local.get $c)
      (then
        (local.get $a)
        (local.get $b)
      )
      (else
        (local.get $b)
        (local.get $a)
      )
    )
    i32.add
  )
  (export "swap" (func $swap))
  (export "multi_block" (func $multi_block))
  (export "multi_if_folded" (func $multi_if_folded))
)
