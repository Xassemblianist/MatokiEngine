(module
  (func $test_i8x16 (param $a v128) (param $b v128) (result i32)
    (local $v v128)
    local.get $a
    i8x16.abs
    i8x16.neg
    i8x16.popcnt
    local.get $b
    i8x16.add
    local.get $b
    i8x16.add_sat_s
    local.get $b
    i8x16.add_sat_u
    local.get $b
    i8x16.sub
    local.get $b
    i8x16.sub_sat_s
    local.get $b
    i8x16.sub_sat_u
    local.get $b
    i8x16.min_s
    local.get $b
    i8x16.min_u
    local.get $b
    i8x16.max_s
    local.get $b
    i8x16.max_u
    local.get $b
    i8x16.avgr_u
    i32.const 1
    i8x16.shl
    i32.const 1
    i8x16.shr_s
    i32.const 1
    i8x16.shr_u
    local.get $b
    i8x16.eq
    local.get $b
    i8x16.ne
    local.get $b
    i8x16.lt_s
    local.get $b
    i8x16.lt_u
    local.get $b
    i8x16.gt_s
    local.get $b
    i8x16.gt_u
    local.get $b
    i8x16.le_s
    local.get $b
    i8x16.le_u
    local.get $b
    i8x16.ge_s
    local.get $b
    i8x16.ge_u
    local.tee $v
    i8x16.all_true
    local.get $v
    i8x16.bitmask
    i32.add
    local.get $v
    i8x16.extract_lane_s 3
    i32.add
    local.get $v
    i8x16.extract_lane_u 7
    i32.add
  )
  (func $test_i16x8 (param $a v128) (param $b v128) (result i32)
    (local $v v128)
    local.get $a
    i16x8.extadd_pairwise_i8x16_s
    local.get $b
    i16x8.extadd_pairwise_i8x16_u
    i16x8.add
    i16x8.abs
    i16x8.neg
    local.get $b
    i16x8.q15mulr_sat_s
    local.get $b
    i16x8.add_sat_s
    local.get $b
    i16x8.add_sat_u
    local.get $b
    i16x8.sub
    local.get $b
    i16x8.sub_sat_s
    local.get $b
    i16x8.sub_sat_u
    local.get $b
    i16x8.mul
    local.get $b
    i16x8.min_s
    local.get $b
    i16x8.min_u
    local.get $b
    i16x8.max_s
    local.get $b
    i16x8.max_u
    local.get $b
    i16x8.avgr_u
    local.get $a
    i16x8.extmul_low_i8x16_s
    local.get $a
    i16x8.extmul_high_i8x16_s
    local.get $a
    i16x8.extmul_low_i8x16_u
    local.get $a
    i16x8.extmul_high_i8x16_u
    i16x8.extend_low_i8x16_s
    i16x8.extend_high_i8x16_s
    i16x8.extend_low_i8x16_u
    i16x8.extend_high_i8x16_u
    i32.const 2
    i16x8.shl
    i32.const 1
    i16x8.shr_s
    i32.const 1
    i16x8.shr_u
    local.get $b
    i16x8.eq
    local.get $b
    i16x8.ne
    local.get $b
    i16x8.lt_s
    local.get $b
    i16x8.lt_u
    local.get $b
    i16x8.gt_s
    local.get $b
    i16x8.gt_u
    local.get $b
    i16x8.le_s
    local.get $b
    i16x8.le_u
    local.get $b
    i16x8.ge_s
    local.get $b
    i16x8.ge_u
    local.tee $v
    i16x8.all_true
    local.get $v
    i16x8.bitmask
    i32.add
    local.get $v
    i16x8.extract_lane_s 2
    i32.add
    local.get $v
    i16x8.extract_lane_u 5
    i32.add
  )
  (export "test_i8x16" (func $test_i8x16))
  (export "test_i16x8" (func $test_i16x8))
)
