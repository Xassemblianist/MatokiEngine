;; 21_simd_shuffle_narrow.wat — i8x16.shuffle, narrow, splat ve dönüşümler
(module
  (func $pack_rgba (export "pack_rgba") (param $r f32) (param $g f32) (result i32)
    (local $vr v128)
    (local $vg v128)
    (local $ir v128)
    (local $ig v128)
    (local $n16 v128)
    (local $n8 v128)
    (local.set $vr (f32x4.convert_i32x4_s (i32x4.trunc_sat_f32x4_s (f32x4.splat (local.get $r)))))
    (local.set $vg (f32x4.convert_i32x4_u (i32x4.trunc_sat_f32x4_u (f32x4.splat (local.get $g)))))
    (local.set $ir (i32x4.trunc_sat_f32x4_s (local.get $vr)))
    (local.set $ig (i32x4.trunc_sat_f32x4_u (local.get $vg)))
    (local.set $n16
      (v128.xor
        (i16x8.narrow_i32x4_s (local.get $ir) (local.get $ig))
        (i16x8.narrow_i32x4_u (local.get $ir) (local.get $ig))))
    (local.set $n8
      (v128.andnot
        (i8x16.narrow_i16x8_s (local.get $n16) (local.get $n16))
        (v128.not (i8x16.narrow_i16x8_u (local.get $n16) (local.get $n16)))))
    (local.set $n8
      (i8x16.shuffle 0 4 8 12 1 5 9 13 2 6 10 14 3 7 11 15
        (local.get $n8)
        (v128.xor
          (i8x16.splat (i32.const 1))
          (v128.xor
            (i16x8.splat (i32.const 2))
            (v128.xor
              (i64x2.splat (i64.const 3))
              (f64x2.splat (f64.const 4.0)))))))
    (i32x4.extract_lane 0 (local.get $n8))
  )
)
