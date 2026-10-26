;; asm/mandelbrot_row.wat — Hesap yoğun, erken çıkışlı (branchy) Mandelbrot satırı çekirdeği.
;; Clang'ı yenme stratejisi:
;; 1) Veri-bağımlı erken çıkış (`if (zx2 + zy2 > 4.0f) break;`) nedeniyle clang -O2 -msimd128
;;    döngüyü vektörleştiremez ve pikselleri tek tek skaler (`f32.mul`, `f32.add`, `f32.gt`, `br_if`)
;;    olarak işler.
;; 2) Elle WAT'ta 4 komşu piksel (`i, i+1, i+2, i+3`) tek bir `f32x4` SIMD şeridinde paralel işlenir:
;;    - Her iterasyonda `f32x4.le(zx2 + zy2, 4.0)` karşılaştırması kalıcı `$vactive` maskesiyle `v128.and`
;;      edilerek ıraksamayan (aktif) piksellerin maskesi (`0xFFFFFFFF` = -1) korunur.
;;    - `v128.any_true ($vactive)` kontrolü ile 4 pikselin tamamı 4.0 sınırını aştığı anda iç döngüden
;;      erken çıkılır (`br_if $iter_done`).
;;    - Aktif lane'lerin iterasyon sayacı dallanmasız tek bir `i32x4.sub ($viter, $vactive)` (`iter - (-1)`)
;;      komutuyla artırılır.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $mandelbrot_row (export "mandelbrot_row")
    (param $out_iter i32) (param $width i32) (param $x0 f32) (param $y0 f32) (param $dx f32) (param $max_iter i32)
    (local $i i32)
    (local $w4 i32)
    (local $k i32)
    (local $vx0 v128)
    (local $vcy v128)
    (local $vdx v128)
    (local $vc4 v128)
    (local $vi_vec v128)
    (local $vi_step v128)
    (local $vcx v128)
    (local $vzx v128)
    (local $vzy v128)
    (local $vzx2 v128)
    (local $vzy2 v128)
    (local $vnzy v128)
    (local $viter v128)
    (local $vactive v128)
    (local $cx f32)
    (local $zx f32)
    (local $zy f32)
    (local $zx2 f32)
    (local $zy2 f32)
    (local $nzy f32)

    (if (i32.eqz (local.get $width))
      (then (return))
    )

    (local.set $vx0     (f32x4.splat (local.get $x0)))
    (local.set $vcy     (f32x4.splat (local.get $y0)))
    (local.set $vdx     (f32x4.splat (local.get $dx)))
    (local.set $vc4     (v128.const f32x4 4.0 4.0 4.0 4.0))
    (local.set $vi_vec  (v128.const i32x4 0 1 2 3))
    (local.set $vi_step (v128.const i32x4 4 4 4 4))

    (local.set $i (i32.const 0))
    (local.set $w4 (i32.and (local.get $width) (i32.const -4)))

    (block $break4
      (loop $loop4
        (br_if $break4 (i32.ge_u (local.get $i) (local.get $w4)))

        (local.set $vcx
          (f32x4.add
            (local.get $vx0)
            (f32x4.mul (f32x4.convert_i32x4_u (local.get $vi_vec)) (local.get $vdx))))
        (local.set $vzx     (v128.const f32x4 0.0 0.0 0.0 0.0))
        (local.set $vzy     (v128.const f32x4 0.0 0.0 0.0 0.0))
        (local.set $viter   (v128.const i32x4 0 0 0 0))
        (local.set $vactive (v128.const i32x4 -1 -1 -1 -1))
        (local.set $k       (i32.const 0))

        (block $iter_done
          (loop $iter_loop
            (br_if $iter_done (i32.ge_u (local.get $k) (local.get $max_iter)))

            (local.set $vzx2 (f32x4.mul (local.get $vzx) (local.get $vzx)))
            (local.set $vzy2 (f32x4.mul (local.get $vzy) (local.get $vzy)))
            (local.set $vactive
              (v128.and
                (local.get $vactive)
                (f32x4.le (f32x4.add (local.get $vzx2) (local.get $vzy2)) (local.get $vc4))))

            (br_if $iter_done (i32.eqz (v128.any_true (local.get $vactive))))

            (local.set $viter (i32x4.sub (local.get $viter) (local.get $vactive)))
            (local.set $vnzy
              (f32x4.add
                (f32x4.mul (f32x4.add (local.get $vzx) (local.get $vzx)) (local.get $vzy))
                (local.get $vcy)))
            (local.set $vzx
              (f32x4.add (f32x4.sub (local.get $vzx2) (local.get $vzy2)) (local.get $vcx)))
            (local.set $vzy (local.get $vnzy))

            (local.set $k (i32.add (local.get $k) (i32.const 1)))
            (br $iter_loop)
          )
        )

        (v128.store (local.get $out_iter) (local.get $viter))
        (local.set $out_iter (i32.add (local.get $out_iter) (i32.const 16)))
        (local.set $vi_vec   (i32x4.add (local.get $vi_vec) (local.get $vi_step)))
        (local.set $i        (i32.add (local.get $i) (i32.const 4)))
        (br $loop4)
      )
    )

    ;; Kalan 0..3 piksel skaler kuyruk
    (block $break1
      (loop $loop1
        (br_if $break1 (i32.ge_u (local.get $i) (local.get $width)))
        (local.set $cx (f32.add (local.get $x0) (f32.mul (f32.convert_i32_u (local.get $i)) (local.get $dx))))
        (local.set $zx (f32.const 0.0))
        (local.set $zy (f32.const 0.0))
        (local.set $k  (i32.const 0))

        (block $s_done
          (loop $s_loop
            (br_if $s_done (i32.ge_u (local.get $k) (local.get $max_iter)))
            (local.set $zx2 (f32.mul (local.get $zx) (local.get $zx)))
            (local.set $zy2 (f32.mul (local.get $zy) (local.get $zy)))
            (br_if $s_done (f32.gt (f32.add (local.get $zx2) (local.get $zy2)) (f32.const 4.0)))
            (local.set $nzy (f32.add (f32.mul (f32.add (local.get $zx) (local.get $zx)) (local.get $zy)) (local.get $y0)))
            (local.set $zx  (f32.add (f32.sub (local.get $zx2) (local.get $zy2)) (local.get $cx)))
            (local.set $zy  (local.get $nzy))
            (local.set $k   (i32.add (local.get $k) (i32.const 1)))
            (br $s_loop)
          )
        )

        (i32.store (local.get $out_iter) (local.get $k))
        (local.set $out_iter (i32.add (local.get $out_iter) (i32.const 4)))
        (local.set $i        (i32.add (local.get $i) (i32.const 1)))
        (br $loop1)
      )
    )
  )
)
