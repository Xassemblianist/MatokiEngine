;; asm/sum_f32.wat — Yatay f32 dizi toplamı (horizontal reduction) çekirdeği.
;; Clang'ı yenme stratejisi:
;; 1) Bağımlılık zincirini kırma + SIMD vektörleştirme: Clang -O2 -msimd128 (katı IEEE-754 kuralları
;;    altında `-ffast-math` olmadan) toplama sırasını değiştiremediği için tek bir skaler `sum`
;;    yazmacı üzerinde seri `f32.add` bağımlılık zinciri üretir (her `f32.add` 3-4 çevrim gecikmeli).
;; 2) Elle WAT'ta 4 bağımsız `f32x4` biriktiricisi (`$acc0..$acc3`) kullanılarak her döngü adımında
;;    16 adet `f32` (64 bayt, `offset=0/16/32/48`) paralel toplanır; 4 bağımsız yazmaç FPU toplama
;;    gecikmesini (latency) tamamen gizler.
;; 3) Döngü sonunda 4 biriktirici ikili ağaç (`(acc0 + acc1) + (acc2 + acc3)`) ile tek `f32x4`'e
;;    indirgenir ve 2 adet `i8x16.shuffle` + `f32x4.add` ile skaler toplama düşürülür.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $sum_f32 (export "sum_f32") (param $a i32) (param $count i32) (result f32)
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
        (local.set $acc0 (f32x4.add (local.get $acc0) (v128.load offset=0  (local.get $a))))
        (local.set $acc1 (f32x4.add (local.get $acc1) (v128.load offset=16 (local.get $a))))
        (local.set $acc2 (f32x4.add (local.get $acc2) (v128.load offset=32 (local.get $a))))
        (local.set $acc3 (f32x4.add (local.get $acc3) (v128.load offset=48 (local.get $a))))
        (local.set $a (i32.add (local.get $a) (i32.const 64)))
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
        (local.set $acc0 (f32x4.add (local.get $acc0) (v128.load (local.get $a))))
        (local.set $a (i32.add (local.get $a) (i32.const 16)))
        (br $loop4)
      )
    )

    ;; Yatay indirgeme: [x0, x1, x2, x3] -> (x0+x2) + (x1+x3)
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
        (local.set $sum (f32.add (local.get $sum) (f32.load (local.get $a))))
        (local.set $a (i32.add (local.get $a) (i32.const 4)))
        (br $loop1)
      )
    )

    (local.get $sum)
  )
)
