;; asm/dot_f32.wat — İki f32 dizisinin iç çarpımı (dot product) çekirdeği.
;; Clang'ı yenme stratejisi:
;; 1) Clang -O2 -msimd128, IEEE-754 toplama birleşimsizliği nedeniyle iç çarpımı vektörleştiremez;
;;    skaler `f32.load` + `f32.mul` + tek bir `sum` yazmacına seri `f32.add` üretir.
;; 2) Elle WAT'ta 4 bağımsız `f32x4` biriktiricisi (`$acc0..$acc3`) ile her iterasyonda 16 çift
;;    `f32` (iki diziden toplam 128 bayt) `v128.load offset=0/16/32/48` + `f32x4.mul` + `f32x4.add`
;;    ile paralel işlenir; FPU çarpma-toplama gecikmesi 4 bağımsız kanal ile tamamen gizlenir.
;; 3) Döngü sonunda 4 vektör biriktiricisi ağaç indirgemesi ve `i8x16.shuffle` ile tek `f32`'ye düşürülür.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $dot_f32 (export "dot_f32") (param $a i32) (param $b i32) (param $count i32) (result f32)
    (local $end16 i32)
    (local $end4 i32)
    (local $end1 i32)
    (local $acc0 v128)
    (local $acc1 v128)
    (local $acc2 v128)
    (local $acc3 v128)
    (local $sum f32)

    (if (i32.eqz (local.get $count))
      (then (return (f32.const 0.0)))
    )

    (local.set $acc0 (v128.const f32x4 0.0 0.0 0.0 0.0))
    (local.set $acc1 (v128.const f32x4 0.0 0.0 0.0 0.0))
    (local.set $acc2 (v128.const f32x4 0.0 0.0 0.0 0.0))
    (local.set $acc3 (v128.const f32x4 0.0 0.0 0.0 0.0))

    (local.set $end1  (i32.add (local.get $a) (i32.shl (local.get $count) (i32.const 2))))
    (local.set $end16 (i32.add (local.get $a) (i32.shl (i32.and (local.get $count) (i32.const -16)) (i32.const 2))))

    (block $break16
      (loop $loop16
        (br_if $break16 (i32.ge_u (local.get $a) (local.get $end16)))
        (local.set $acc0
          (f32x4.add (local.get $acc0)
            (f32x4.mul (v128.load offset=0  (local.get $a)) (v128.load offset=0  (local.get $b)))))
        (local.set $acc1
          (f32x4.add (local.get $acc1)
            (f32x4.mul (v128.load offset=16 (local.get $a)) (v128.load offset=16 (local.get $b)))))
        (local.set $acc2
          (f32x4.add (local.get $acc2)
            (f32x4.mul (v128.load offset=32 (local.get $a)) (v128.load offset=32 (local.get $b)))))
        (local.set $acc3
          (f32x4.add (local.get $acc3)
            (f32x4.mul (v128.load offset=48 (local.get $a)) (v128.load offset=48 (local.get $b)))))
        (local.set $a (i32.add (local.get $a) (i32.const 64)))
        (local.set $b (i32.add (local.get $b) (i32.const 64)))
        (br $loop16)
      )
    )

    (local.set $acc0
      (f32x4.add
        (f32x4.add (local.get $acc0) (local.get $acc1))
        (f32x4.add (local.get $acc2) (local.get $acc3))))

    (local.set $end4 (i32.sub (local.get $end1) (i32.shl (i32.and (local.get $count) (i32.const 3)) (i32.const 2))))
    (block $break4
      (loop $loop4
        (br_if $break4 (i32.ge_u (local.get $a) (local.get $end4)))
        (local.set $acc0
          (f32x4.add (local.get $acc0)
            (f32x4.mul (v128.load (local.get $a)) (v128.load (local.get $b)))))
        (local.set $a (i32.add (local.get $a) (i32.const 16)))
        (local.set $b (i32.add (local.get $b) (i32.const 16)))
        (br $loop4)
      )
    )

    (local.set $acc0
      (f32x4.add
        (local.get $acc0)
        (i8x16.shuffle 8 9 10 11 12 13 14 15 0 1 2 3 4 5 6 7 (local.get $acc0) (local.get $acc0))))
    (local.set $sum
      (f32.add
        (f32x4.extract_lane 0 (local.get $acc0))
        (f32x4.extract_lane 1 (local.get $acc0))))

    (block $break1
      (loop $loop1
        (br_if $break1 (i32.ge_u (local.get $a) (local.get $end1)))
        (local.set $sum
          (f32.add (local.get $sum)
            (f32.mul (f32.load (local.get $a)) (f32.load (local.get $b)))))
        (local.set $a (i32.add (local.get $a) (i32.const 4)))
        (local.set $b (i32.add (local.get $b) (i32.const 4)))
        (br $loop1)
      )
    )

    (local.get $sum)
  )
)
