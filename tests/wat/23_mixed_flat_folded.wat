;; 23_mixed_flat_folded.wat — Aynı fonksiyonda düz ve katlanmış ifadelerin karışımı
(module
  (func $mixed (export "mixed") (param $a i32) (param $b i32) (result i32)
    (local $tmp i32)
    (i32.add (local.get $a) (i32.const 5))
    local.get $b
    i32.mul
    local.tee $tmp
    (i32.sub (local.get $b) (i32.const 100))
    i32.add
    block $blk (result i32)
      (if (i32.gt_s (local.get $a) (i32.const 10))
        (then
          (br $blk (i32.const 77))))
      local.get $tmp
    end $blk
    i32.add
  )
)
