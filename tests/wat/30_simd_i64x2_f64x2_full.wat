(module
  (func $test_i64x2 (param $a v128) (param $b v128) (result i64)
    (local $v v128)
    i64.const 42
    i64x2.splat
    i64.const 99
    i64x2.replace_lane 1
    local.get $a
    i64x2.add
    local.get $b
    i64x2.sub
    local.get $a
    i64x2.mul
    i64x2.abs
    i64x2.neg
    i32.const 3
    i64x2.shl
    i32.const 1
    i64x2.shr_s
    i32.const 1
    i64x2.shr_u
    local.get $b
    i64x2.eq
    local.get $b
    i64x2.ne
    local.get $b
    i64x2.lt_s
    local.get $b
    i64x2.gt_s
    local.get $b
    i64x2.le_s
    local.get $b
    i64x2.ge_s
    i64x2.extend_low_i32x4_s
    i64x2.extend_high_i32x4_s
    i64x2.extend_low_i32x4_u
    i64x2.extend_high_i32x4_u
    local.get $a
    i64x2.extmul_low_i32x4_s
    local.get $a
    i64x2.extmul_high_i32x4_s
    local.get $a
    i64x2.extmul_low_i32x4_u
    local.get $a
    i64x2.extmul_high_i32x4_u
    local.tee $v
    i64x2.extract_lane 0
    local.get $v
    i64x2.all_true
    local.get $v
    i64x2.bitmask
    i32.add
    i64.extend_i32_s
    i64.add
  )
  (func $test_f64x2 (param $a v128) (param $b v128) (result f64)
    (local $v v128)
    f64.const 1.5
    f64x2.splat
    f64.const 2.5
    f64x2.replace_lane 1
    local.get $a
    f64x2.add
    local.get $b
    f64x2.sub
    local.get $a
    f64x2.mul
    local.get $b
    f64x2.div
    f64x2.abs
    f64x2.neg
    f64x2.sqrt
    f64x2.ceil
    f64x2.floor
    f64x2.trunc
    f64x2.nearest
    local.get $b
    f64x2.min
    local.get $b
    f64x2.max
    local.get $b
    f64x2.pmin
    local.get $b
    f64x2.pmax
    f64x2.convert_low_i32x4_s
    f64x2.convert_low_i32x4_u
    f32x4.demote_f64x2_zero
    f64x2.promote_low_f32x4
    i32x4.trunc_sat_f64x2_s_zero
    f64x2.convert_low_i32x4_s
    i32x4.trunc_sat_f64x2_u_zero
    f64x2.convert_low_i32x4_u
    local.get $b
    f64x2.eq
    local.get $b
    f64x2.ne
    local.get $b
    f64x2.lt
    local.get $b
    f64x2.gt
    local.get $b
    f64x2.le
    local.get $b
    f64x2.ge
    f64x2.extract_lane 0
  )
  (export "test_i64x2" (func $test_i64x2))
  (export "test_f64x2" (func $test_f64x2))
)
