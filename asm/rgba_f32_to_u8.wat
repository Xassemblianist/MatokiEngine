;; asm/rgba_f32_to_u8.wat — SoA 4 kanal f32 [0..1] -> paketli RGBA8 (u32) dönüşüm çekirdeği.
;; Clang'ı yenme stratejisi:
;; 1) Sabit vektörlerin (0.0, 1.0, 255.0, 0.5) döngü dışına taşınması (LICM): clang -O2 döngü
;;    gövdesi içinde her iterasyonda 4 adet 16-baytlık `v128.const` sabitini yeniden yüklerken,
;;    elle WAT bunları döngü öncesinde yerel `v128` yazmaçlarına sabitler.
;; 2) Daraltma + yazmaç içi devrik (transpose) paketleme: 4 adet `i32x4` kanalını `i32x4.shl` +
;;    `v128.or` zinciriyle birleştirmek yerine `i16x8.narrow_i32x4_u` (x2) + `i8x16.narrow_i16x8_u`
;;    + tek bir `i8x16.shuffle (0,4,8,12, 1,5,9,13, 2,6,10,14, 3,7,11,15)` ile SoA->AoS dönüşümü yapar.
;; 3) 2x döngü açma (8 piksel = 32 bayt/iterasyon, `offset=0` ve `offset=16`): 5 ayrı işaretçi
;;    ve döngü sayacı güncelleme maliyetini yarıya indirir, yükleme/FMA gecikmesini gizler.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $rgba_f32_to_u8 (export "rgba_f32_to_u8")
    (param $dst i32) (param $r i32) (param $g i32) (param $b i32) (param $a i32) (param $count i32)
    (local $end8 i32)
    (local $end4 i32)
    (local $end1 i32)
    (local $c0 v128)
    (local $c1 v128)
    (local $c255 v128)
    (local $chalf v128)
    (local $ri v128)
    (local $gi v128)
    (local $bi v128)
    (local $ai v128)
    (local $planar v128)
    (local $fv f32)
    (local $ir i32)
    (local $ig i32)
    (local $ib i32)
    (local $ia i32)

    (if (i32.eqz (local.get $count))
      (then (return))
    )

    (local.set $c0    (v128.const f32x4 0.0 0.0 0.0 0.0))
    (local.set $c1    (v128.const f32x4 1.0 1.0 1.0 1.0))
    (local.set $c255  (v128.const f32x4 255.0 255.0 255.0 255.0))
    (local.set $chalf (v128.const f32x4 0.5 0.5 0.5 0.5))

    (local.set $end1 (i32.add (local.get $dst) (i32.shl (local.get $count) (i32.const 2))))
    (local.set $end8 (i32.add (local.get $dst) (i32.shl (i32.and (local.get $count) (i32.const -8)) (i32.const 2))))

    ;; 2x açılmış SIMD döngüsü (8 piksel / iterasyon)
    (block $break8
      (loop $loop8
        (br_if $break8 (i32.ge_u (local.get $dst) (local.get $end8)))

        ;; İlk 4 piksel (offset=0)
        (local.set $ri
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=0 (local.get $r))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $gi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=0 (local.get $g))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $bi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=0 (local.get $b))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $ai
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=0 (local.get $a))))
                (local.get $c255))
              (local.get $chalf))))

        (local.set $planar
          (i8x16.narrow_i16x8_u
            (i16x8.narrow_i32x4_u (local.get $ri) (local.get $gi))
            (i16x8.narrow_i32x4_u (local.get $bi) (local.get $ai))))
        (v128.store offset=0 (local.get $dst)
          (i8x16.shuffle 0 4 8 12 1 5 9 13 2 6 10 14 3 7 11 15
            (local.get $planar) (local.get $planar)))

        ;; İkinci 4 piksel (offset=16)
        (local.set $ri
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=16 (local.get $r))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $gi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=16 (local.get $g))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $bi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=16 (local.get $b))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $ai
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load offset=16 (local.get $a))))
                (local.get $c255))
              (local.get $chalf))))

        (local.set $planar
          (i8x16.narrow_i16x8_u
            (i16x8.narrow_i32x4_u (local.get $ri) (local.get $gi))
            (i16x8.narrow_i32x4_u (local.get $bi) (local.get $ai))))
        (v128.store offset=16 (local.get $dst)
          (i8x16.shuffle 0 4 8 12 1 5 9 13 2 6 10 14 3 7 11 15
            (local.get $planar) (local.get $planar)))

        (local.set $r   (i32.add (local.get $r)   (i32.const 32)))
        (local.set $g   (i32.add (local.get $g)   (i32.const 32)))
        (local.set $b   (i32.add (local.get $b)   (i32.const 32)))
        (local.set $a   (i32.add (local.get $a)   (i32.const 32)))
        (local.set $dst (i32.add (local.get $dst) (i32.const 32)))
        (br $loop8)
      )
    )

    ;; Kalan 4 piksellik grup
    (local.set $end4 (i32.sub (local.get $end1) (i32.shl (i32.and (local.get $count) (i32.const 3)) (i32.const 2))))
    (block $break4
      (loop $loop4
        (br_if $break4 (i32.ge_u (local.get $dst) (local.get $end4)))
        (local.set $ri
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load (local.get $r))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $gi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load (local.get $g))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $bi
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load (local.get $b))))
                (local.get $c255))
              (local.get $chalf))))
        (local.set $ai
          (i32x4.trunc_sat_f32x4_u
            (f32x4.add
              (f32x4.mul
                (f32x4.pmin (local.get $c1) (f32x4.pmax (local.get $c0) (v128.load (local.get $a))))
                (local.get $c255))
              (local.get $chalf))))

        (local.set $planar
          (i8x16.narrow_i16x8_u
            (i16x8.narrow_i32x4_u (local.get $ri) (local.get $gi))
            (i16x8.narrow_i32x4_u (local.get $bi) (local.get $ai))))
        (v128.store (local.get $dst)
          (i8x16.shuffle 0 4 8 12 1 5 9 13 2 6 10 14 3 7 11 15
            (local.get $planar) (local.get $planar)))

        (local.set $r   (i32.add (local.get $r)   (i32.const 16)))
        (local.set $g   (i32.add (local.get $g)   (i32.const 16)))
        (local.set $b   (i32.add (local.get $b)   (i32.const 16)))
        (local.set $a   (i32.add (local.get $a)   (i32.const 16)))
        (local.set $dst (i32.add (local.get $dst) (i32.const 16)))
        (br $loop4)
      )
    )

    ;; Kalan 0..3 piksel skaler kuyruk
    (block $break1
      (loop $loop1
        (br_if $break1 (i32.ge_u (local.get $dst) (local.get $end1)))

        (local.set $fv (f32.load (local.get $r)))
        (local.set $fv (select (local.get $fv) (f32.const 0.0) (f32.gt (local.get $fv) (f32.const 0.0))))
        (local.set $fv (select (local.get $fv) (f32.const 1.0) (f32.lt (local.get $fv) (f32.const 1.0))))
        (local.set $ir (i32.trunc_sat_f32_u (f32.add (f32.mul (local.get $fv) (f32.const 255.0)) (f32.const 0.5))))

        (local.set $fv (f32.load (local.get $g)))
        (local.set $fv (select (local.get $fv) (f32.const 0.0) (f32.gt (local.get $fv) (f32.const 0.0))))
        (local.set $fv (select (local.get $fv) (f32.const 1.0) (f32.lt (local.get $fv) (f32.const 1.0))))
        (local.set $ig (i32.trunc_sat_f32_u (f32.add (f32.mul (local.get $fv) (f32.const 255.0)) (f32.const 0.5))))

        (local.set $fv (f32.load (local.get $b)))
        (local.set $fv (select (local.get $fv) (f32.const 0.0) (f32.gt (local.get $fv) (f32.const 0.0))))
        (local.set $fv (select (local.get $fv) (f32.const 1.0) (f32.lt (local.get $fv) (f32.const 1.0))))
        (local.set $ib (i32.trunc_sat_f32_u (f32.add (f32.mul (local.get $fv) (f32.const 255.0)) (f32.const 0.5))))

        (local.set $fv (f32.load (local.get $a)))
        (local.set $fv (select (local.get $fv) (f32.const 0.0) (f32.gt (local.get $fv) (f32.const 0.0))))
        (local.set $fv (select (local.get $fv) (f32.const 1.0) (f32.lt (local.get $fv) (f32.const 1.0))))
        (local.set $ia (i32.trunc_sat_f32_u (f32.add (f32.mul (local.get $fv) (f32.const 255.0)) (f32.const 0.5))))

        (i32.store (local.get $dst)
          (i32.or
            (i32.or (local.get $ir) (i32.shl (local.get $ig) (i32.const 8)))
            (i32.or (i32.shl (local.get $ib) (i32.const 16)) (i32.shl (local.get $ia) (i32.const 24)))))

        (local.set $r   (i32.add (local.get $r)   (i32.const 4)))
        (local.set $g   (i32.add (local.get $g)   (i32.const 4)))
        (local.set $b   (i32.add (local.get $b)   (i32.const 4)))
        (local.set $a   (i32.add (local.get $a)   (i32.const 4)))
        (local.set $dst (i32.add (local.get $dst) (i32.const 4)))
        (br $loop1)
      )
    )
  )
)
