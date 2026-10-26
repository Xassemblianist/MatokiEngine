;; asm/blend_alpha_rgba8.wat — n piksel tamsayı src-over alfa harmanlama (RGBA8 + RGBA8 -> RGBA8).
;; Clang'ı yenme stratejisi:
;; 1) Clang -O2 -msimd128, 32-bit `u32` içindeki R,G,B,A kanallarını `i32x4.shr_u` + `v128.and 0xFF`
;;    ile 4 ayrı kanal olarak ayıklayıp `i32x4.mul` (4x4 = 16 çarpım) ve `i32x4.shl` + `v128.or`
;;    ile yeniden birleştirir; ayrıca sabit vektörleri döngü içinde tekrar yükler.
;; 2) Elle WAT'ta ise kanallar ayıklanmaz:
;;    - `i8x16.shuffle (3,3,3,3, 7,7,7,7, 11,11,11,11, 15,15,15,15)` ile her pikselin alfa baytı
;;      kendi 4 kanalına tek komutta yayılır (`a8`).
;;    - `255 - sa` işlemi `u8` aritmetiğinde birebir `~sa` olduğundan tek bir `v128.not` ile hesaplanır.
;;    - Kaynak alfa kanalı `v128.or 0xFF000000` ile 255'e sabitlenerek R,G,B,A'nın dördü de aynı
;;      `(sc * sa + dc * (255 - sa) + 128) >> 8` formülüyle hesaplanır.
;;    - `i16x8.extmul_low_i8x16_u` ve `i16x8.extmul_high_i8x16_u` ile genişletme + çarpma tek
;;      komutta birleştirilir; sonuç `i8x16.narrow_i16x8_u` ile tek komutta 16 bayta daraltılır.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $blend_alpha_rgba8 (export "blend_alpha_rgba8") (param $dst i32) (param $src i32) (param $count i32)
    (local $end4 i32)
    (local $end1 i32)
    (local $s8 v128)
    (local $d8 v128)
    (local $a8 v128)
    (local $inv_a8 v128)
    (local $s8_a255 v128)
    (local $lo v128)
    (local $hi v128)
    (local $mask_a255 v128)
    (local $c128 v128)
    (local $s i32)
    (local $d i32)
    (local $sa i32)
    (local $inv_a i32)

    (if (i32.eqz (local.get $count))
      (then (return))
    )

    (local.set $mask_a255 (v128.const i32x4 0xFF000000 0xFF000000 0xFF000000 0xFF000000))
    (local.set $c128 (v128.const i16x8 128 128 128 128 128 128 128 128))

    (local.set $end1 (i32.add (local.get $dst) (i32.shl (local.get $count) (i32.const 2))))
    (local.set $end4 (i32.add (local.get $dst) (i32.shl (i32.and (local.get $count) (i32.const -4)) (i32.const 2))))

    (block $break4
      (loop $loop4
        (br_if $break4 (i32.ge_u (local.get $dst) (local.get $end4)))

        (local.set $s8 (v128.load (local.get $src)))
        (local.set $d8 (v128.load (local.get $dst)))

        (local.set $a8
          (i8x16.shuffle 3 3 3 3 7 7 7 7 11 11 11 11 15 15 15 15
            (local.get $s8) (local.get $s8)))
        (local.set $inv_a8 (v128.not (local.get $a8)))
        (local.set $s8_a255 (v128.or (local.get $s8) (local.get $mask_a255)))

        (local.set $lo
          (i16x8.shr_u
            (i16x8.add
              (i16x8.add
                (i16x8.extmul_low_i8x16_u (local.get $s8_a255) (local.get $a8))
                (i16x8.extmul_low_i8x16_u (local.get $d8) (local.get $inv_a8)))
              (local.get $c128))
            (i32.const 8)))

        (local.set $hi
          (i16x8.shr_u
            (i16x8.add
              (i16x8.add
                (i16x8.extmul_high_i8x16_u (local.get $s8_a255) (local.get $a8))
                (i16x8.extmul_high_i8x16_u (local.get $d8) (local.get $inv_a8)))
              (local.get $c128))
            (i32.const 8)))

        (v128.store (local.get $dst)
          (i8x16.narrow_i16x8_u (local.get $lo) (local.get $hi)))

        (local.set $src (i32.add (local.get $src) (i32.const 16)))
        (local.set $dst (i32.add (local.get $dst) (i32.const 16)))
        (br $loop4)
      )
    )

    ;; Kalan 0..3 piksel için skaler kuyruk
    (block $break1
      (loop $loop1
        (br_if $break1 (i32.ge_u (local.get $dst) (local.get $end1)))
        (local.set $s (i32.load (local.get $src)))
        (local.set $d (i32.load (local.get $dst)))
        (local.set $sa (i32.and (i32.shr_u (local.get $s) (i32.const 24)) (i32.const 255)))
        (local.set $inv_a (i32.sub (i32.const 255) (local.get $sa)))

        (i32.store (local.get $dst)
          (i32.or
            (i32.or
              (i32.shr_u
                (i32.add
                  (i32.add
                    (i32.mul (i32.and (local.get $s) (i32.const 255)) (local.get $sa))
                    (i32.mul (i32.and (local.get $d) (i32.const 255)) (local.get $inv_a)))
                  (i32.const 128))
                (i32.const 8))
              (i32.shl
                (i32.shr_u
                  (i32.add
                    (i32.add
                      (i32.mul (i32.and (i32.shr_u (local.get $s) (i32.const 8)) (i32.const 255)) (local.get $sa))
                      (i32.mul (i32.and (i32.shr_u (local.get $d) (i32.const 8)) (i32.const 255)) (local.get $inv_a)))
                    (i32.const 128))
                  (i32.const 8))
                (i32.const 8)))
            (i32.or
              (i32.shl
                (i32.shr_u
                  (i32.add
                    (i32.add
                      (i32.mul (i32.and (i32.shr_u (local.get $s) (i32.const 16)) (i32.const 255)) (local.get $sa))
                      (i32.mul (i32.and (i32.shr_u (local.get $d) (i32.const 16)) (i32.const 255)) (local.get $inv_a)))
                    (i32.const 128))
                  (i32.const 8))
                (i32.const 16))
              (i32.shl
                (i32.shr_u
                  (i32.add
                    (i32.add
                      (i32.mul (i32.const 255) (local.get $sa))
                      (i32.mul (i32.and (i32.shr_u (local.get $d) (i32.const 24)) (i32.const 255)) (local.get $inv_a)))
                    (i32.const 128))
                  (i32.const 8))
                (i32.const 24)))))

        (local.set $src (i32.add (local.get $src) (i32.const 4)))
        (local.set $dst (i32.add (local.get $dst) (i32.const 4)))
        (br $loop1)
      )
    )
  )
)
