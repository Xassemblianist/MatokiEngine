;; 22_simd_memory_quad.wat — v128.load / v128.store ile SoA quad gölgelendirme döngüsü
(module
  (import "env" "memory" (memory 1 256))
  (func $shade_loop (export "shade_loop") (param $in_ptr i32) (param $out_ptr i32) (param $count i32)
    (local $i i32)
    (local $off i32)
    (local $v v128)
    (local.set $i (i32.const 0))
    (block $done
      (loop $step
        (br_if $done (i32.ge_u (local.get $i) (local.get $count)))
        (local.set $off (i32.shl (local.get $i) (i32.const 4)))
        (local.set $v
          (f32x4.mul
            (v128.load offset=0 align=4 (i32.add (local.get $in_ptr) (local.get $off)))
            (v128.const f32x4 0.5 0.5 0.5 1.0)))
        (v128.store offset=0 align=16 (i32.add (local.get $out_ptr) (local.get $off)) (local.get $v))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $step)
      )
    )
  )
)
