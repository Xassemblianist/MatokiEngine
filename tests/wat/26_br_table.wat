(module
  (func $switch_flat (param $x i32) (result i32)
    block $d
      block $c
        block $b
          block $a
            local.get $x
            br_table $a $b $c $d
          end
          i32.const 10
          return
        end
        i32.const 20
        return
      end
      i32.const 30
      return
    end
    i32.const 99
  )
  (func $switch_folded (param $x i32) (result i32)
    (block $default
      (block $case1
        (block $case0
          (br_table 0 1 $default (local.get $x))
        )
        (return (i32.const 100))
      )
      (return (i32.const 200))
    )
    (i32.const -1)
  )
  (export "switch_flat" (func $switch_flat))
  (export "switch_folded" (func $switch_folded))
)
