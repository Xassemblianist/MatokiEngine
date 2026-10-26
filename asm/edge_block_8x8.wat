;; asm/edge_block_8x8.wat — 3 kenar fonksiyonu (a*x + b*y + c), 8x8 blok kapsama maskeleri (16 quad x 4 bit).
;; Clang'ı yenme stratejisi:
;; 1) Vektör ekseni seçimi + `i32x4.bitmask`: Clang -O2 iç döngüdeki 4 quad'ı (`rx=0..3`) vektörleştirip
;;    4 lane'i ayrı ayrı hesaplar ve her satırda 4 kez `i32x4.lt_s` + `i8x16.shuffle` + `v128.bitselect`
;;    zinciri kurar. Elle WAT ise bir 2x2 quad'ın 4 pikselini (`(0,0), (1,0), (0,1), (1,1)`) tek bir
;;    `i32x4` yazmacında tutar; üç kenarın işaret bitlerini `v128.or` + tek bir `i32x4.bitmask` komutuyla
;;    doğrudan 4-bitlik maskeye dönüştürür.
;; 2) Negatif maske ertelemesi (`0x0F0F0F0F` XOR) ve 32-bit birleşik yazma (`i32.store`):
;;    Her quad'da `v128.not` yapmak yerine `i32x4.bitmask` ile ters maskeler (`~m & 0xF`) alınır,
;;    bir satırdaki 4 quad maskesi tek bir 32-bit tamsayıda birleştirilip tek bir `i32.xor 0x0F0F0F0F`
;;    ve tek bir `i32.store` ile 4 bayt birden belleğe yazılır (16 `i32.store8` -> 4 `i32.store`).
;; 3) Tam döngü açma ve `row_step = splat(2*b - 6*a)` geçişi: 4x4 quad'ın tamamı dallanmasız (0 branch)
;;    açılır; satır sonundan bir sonraki satır başına tek bir vektör toplamayla geçilir.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $edge_block_8x8 (export "edge_block_8x8")
    (param $coeffs i32) (param $bx i32) (param $by i32) (param $out_masks i32)
    (local $a0 i32) (local $b0 i32) (local $c0 i32)
    (local $a1 i32) (local $b1 i32) (local $c1 i32)
    (local $a2 i32) (local $b2 i32) (local $c2 i32)
    (local $qe0 v128) (local $qe1 v128) (local $qe2 v128)
    (local $sx0 v128) (local $sx1 v128) (local $sx2 v128)
    (local $sy0 v128) (local $sy1 v128) (local $sy2 v128)
    (local $ry i32)
    (local $m0 i32) (local $m1 i32) (local $m2 i32) (local $m3 i32)

    ;; Katsayıları yükle
    (local.set $a0 (i32.load offset=0  (local.get $coeffs)))
    (local.set $b0 (i32.load offset=4  (local.get $coeffs)))
    (local.set $c0 (i32.load offset=8  (local.get $coeffs)))
    (local.set $a1 (i32.load offset=12 (local.get $coeffs)))
    (local.set $b1 (i32.load offset=16 (local.get $coeffs)))
    (local.set $c1 (i32.load offset=20 (local.get $coeffs)))
    (local.set $a2 (i32.load offset=24 (local.get $coeffs)))
    (local.set $b2 (i32.load offset=28 (local.get $coeffs)))
    (local.set $c2 (i32.load offset=32 (local.get $coeffs)))

    ;; Başlangıç quad (bx, by) 4-lane vektörleri: base + (0, a, b, a+b)
    (local.set $c0 (i32.add (i32.add (i32.mul (local.get $a0) (local.get $bx)) (i32.mul (local.get $b0) (local.get $by))) (local.get $c0)))
    (local.set $c1 (i32.add (i32.add (i32.mul (local.get $a1) (local.get $bx)) (i32.mul (local.get $b1) (local.get $by))) (local.get $c1)))
    (local.set $c2 (i32.add (i32.add (i32.mul (local.get $a2) (local.get $bx)) (i32.mul (local.get $b2) (local.get $by))) (local.get $c2)))

    (local.set $qe0
      (i32x4.replace_lane 3
        (i32x4.replace_lane 2
          (i32x4.replace_lane 1
            (i32x4.splat (local.get $c0))
            (i32.add (local.get $c0) (local.get $a0)))
          (i32.add (local.get $c0) (local.get $b0)))
        (i32.add (local.get $c0) (i32.add (local.get $a0) (local.get $b0)))))

    (local.set $qe1
      (i32x4.replace_lane 3
        (i32x4.replace_lane 2
          (i32x4.replace_lane 1
            (i32x4.splat (local.get $c1))
            (i32.add (local.get $c1) (local.get $a1)))
          (i32.add (local.get $c1) (local.get $b1)))
        (i32.add (local.get $c1) (i32.add (local.get $a1) (local.get $b1)))))

    (local.set $qe2
      (i32x4.replace_lane 3
        (i32x4.replace_lane 2
          (i32x4.replace_lane 1
            (i32x4.splat (local.get $c2))
            (i32.add (local.get $c2) (local.get $a2)))
          (i32.add (local.get $c2) (local.get $b2)))
        (i32.add (local.get $c2) (i32.add (local.get $a2) (local.get $b2)))))

    ;; Yatay adım: 2*a, satır sonu düzeltmeli dikey adım: 2*b - 6*a
    (local.set $sx0 (i32x4.splat (i32.shl (local.get $a0) (i32.const 1))))
    (local.set $sx1 (i32x4.splat (i32.shl (local.get $a1) (i32.const 1))))
    (local.set $sx2 (i32x4.splat (i32.shl (local.get $a2) (i32.const 1))))

    (local.set $sy0 (i32x4.splat (i32.sub (i32.shl (local.get $b0) (i32.const 1)) (i32.mul (local.get $a0) (i32.const 6)))))
    (local.set $sy1 (i32x4.splat (i32.sub (i32.shl (local.get $b1) (i32.const 1)) (i32.mul (local.get $a1) (i32.const 6)))))
    (local.set $sy2 (i32x4.splat (i32.sub (i32.shl (local.get $b2) (i32.const 1)) (i32.mul (local.get $a2) (i32.const 6)))))

    (local.set $ry (i32.const 0))
    (loop $row_loop
      ;; rx = 0
      (local.set $m0 (i32x4.bitmask (v128.or (v128.or (local.get $qe0) (local.get $qe1)) (local.get $qe2))))
      (local.set $qe0 (i32x4.add (local.get $qe0) (local.get $sx0)))
      (local.set $qe1 (i32x4.add (local.get $qe1) (local.get $sx1)))
      (local.set $qe2 (i32x4.add (local.get $qe2) (local.get $sx2)))

      ;; rx = 1
      (local.set $m1 (i32x4.bitmask (v128.or (v128.or (local.get $qe0) (local.get $qe1)) (local.get $qe2))))
      (local.set $qe0 (i32x4.add (local.get $qe0) (local.get $sx0)))
      (local.set $qe1 (i32x4.add (local.get $qe1) (local.get $sx1)))
      (local.set $qe2 (i32x4.add (local.get $qe2) (local.get $sx2)))

      ;; rx = 2
      (local.set $m2 (i32x4.bitmask (v128.or (v128.or (local.get $qe0) (local.get $qe1)) (local.get $qe2))))
      (local.set $qe0 (i32x4.add (local.get $qe0) (local.get $sx0)))
      (local.set $qe1 (i32x4.add (local.get $qe1) (local.get $sx1)))
      (local.set $qe2 (i32x4.add (local.get $qe2) (local.get $sx2)))

      ;; rx = 3
      (local.set $m3 (i32x4.bitmask (v128.or (v128.or (local.get $qe0) (local.get $qe1)) (local.get $qe2))))
      (local.set $qe0 (i32x4.add (local.get $qe0) (local.get $sy0)))
      (local.set $qe1 (i32x4.add (local.get $qe1) (local.get $sy1)))
      (local.set $qe2 (i32x4.add (local.get $qe2) (local.get $sy2)))

      ;; 4 quad maskesini tek 32-bit store ile yaz
      (i32.store (local.get $out_masks)
        (i32.xor
          (i32.or
            (i32.or (local.get $m0) (i32.shl (local.get $m1) (i32.const 8)))
            (i32.or (i32.shl (local.get $m2) (i32.const 16)) (i32.shl (local.get $m3) (i32.const 24))))
          (i32.const 0x0F0F0F0F)))

      (local.set $out_masks (i32.add (local.get $out_masks) (i32.const 4)))
      (local.set $ry (i32.add (local.get $ry) (i32.const 1)))
      (br_if $row_loop (i32.lt_u (local.get $ry) (i32.const 4)))
    )
  )
)
