(module
  (memory 1)
  (func $widening_and_lane_loads (param $p i32) (param $v v128) (result v128)
    local.get $p
    v128.load8x8_s
    local.get $p
    v128.load8x8_u offset=8
    i16x8.add
    local.get $p
    v128.load16x4_s offset=16 align=4
    local.get $p
    v128.load16x4_u
    i32x4.add
    i32x4.add
    local.get $p
    v128.load32x2_s
    local.get $p
    v128.load32x2_u
    i64x2.add
    i64x2.add
    local.get $p
    v128.load8_splat
    local.get $p
    v128.load16_splat
    local.get $p
    v128.load32_splat
    local.get $p
    v128.load64_splat
    v128.xor
    v128.xor
    v128.xor
    v128.xor
    local.get $p
    v128.load32_zero
    local.get $p
    v128.load64_zero offset=4
    v128.xor
    v128.xor
    local.set $v
    local.get $p
    local.get $v
    v128.load8_lane 3
    local.set $v
    local.get $p
    local.get $v
    v128.load16_lane offset=2 5
    local.set $v
    local.get $p
    local.get $v
    v128.load32_lane offset=4 align=2 2
    local.set $v
    local.get $p
    local.get $v
    v128.load64_lane offset=8 align=4 1
    local.set $v
    local.get $p
    local.get $v
    v128.store8_lane 0
    local.get $p
    local.get $v
    v128.store16_lane offset=2 1
    local.get $p
    local.get $v
    v128.store32_lane offset=4 align=2 2
    local.get $p
    local.get $v
    v128.store64_lane offset=8 align=4 1
    local.get $v
  )
  (export "widening_and_lane_loads" (func $widening_and_lane_loads))
)
