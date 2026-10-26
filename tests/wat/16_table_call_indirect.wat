;; 16_table_call_indirect.wat — Tablo, elem segmenti ve call_indirect
(module
  (type $binop (func (param i32 i32) (result i32)))
  (table 4 funcref)
  (elem (i32.const 0) func $op_add $op_sub $op_mul)

  (func $op_add (type $binop) (param $a i32) (param $b i32) (result i32)
    (i32.add (local.get $a) (local.get $b))
  )
  (func $op_sub (type $binop) (param $a i32) (param $b i32) (result i32)
    (i32.sub (local.get $a) (local.get $b))
  )
  (func $op_mul (type $binop) (param $a i32) (param $b i32) (result i32)
    (i32.mul (local.get $a) (local.get $b))
  )
  (func $dispatch (export "dispatch") (param $idx i32) (param $x i32) (param $y i32) (result i32)
    (call_indirect (type $binop) (local.get $x) (local.get $y) (local.get $idx))
  )
)
