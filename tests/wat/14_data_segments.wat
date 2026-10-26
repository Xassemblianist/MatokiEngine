;; 14_data_segments.wat — Data segmentleri ve kaçış dizileri
(module
  (memory 1)
  (data (i32.const 0) "MATOKI\00\01\02\ff")
  (data (i32.const 64) "shader_v1\n\t\"ok\"\\end")
  (func $read_magic (export "read_magic") (result i32)
    (i32.load (i32.const 0))
  )
)
