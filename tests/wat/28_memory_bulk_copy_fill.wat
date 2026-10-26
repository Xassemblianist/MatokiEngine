(module
  (memory 1)
  (func $bulk_ops (param $dst i32) (param $src i32) (param $len i32) (param $val i32) (result i32)
    local.get $dst
    local.get $val
    local.get $len
    memory.fill
    (memory.copy (local.get $dst) (local.get $src) (local.get $len))
    memory.size
    (memory.grow (i32.const 1))
    i32.add
  )
  (export "bulk_ops" (func $bulk_ops))
)
