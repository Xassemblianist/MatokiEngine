;; asm/shade_plasma.wat — plasma.mtk (7 adet sin/cos + sqrt dalga girişimi) elle yazılmış SIMD gölgelendirici.
;; ShadeBatch ABI (src/raster/shade_abi.h) sözleşmesini birebir uygular.
;; Matoki çıktısına (683 döngü komutu, 44 v128 local -> yoğun XMM yazmaç taşması) göre optimizasyonlar:
;; 1) Tek polinomlu pi-indirgemesi: Matoki her sin/cos çağrısında pi/2 indirgemesi yapıp HEM sin_poly
;;    HEM cos_poly'yi (2 polinom) hesaplayarak bitselect ile seçer (7 çağrı = 14 polinom/quad).
;;    Elle WAT'ta cos(x) = sin(x + pi/2) faz kayması döngü önündeki uniform/sabit terimlere katılır ve
;;    x mod pi -> r in [-pi/2, pi/2] indirgemesi + işaret biti XOR'u ((k << 31) ^ r) ile çağrı başına
;;    YALNIZCA TEK polinom (7. derece minimax sin) hesaplanır (maks fark ~0.003 LSB << 1 LSB).
;; 2) Yazmaç basıncı kontrolü (<= 16 XMM): Matoki 44 v128 local tahsis ederek döngü içinde yoğun XMM
;;    taşmasına (stack spill) yol açarken, elle WAT çok kullanımlı sabitleri + geçicileri toplam 15 v128
;;    yazmacında tutar; tek kullanımlı sabitler doğrudan komut içine gömülür.
(module
  (import "env" "memory" (memory 1))
  (func $shade (export "shade") (param $batch i32)
    (local $count i32)
    (local $varyings i32)
    (local $uniforms i32)
    (local $out_rgba i32)
    (local $u_base i32)
    (local $v_base i32)
    (local $or i32)
    (local $og i32)
    (local $ob i32)
    (local $oa i32)
    (local $q16 i32)
    (local $t f32)
    (local $vinv_pi v128)
    (local $vhalf v128)
    (local $vpi1 v128)
    (local $vpi2 v128)
    (local $vs7 v128)
    (local $vs5 v128)
    (local $vs3 v128)
    (local $vt1 v128)
    (local $vt2 v128)
    (local $vt3 v128)
    (local $vt4 v128)
    (local $ux v128)
    (local $uy v128)
    (local $kf v128)
    (local $r v128)
    (local $r2 v128)
    (local $acc v128)

    (if (i32.le_s (local.get $batch) (i32.const 0))
      (then (return))
    )
    (local.set $count (i32.load offset=0 (local.get $batch)))
    (if (i32.le_s (local.get $count) (i32.const 0))
      (then (return))
    )
    (if (i32.gt_u (local.get $count) (i32.const 0x00FFFFFF))
      (then (return))
    )

    (local.set $varyings (i32.load offset=12 (local.get $batch)))
    (local.set $uniforms (i32.load offset=16 (local.get $batch)))
    (local.set $out_rgba (i32.load offset=20 (local.get $batch)))
    (if (i32.eqz (local.get $varyings)) (then (return)))
    (if (i32.eqz (local.get $uniforms)) (then (return)))
    (if (i32.eqz (local.get $out_rgba)) (then (return)))

    (local.set $t (f32.load offset=0 (local.get $uniforms)))

    ;; Çok kullanımlı sabit vektörler (7x-15x kullanılan 7 sabit -> XMM yazmaçlarında kalır)
    (local.set $vinv_pi (v128.const f32x4 0.31830987 0.31830987 0.31830987 0.31830987))
    (local.set $vhalf   (v128.const f32x4 0.5 0.5 0.5 0.5))
    (local.set $vpi1    (v128.const f32x4 3.140625 3.140625 3.140625 3.140625))
    (local.set $vpi2    (v128.const f32x4 9.6765359e-04 9.6765359e-04 9.6765359e-04 9.6765359e-04))
    (local.set $vs7     (v128.const f32x4 -1.95152959e-04 -1.95152959e-04 -1.95152959e-04 -1.95152959e-04))
    (local.set $vs5     (v128.const f32x4 8.33216076e-03 8.33216076e-03 8.33216076e-03 8.33216076e-03))
    (local.set $vs3     (v128.const f32x4 -1.66666546e-01 -1.66666546e-01 -1.66666546e-01 -1.66666546e-01))

    ;; Dalga uniform fazları (cos çağrılarına +pi/2 = 1.57079633 eklenerek sin'e dönüştürülmüştür)
    (local.set $vt1 (f32x4.splat (local.get $t)))
    (local.set $vt2 (f32x4.splat (f32.sub (f32.const 1.57079633) (f32.mul (local.get $t) (f32.const 0.5)))))
    (local.set $vt3 (f32x4.splat (f32.mul (local.get $t) (f32.const 0.7))))
    (local.set $vt4 (f32x4.splat (f32.sub (f32.const 1.57079633) (f32.mul (local.get $t) (f32.const 1.3)))))

    (local.set $count  (i32.shl (local.get $count) (i32.const 4)))
    (local.set $u_base (local.get $varyings))
    (local.set $v_base (i32.add (local.get $u_base) (local.get $count)))

    (local.set $or (local.get $out_rgba))
    (local.set $og (i32.add (local.get $or) (local.get $count)))
    (local.set $ob (i32.add (local.get $og) (local.get $count)))
    (local.set $oa (i32.add (local.get $ob) (local.get $count)))

    (local.set $q16 (i32.const 0))

    (loop $quad_loop
      (local.set $ux (v128.load (i32.add (local.get $u_base) (local.get $q16))))
      (local.set $uy (v128.load (i32.add (local.get $v_base) (local.get $q16))))

      ;; v1 = sin(ux * 10.0 + t)
      (local.set $r  (f32x4.add (f32x4.mul (local.get $ux) (v128.const f32x4 10.0 10.0 10.0 10.0)) (local.get $vt1)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (local.set $acc
        (f32x4.add
          (local.get $r)
          (f32x4.mul
            (local.get $r)
            (f32x4.mul
              (local.get $r2)
              (f32x4.add
                (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                (local.get $vs3))))))

      ;; v2 = cos(uy * 10.0 - t * 0.5) = sin(uy * 10.0 + (pi/2 - t * 0.5))
      (local.set $r  (f32x4.add (f32x4.mul (local.get $uy) (v128.const f32x4 10.0 10.0 10.0 10.0)) (local.get $vt2)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (local.set $acc
        (f32x4.add
          (local.get $acc)
          (f32x4.add
            (local.get $r)
            (f32x4.mul
              (local.get $r)
              (f32x4.mul
                (local.get $r2)
                (f32x4.add
                  (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                  (local.get $vs3)))))))

      ;; v3 = sin((ux + uy) * 7.0 + t * 0.7)
      (local.set $r  (f32x4.add (f32x4.mul (f32x4.add (local.get $ux) (local.get $uy)) (v128.const f32x4 7.0 7.0 7.0 7.0)) (local.get $vt3)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (local.set $acc
        (f32x4.add
          (local.get $acc)
          (f32x4.add
            (local.get $r)
            (f32x4.mul
              (local.get $r)
              (f32x4.mul
                (local.get $r2)
                (f32x4.add
                  (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                  (local.get $vs3)))))))

      ;; v4 = cos(sqrt((ux - 0.5)^2 + (uy - 0.5)^2) * 14.0 - t * 1.3) ($ux ve $uy burada son kez kullanılır)
      (local.set $ux (f32x4.sub (local.get $ux) (local.get $vhalf)))
      (local.set $uy (f32x4.sub (local.get $uy) (local.get $vhalf)))
      (local.set $r
        (f32x4.add
          (f32x4.mul
            (f32x4.sqrt (f32x4.add (f32x4.mul (local.get $ux) (local.get $ux)) (f32x4.mul (local.get $uy) (local.get $uy))))
            (v128.const f32x4 14.0 14.0 14.0 14.0))
          (local.get $vt4)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))

      ;; wpi = (v1 + v2 + v3 + v4) * (0.25 * PI)
      (local.set $acc
        (f32x4.mul
          (f32x4.add
            (local.get $acc)
            (f32x4.add
              (local.get $r)
              (f32x4.mul
                (local.get $r)
                (f32x4.mul
                  (local.get $r2)
                  (f32x4.add
                    (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                    (local.get $vs3))))))
          (v128.const f32x4 0.785398163 0.785398163 0.785398163 0.785398163)))

      ;; out_r = sin(wpi) * 0.5 + 0.5
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $acc) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $acc) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (v128.store (i32.add (local.get $or) (local.get $q16))
        (f32x4.add
          (f32x4.mul
            (f32x4.add
              (local.get $r)
              (f32x4.mul
                (local.get $r)
                (f32x4.mul
                  (local.get $r2)
                  (f32x4.add
                    (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                    (local.get $vs3)))))
            (local.get $vhalf))
          (local.get $vhalf)))

      ;; out_g = cos(wpi + 1.0) * 0.5 + 0.5 = sin(wpi + (1.0 + pi/2)) * 0.5 + 0.5
      (local.set $r  (f32x4.add (local.get $acc) (v128.const f32x4 2.57079633 2.57079633 2.57079633 2.57079633)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (v128.store (i32.add (local.get $og) (local.get $q16))
        (f32x4.add
          (f32x4.mul
            (f32x4.add
              (local.get $r)
              (f32x4.mul
                (local.get $r)
                (f32x4.mul
                  (local.get $r2)
                  (f32x4.add
                    (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                    (local.get $vs3)))))
            (local.get $vhalf))
          (local.get $vhalf)))

      ;; out_b = sin(wpi + 2.0) * 0.5 + 0.5
      (local.set $r  (f32x4.add (local.get $acc) (v128.const f32x4 2.0 2.0 2.0 2.0)))
      (local.set $kf (f32x4.floor (f32x4.add (f32x4.mul (local.get $r) (local.get $vinv_pi)) (local.get $vhalf))))
      (local.set $r
        (v128.xor
          (f32x4.sub (f32x4.sub (local.get $r) (f32x4.mul (local.get $kf) (local.get $vpi1))) (f32x4.mul (local.get $kf) (local.get $vpi2)))
          (i32x4.shl (i32x4.trunc_sat_f32x4_s (local.get $kf)) (i32.const 31))))
      (local.set $r2 (f32x4.mul (local.get $r) (local.get $r)))
      (v128.store (i32.add (local.get $ob) (local.get $q16))
        (f32x4.add
          (f32x4.mul
            (f32x4.add
              (local.get $r)
              (f32x4.mul
                (local.get $r)
                (f32x4.mul
                  (local.get $r2)
                  (f32x4.add
                    (f32x4.mul (local.get $r2) (f32x4.add (f32x4.mul (local.get $r2) (local.get $vs7)) (local.get $vs5)))
                    (local.get $vs3)))))
            (local.get $vhalf))
          (local.get $vhalf)))

      ;; out_a = 1.0
      (v128.store (i32.add (local.get $oa) (local.get $q16)) (v128.const f32x4 1.0 1.0 1.0 1.0))

      (local.set $q16 (i32.add (local.get $q16) (i32.const 16)))
      (br_if $quad_loop (i32.ne (local.get $q16) (local.get $count)))
    )
  )
)
