;; 15_imports_exports.wat — Çeşitli import ve export bildirimleri
(module
  (type $log_t (func (param i32 i32)))
  (import "env" "plat_log" (func $log (type $log_t)))
  (import "env" "plat_time" (func $time (result f32)))
  (import "env" "memory" (memory 1 256))
  (import "env" "g_frame" (global $frame (mut i32)))

  (func $entry (param $x i32) (result i32)
    (call $log (local.get $x) (global.get $frame))
    (i32.add (local.get $x) (i32.trunc_sat_f32_s (call $time)))
  )

  (export "entry" (func $entry))
  (export "memory" (memory 0))
)
