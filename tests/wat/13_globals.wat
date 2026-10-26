;; 13_globals.wat — Sabit ve değiştirilebilir (mut) global değişkenler
(module
  (global $g_const i32 (i32.const 100))
  (global $g_mut (export "g_mut") (mut i32) (i32.const 42))
  (global $g_f32 (mut f32) (f32.const 1.5))
  (global $g_i64 i64 (i64.const -999))
  (func $step (export "step") (param $delta i32) (result i32)
    (global.set $g_mut
      (i32.add
        (global.get $g_mut)
        (i32.add (local.get $delta) (global.get $g_const))))
    (global.set $g_f32 (f32.add (global.get $g_f32) (f32.const 0.5)))
    (i32.add
      (global.get $g_mut)
      (i32.add
        (i32.trunc_sat_f32_s (global.get $g_f32))
        (i32.wrap_i64 (global.get $g_i64))))
  )
)
