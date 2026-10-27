;; asm/shade_lit3d.wat — lit3d.mtk (mipmap + bilinear doku + Lambert ışık + mesafe sisi) elle yazılmış WAT.
;; ShadeBatch ABI (src/raster/shade_abi.h) sözleşmesini birebir uygular.
;; Matoki çıktısına (670 döngü komutu) göre yapılan başlıca optimizasyonlar:
;; 1) Quad-tekdüze (quad-uniform) ve dalsız (branchless) mipmap seviyesi: dFdx(uv) ve dFdy(uv) 2x2 quad
;;    içinde tüm lane'lerde aynıdır. Matoki bunu varying (v128) olarak takip ettiği için 8 adet i8x16.shuffle
;;    ve değişken kaydırma miktarlı 3 adet 4-lane skalerleştirilmiş kaydırma (90 komut!) üretir.
;;    Elle WAT'ta 4 türev [dudx, dudy, dvdx, dvdy] tek bir SIMD vektöründe paralel hesaplanır, ardından
;;    rho2, mip seviyesi (select ile dalsız), base_k, wk ve hk tamamen skaler (f32/i32) hesaplanır!
;; 2) Yazılım gather aşamasında düz yığın sırasıyla (her adımda 1 geçici GPR) v128.load32_zero +
;;    i32.load + i32x4.replace_lane 1..3 kullanılır.
;; 3) 0..255 aralığındaki 16 texel kanalı için x86'da 6 komuta açılan f32x4.convert_i32x4_u yerine tek
;;    komutluk (cvtdq2ps) f32x4.convert_i32x4_s kullanılır (~80 x86 komutu tasarruf).
;; 4) (1/255.0) ölçeği her kanalda 4 komşu texel için ayrı ayrı (16 çarpma) uygulanmak yerine
;;    döngü önünde base rengiyle birleştirilerek (base * (1/255)) bilinear lerp sonrasında kanal başına
;;    1 kez uygulanır (20 çarpma -> 4 çarpma; fark <= 2 ULP = 0.000045 LSB << 1 LSB).
;; 5) Işık (l) ve sis (f) doku örneklemesinden önce hesaplanıp tek yazmaçta tutularak (nx, ny, nz, inv_len
;;    hemen öldürülür) ve kanallar sıralı işlenerek XMM yazmaç taşması önlenir.
(module
  (import "env" "memory" (memory 1))
  (func $shade (export "shade") (param $batch i32)
    (local $count i32)
    (local $varyings i32)
    (local $uniforms i32)
    (local $out_rgba i32)
    (local $textures i32)
    (local $px_base i32)
    (local $w i32)
    (local $h i32)
    (local $levels i32)
    (local $max_lvl i32)
    (local $wh4 i32)
    (local $u_base i32)
    (local $v_base i32)
    (local $nx_base i32)
    (local $ny_base i32)
    (local $nz_base i32)
    (local $dep_base i32)
    (local $or i32)
    (local $og i32)
    (local $ob i32)
    (local $oa i32)
    (local $q16 i32)
    (local $lvl i32)
    (local $wk i32)
    (local $hk i32)
    (local $rho2 f32)
    (local $vwhf v128)
    (local $vlx v128)
    (local $vly v128)
    (local $vlz v128)
    (local $vbase_r v128)
    (local $vbase_g v128)
    (local $vbase_b v128)
    (local $vbase_a v128)
    (local $vfog_r v128)
    (local $vfog_g v128)
    (local $vfog_b v128)
    (local $vfog_a v128)
    (local $vzero v128)
    (local $vone v128)
    (local $vhalf v128)
    (local $vc03 v128)
    (local $vc07 v128)
    (local $vmask_ff v128)
    (local $vc1 v128)
    (local $vinv255 v128)
    (local $vu v128)
    (local $vv v128)
    (local $nx v128)
    (local $ny v128)
    (local $nz v128)
    (local $vl v128)
    (local $vf v128)
    (local $fu v128)
    (local $fv v128)
    (local $x0f v128)
    (local $y0f v128)
    (local $ax v128)
    (local $ay v128)
    (local $x0i v128)
    (local $y0i v128)
    (local $vwk v128)
    (local $vwk_mask v128)
    (local $vhk_mask v128)
    (local $vpx_lvl v128)
    (local $x0 v128)
    (local $x1 v128)
    (local $r0 v128)
    (local $r1 v128)
    (local $addr v128)
    (local $p00 v128)
    (local $p10 v128)
    (local $p01 v128)
    (local $p11 v128)
    (local $c00 v128)
    (local $c01 v128)
    (local $top v128)
    (local $bot v128)

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
    (local.set $textures (i32.load offset=24 (local.get $batch)))
    (if (i32.eqz (local.get $varyings)) (then (return)))
    (if (i32.eqz (local.get $uniforms)) (then (return)))
    (if (i32.eqz (local.get $out_rgba)) (then (return)))
    (if (i32.eqz (local.get $textures)) (then (return)))

    (local.set $px_base (i32.load offset=0  (local.get $textures)))
    (local.set $w       (i32.load offset=4  (local.get $textures)))
    (local.set $h       (i32.load offset=8  (local.get $textures)))
    (local.set $levels  (i32.load offset=28 (local.get $textures)))
    (if (i32.eqz (local.get $px_base))          (then (return)))
    (if (i32.le_s (local.get $w) (i32.const 0)) (then (return)))
    (if (i32.le_s (local.get $h) (i32.const 0)) (then (return)))

    ;; $vwhf = [wf, wf, hf, hf]
    (local.set $vwhf
      (i8x16.shuffle 0 1 2 3 0 1 2 3 16 17 18 19 16 17 18 19
        (f32x4.splat (f32.convert_i32_s (local.get $w)))
        (f32x4.splat (f32.convert_i32_s (local.get $h)))))
    (local.set $wh4 (i32.shl (i32.mul (local.get $w) (local.get $h)) (i32.const 2)))

    ;; max_lvl = max(levels, 1) - 1, güvenlik için <= ctz(w) ile sınırla
    (if (i32.le_s (local.get $levels) (i32.const 1))
      (then (local.set $max_lvl (i32.const 0)))
      (else
        (local.set $max_lvl (i32.sub (local.get $levels) (i32.const 1)))
        (if (i32.gt_s (local.get $max_lvl) (i32.ctz (local.get $w)))
          (then (local.set $max_lvl (i32.ctz (local.get $w))))
        )
      )
    )

    (local.set $vzero    (v128.const f32x4 0.0 0.0 0.0 0.0))
    (local.set $vone     (v128.const f32x4 1.0 1.0 1.0 1.0))
    (local.set $vhalf    (v128.const f32x4 0.5 0.5 0.5 0.5))
    (local.set $vc03     (v128.const f32x4 0.3 0.3 0.3 0.3))
    (local.set $vc07     (v128.const f32x4 0.7 0.7 0.7 0.7))
    (local.set $vmask_ff (v128.const i32x4 255 255 255 255))
    (local.set $vc1      (v128.const i32x4 1 1 1 1))
    (local.set $vinv255  (v128.const f32x4 0x1.010102p-8 0x1.010102p-8 0x1.010102p-8 0x1.010102p-8))

    ;; Uniform'ları yükle (base rengi döngü önünde 1/255.0 ile ölçeklenir)
    (local.set $vlx     (v128.load32_splat offset=0  (local.get $uniforms)))
    (local.set $vly     (v128.load32_splat offset=4  (local.get $uniforms)))
    (local.set $vlz     (v128.load32_splat offset=8  (local.get $uniforms)))
    (local.set $vbase_r (f32x4.mul (v128.load32_splat offset=16 (local.get $uniforms)) (local.get $vinv255)))
    (local.set $vbase_g (f32x4.mul (v128.load32_splat offset=20 (local.get $uniforms)) (local.get $vinv255)))
    (local.set $vbase_b (f32x4.mul (v128.load32_splat offset=24 (local.get $uniforms)) (local.get $vinv255)))
    (local.set $vbase_a (f32x4.mul (v128.load32_splat offset=28 (local.get $uniforms)) (local.get $vinv255)))
    (local.set $vfog_r  (v128.load32_splat offset=32 (local.get $uniforms)))
    (local.set $vfog_g  (v128.load32_splat offset=36 (local.get $uniforms)))
    (local.set $vfog_b  (v128.load32_splat offset=40 (local.get $uniforms)))
    (local.set $vfog_a  (v128.load32_splat offset=44 (local.get $uniforms)))

    (local.set $count    (i32.shl (local.get $count) (i32.const 4)))
    (local.set $u_base   (local.get $varyings))
    (local.set $v_base   (i32.add (local.get $u_base)  (local.get $count)))
    (local.set $nx_base  (i32.add (local.get $v_base)  (local.get $count)))
    (local.set $ny_base  (i32.add (local.get $nx_base) (local.get $count)))
    (local.set $nz_base  (i32.add (local.get $ny_base) (local.get $count)))
    (local.set $dep_base (i32.add (local.get $nz_base) (local.get $count)))

    (local.set $or (local.get $out_rgba))
    (local.set $og (i32.add (local.get $or) (local.get $count)))
    (local.set $ob (i32.add (local.get $og) (local.get $count)))
    (local.set $oa (i32.add (local.get $ob) (local.get $count)))

    (local.set $q16 (i32.const 0))

    (loop $quad_loop
      ;; 1. Aydınlatma: n = normalize(nrm), l = 0.3 + 0.7 * max(dot(n, light_dir), 0.0)
      (local.set $nx (v128.load (i32.add (local.get $nx_base) (local.get $q16))))
      (local.set $ny (v128.load (i32.add (local.get $ny_base) (local.get $q16))))
      (local.set $nz (v128.load (i32.add (local.get $nz_base) (local.get $q16))))
      (local.set $vl
        (f32x4.div
          (local.get $vone)
          (f32x4.sqrt
            (f32x4.add
              (f32x4.add (f32x4.mul (local.get $nx) (local.get $nx)) (f32x4.mul (local.get $ny) (local.get $ny)))
              (f32x4.mul (local.get $nz) (local.get $nz))))))
      (local.set $vl
        (f32x4.add
          (local.get $vc03)
          (f32x4.mul
            (f32x4.max
              (f32x4.add
                (f32x4.add
                  (f32x4.mul (f32x4.mul (local.get $nx) (local.get $vl)) (local.get $vlx))
                  (f32x4.mul (f32x4.mul (local.get $ny) (local.get $vl)) (local.get $vly)))
                (f32x4.mul (f32x4.mul (local.get $nz) (local.get $vl)) (local.get $vlz)))
              (local.get $vzero))
            (local.get $vc07))))

      ;; 2. Sis faktörü: f = clamp(depth * fog.a, 0.0, 1.0)
      (local.set $vf
        (f32x4.min
          (f32x4.max
            (f32x4.mul (v128.load (i32.add (local.get $dep_base) (local.get $q16))) (local.get $vfog_a))
            (local.get $vzero))
          (local.get $vone)))

      ;; 3. Quad-tekdüze dalsız mipmap seviyesi: [dudx, dudy, dvdx, dvdy] tek SIMD vektöründe
      (local.set $vu (v128.load (i32.add (local.get $u_base) (local.get $q16))))
      (local.set $vv (v128.load (i32.add (local.get $v_base) (local.get $q16))))
      (local.set $top
        (f32x4.mul
          (f32x4.sub
            (i8x16.shuffle 4 5 6 7 8 9 10 11 20 21 22 23 24 25 26 27 (local.get $vu) (local.get $vv))
            (i8x16.shuffle 0 1 2 3 0 1 2 3 16 17 18 19 16 17 18 19 (local.get $vu) (local.get $vv)))
          (local.get $vwhf)))
      (local.set $top (f32x4.mul (local.get $top) (local.get $top)))
      (local.set $top
        (f32x4.add
          (local.get $top)
          (i8x16.shuffle 8 9 10 11 12 13 14 15 0 1 2 3 4 5 6 7 (local.get $top) (local.get $top))))
      (local.set $rho2
        (f32.max (f32x4.extract_lane 0 (local.get $top)) (f32x4.extract_lane 1 (local.get $top))))

      ;; Dalsız (branchless) mip seviyesi: rho2 >= 1.0 ise ((bits(rho2)>>>23)-127)>>1, değilse 0
      (local.set $lvl
        (i32.and
          (i32.shr_s
            (i32.sub (i32.shr_u (i32.reinterpret_f32 (local.get $rho2)) (i32.const 23)) (i32.const 127))
            (i32.const 1))
          (i32.sub (i32.const 0) (f32.ge (local.get $rho2) (f32.const 1.0)))))
      (local.set $lvl
        (select (local.get $max_lvl) (local.get $lvl) (i32.gt_s (local.get $lvl) (local.get $max_lvl))))

      (local.set $wk  (i32.shr_s (local.get $w) (local.get $lvl)))
      (local.set $hk  (i32.shr_s (local.get $h) (local.get $lvl)))
      (local.set $vwk (i32x4.splat (local.get $wk)))
      (local.set $vpx_lvl
        (i32x4.splat
          (i32.add
            (local.get $px_base)
            (i32.shl
              (i32.mul
                (i32.sub (local.get $wh4) (i32.shr_u (local.get $wh4) (i32.shl (local.get $lvl) (i32.const 1))))
                (i32.const -1431655765))
              (i32.const 2)))))

      ;; 4. Çift doğrusal (bilinear) koordinatlar ve 4 komşu texel gather (düz yığın sırası)
      (local.set $fu  (f32x4.sub (f32x4.mul (local.get $vu) (f32x4.splat (f32.convert_i32_s (local.get $wk)))) (local.get $vhalf)))
      (local.set $fv  (f32x4.sub (f32x4.mul (local.get $vv) (f32x4.splat (f32.convert_i32_s (local.get $hk)))) (local.get $vhalf)))
      (local.set $x0f (f32x4.floor (local.get $fu)))
      (local.set $y0f (f32x4.floor (local.get $fv)))
      (local.set $ax  (f32x4.sub (local.get $fu) (local.get $x0f)))
      (local.set $ay  (f32x4.sub (local.get $fv) (local.get $y0f)))

      (local.set $x0i      (i32x4.trunc_sat_f32x4_s (local.get $x0f)))
      (local.set $y0i      (i32x4.trunc_sat_f32x4_s (local.get $y0f)))
      (local.set $vwk_mask (i32x4.splat (i32.sub (local.get $wk) (i32.const 1))))
      (local.set $vhk_mask (i32x4.splat (i32.sub (local.get $hk) (i32.const 1))))

      (local.set $x0 (v128.and (local.get $x0i) (local.get $vwk_mask)))
      (local.set $x1 (v128.and (i32x4.add (local.get $x0i) (local.get $vc1)) (local.get $vwk_mask)))
      (local.set $r0 (i32x4.mul (v128.and (local.get $y0i) (local.get $vhk_mask)) (local.get $vwk)))
      (local.set $r1 (i32x4.mul (v128.and (i32x4.add (local.get $y0i) (local.get $vc1)) (local.get $vhk_mask)) (local.get $vwk)))

      ;; Gather p00
      local.get $vpx_lvl
      local.get $r0
      local.get $x0
      i32x4.add
      i32.const 2
      i32x4.shl
      i32x4.add
      local.tee $addr
      i32x4.extract_lane 0
      v128.load32_zero
      local.get $addr
      i32x4.extract_lane 1
      i32.load
      i32x4.replace_lane 1
      local.get $addr
      i32x4.extract_lane 2
      i32.load
      i32x4.replace_lane 2
      local.get $addr
      i32x4.extract_lane 3
      i32.load
      i32x4.replace_lane 3
      local.set $p00

      ;; Gather p10
      local.get $vpx_lvl
      local.get $r0
      local.get $x1
      i32x4.add
      i32.const 2
      i32x4.shl
      i32x4.add
      local.tee $addr
      i32x4.extract_lane 0
      v128.load32_zero
      local.get $addr
      i32x4.extract_lane 1
      i32.load
      i32x4.replace_lane 1
      local.get $addr
      i32x4.extract_lane 2
      i32.load
      i32x4.replace_lane 2
      local.get $addr
      i32x4.extract_lane 3
      i32.load
      i32x4.replace_lane 3
      local.set $p10

      ;; Gather p01
      local.get $vpx_lvl
      local.get $r1
      local.get $x0
      i32x4.add
      i32.const 2
      i32x4.shl
      i32x4.add
      local.tee $addr
      i32x4.extract_lane 0
      v128.load32_zero
      local.get $addr
      i32x4.extract_lane 1
      i32.load
      i32x4.replace_lane 1
      local.get $addr
      i32x4.extract_lane 2
      i32.load
      i32x4.replace_lane 2
      local.get $addr
      i32x4.extract_lane 3
      i32.load
      i32x4.replace_lane 3
      local.set $p01

      ;; Gather p11
      local.get $vpx_lvl
      local.get $r1
      local.get $x1
      i32x4.add
      i32.const 2
      i32x4.shl
      i32x4.add
      local.tee $addr
      i32x4.extract_lane 0
      v128.load32_zero
      local.get $addr
      i32x4.extract_lane 1
      i32.load
      i32x4.replace_lane 1
      local.get $addr
      i32x4.extract_lane 2
      i32.load
      i32x4.replace_lane 2
      local.get $addr
      i32x4.extract_lane 3
      i32.load
      i32x4.replace_lane 3
      local.set $p11

      ;; 5. Kanal R (bit 0..7): convert_i32x4_s (cvtdq2ps) -> bilinear lerp -> * (base.r / 255) * l -> mix(fog.r, f)
      (local.set $c00 (f32x4.convert_i32x4_s (v128.and (local.get $p00) (local.get $vmask_ff))))
      (local.set $c01 (f32x4.convert_i32x4_s (v128.and (local.get $p01) (local.get $vmask_ff))))
      (local.set $top
        (f32x4.add
          (local.get $c00)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (local.get $p10) (local.get $vmask_ff))) (local.get $c00))
            (local.get $ax))))
      (local.set $bot
        (f32x4.add
          (local.get $c01)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (local.get $p11) (local.get $vmask_ff))) (local.get $c01))
            (local.get $ax))))
      (local.set $top
        (f32x4.mul
          (f32x4.mul
            (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $bot) (local.get $top)) (local.get $ay)))
            (local.get $vbase_r))
          (local.get $vl)))
      (v128.store (i32.add (local.get $or) (local.get $q16))
        (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $vfog_r) (local.get $top)) (local.get $vf))))

      ;; 6. Kanal G (bit 8..15): convert_i32x4_s -> bilinear lerp -> * (base.g / 255) * l -> mix(fog.g, f)
      (local.set $c00 (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p00) (i32.const 8)) (local.get $vmask_ff))))
      (local.set $c01 (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p01) (i32.const 8)) (local.get $vmask_ff))))
      (local.set $top
        (f32x4.add
          (local.get $c00)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p10) (i32.const 8)) (local.get $vmask_ff))) (local.get $c00))
            (local.get $ax))))
      (local.set $bot
        (f32x4.add
          (local.get $c01)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p11) (i32.const 8)) (local.get $vmask_ff))) (local.get $c01))
            (local.get $ax))))
      (local.set $top
        (f32x4.mul
          (f32x4.mul
            (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $bot) (local.get $top)) (local.get $ay)))
            (local.get $vbase_g))
          (local.get $vl)))
      (v128.store (i32.add (local.get $og) (local.get $q16))
        (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $vfog_g) (local.get $top)) (local.get $vf))))

      ;; 7. Kanal B (bit 16..23): convert_i32x4_s -> bilinear lerp -> * (base.b / 255) * l -> mix(fog.b, f)
      (local.set $c00 (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p00) (i32.const 16)) (local.get $vmask_ff))))
      (local.set $c01 (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p01) (i32.const 16)) (local.get $vmask_ff))))
      (local.set $top
        (f32x4.add
          (local.get $c00)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p10) (i32.const 16)) (local.get $vmask_ff))) (local.get $c00))
            (local.get $ax))))
      (local.set $bot
        (f32x4.add
          (local.get $c01)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (v128.and (i32x4.shr_u (local.get $p11) (i32.const 16)) (local.get $vmask_ff))) (local.get $c01))
            (local.get $ax))))
      (local.set $top
        (f32x4.mul
          (f32x4.mul
            (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $bot) (local.get $top)) (local.get $ay)))
            (local.get $vbase_b))
          (local.get $vl)))
      (v128.store (i32.add (local.get $ob) (local.get $q16))
        (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $vfog_b) (local.get $top)) (local.get $vf))))

      ;; 8. Kanal A (bit 24..31): convert_i32x4_s -> bilinear lerp -> * (base.a / 255)
      (local.set $c00 (f32x4.convert_i32x4_s (i32x4.shr_u (local.get $p00) (i32.const 24))))
      (local.set $c01 (f32x4.convert_i32x4_s (i32x4.shr_u (local.get $p01) (i32.const 24))))
      (local.set $top
        (f32x4.add
          (local.get $c00)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (i32x4.shr_u (local.get $p10) (i32.const 24))) (local.get $c00))
            (local.get $ax))))
      (local.set $bot
        (f32x4.add
          (local.get $c01)
          (f32x4.mul
            (f32x4.sub (f32x4.convert_i32x4_s (i32x4.shr_u (local.get $p11) (i32.const 24))) (local.get $c01))
            (local.get $ax))))
      (v128.store (i32.add (local.get $oa) (local.get $q16))
        (f32x4.mul
          (f32x4.add (local.get $top) (f32x4.mul (f32x4.sub (local.get $bot) (local.get $top)) (local.get $ay)))
          (local.get $vbase_a)))

      (local.set $q16 (i32.add (local.get $q16) (i32.const 16)))
      (br_if $quad_loop (i32.ne (local.get $q16) (local.get $count)))
    )
  )
)
