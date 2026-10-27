;; asm/shade_sprite.wat — sprite.mtk (en yakın komşu doku + alfa eşiği discard + tint) elle yazılmış WAT.
;; ShadeBatch ABI (src/raster/shade_abi.h) sözleşmesini birebir uygular.
;; Matoki çıktısına göre yapılan optimizasyonlar:
;; 1) Alfa eşiği (c.a < 0.01 <=> (px >>> 24) < 3) kayan noktaya çevirmeden ve (1/255) ile
;;    çarpmadan doğrudan tam sayı olarak (i32x4.lt_s(a_i, 3)) sınanır.
;; 2) (1/255.0) ölçeği döngü öncesinde uniform tint vektörüyle birleştirilerek (tint * (1/255))
;;    döngü içindeki kanal başına 2 adet f32x4.mul işlemi 1 adede indirilir (fark <= 1 ULP << 1 LSB).
;; 3) 0..255 aralığındaki kanallar için x86'da 6 komuta açılan f32x4.convert_i32x4_u yerine tek komutluk
;;    (cvtdq2ps) işaretli f32x4.convert_i32x4_s ve düz yığın sırasıyla tek GPR'li gather kullanılır.
(module
  (import "env" "memory" (memory 1))
  (func $shade (export "shade") (param $batch i32)
    (local $count i32)
    (local $quads i32)
    (local $varyings i32)
    (local $uniforms i32)
    (local $out_rgba i32)
    (local $textures i32)
    (local $px_base i32)
    (local $w i32)
    (local $h i32)
    (local $u_base i32)
    (local $v_base i32)
    (local $or i32)
    (local $og i32)
    (local $ob i32)
    (local $oa i32)
    (local $q16 i32)
    (local $qhdr i32)
    (local $vpx_base v128)
    (local $vw v128)
    (local $vwf v128)
    (local $vhf v128)
    (local $vw_mask v128)
    (local $vh_mask v128)
    (local $vmask_ff v128)
    (local $v3 v128)
    (local $vinv255 v128)
    (local $tr v128)
    (local $tg v128)
    (local $tb v128)
    (local $ta v128)
    (local $addr v128)
    (local $px v128)
    (local $ai v128)

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

    (local.set $quads    (i32.load offset=8  (local.get $batch)))
    (local.set $varyings (i32.load offset=12 (local.get $batch)))
    (local.set $uniforms (i32.load offset=16 (local.get $batch)))
    (local.set $out_rgba (i32.load offset=20 (local.get $batch)))
    (local.set $textures (i32.load offset=24 (local.get $batch)))
    (if (i32.eqz (local.get $quads))    (then (return)))
    (if (i32.eqz (local.get $varyings)) (then (return)))
    (if (i32.eqz (local.get $uniforms)) (then (return)))
    (if (i32.eqz (local.get $out_rgba)) (then (return)))
    (if (i32.eqz (local.get $textures)) (then (return)))

    (local.set $px_base (i32.load offset=0 (local.get $textures)))
    (local.set $w       (i32.load offset=4 (local.get $textures)))
    (local.set $h       (i32.load offset=8 (local.get $textures)))
    (if (i32.eqz (local.get $px_base))            (then (return)))
    (if (i32.le_s (local.get $w) (i32.const 0))   (then (return)))
    (if (i32.le_s (local.get $h) (i32.const 0))   (then (return)))

    (local.set $vpx_base (i32x4.splat (local.get $px_base)))
    (local.set $vw       (i32x4.splat (local.get $w)))
    (local.set $vwf      (f32x4.splat (f32.convert_i32_s (local.get $w))))
    (local.set $vhf      (f32x4.splat (f32.convert_i32_s (local.get $h))))
    (local.set $vw_mask  (i32x4.splat (i32.sub (local.get $w) (i32.const 1))))
    (local.set $vh_mask  (i32x4.splat (i32.sub (local.get $h) (i32.const 1))))

    (local.set $vmask_ff (v128.const i32x4 255 255 255 255))
    (local.set $v3       (v128.const i32x4 3 3 3 3))
    (local.set $vinv255  (v128.const f32x4 0x1.010102p-8 0x1.010102p-8 0x1.010102p-8 0x1.010102p-8))

    (local.set $tr (f32x4.mul (v128.load32_splat offset=0  (local.get $uniforms)) (local.get $vinv255)))
    (local.set $tg (f32x4.mul (v128.load32_splat offset=4  (local.get $uniforms)) (local.get $vinv255)))
    (local.set $tb (f32x4.mul (v128.load32_splat offset=8  (local.get $uniforms)) (local.get $vinv255)))
    (local.set $ta (f32x4.mul (v128.load32_splat offset=12 (local.get $uniforms)) (local.get $vinv255)))

    (local.set $count  (i32.shl (local.get $count) (i32.const 4)))
    (local.set $u_base (local.get $varyings))
    (local.set $v_base (i32.add (local.get $u_base) (local.get $count)))

    (local.set $or (local.get $out_rgba))
    (local.set $og (i32.add (local.get $or) (local.get $count)))
    (local.set $ob (i32.add (local.get $og) (local.get $count)))
    (local.set $oa (i32.add (local.get $ob) (local.get $count)))

    (local.set $q16  (i32.const 0))
    (local.set $qhdr (local.get $quads))

    (loop $quad_loop
      ;; 4 lane texel bayt adresi: px_base + ((ty * w + tx) << 2)
      local.get $u_base
      local.get $q16
      i32.add
      v128.load align=4
      local.get $vwf
      f32x4.mul
      f32x4.floor
      i32x4.trunc_sat_f32x4_s
      local.get $vw_mask
      v128.and

      local.get $v_base
      local.get $q16
      i32.add
      v128.load align=4
      local.get $vhf
      f32x4.mul
      f32x4.floor
      i32x4.trunc_sat_f32x4_s
      local.get $vh_mask
      v128.and
      local.get $vw
      i32x4.mul
      i32x4.add
      i32.const 2
      i32x4.shl
      local.get $vpx_base
      i32x4.add
      local.tee $addr

      ;; Yazılım gather: düz yığın sırasıyla (her adımda yalnızca 1 geçici GPR canlı)
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
      local.tee $px

      ;; Alfa kanalı (bit 24..31) ve tam sayı discard testi: (px >>> 24) < 3 <=> c.a < 0.01
      i32.const 24
      i32x4.shr_u
      local.set $ai

      local.get $qhdr
      local.get $qhdr
      i32.load
      local.get $ai
      local.get $v3
      i32x4.lt_s
      i32x4.bitmask
      i32.const -1
      i32.xor
      i32.and
      i32.store

      ;; R, G, B, A kanallarını aç (cvtdq2ps için convert_i32x4_s), önceden ölçeklenmiş tint*(1/255) ile çarp
      local.get $or
      local.get $q16
      i32.add
      local.get $px
      local.get $vmask_ff
      v128.and
      f32x4.convert_i32x4_s
      local.get $tr
      f32x4.mul
      v128.store align=4

      local.get $og
      local.get $q16
      i32.add
      local.get $px
      i32.const 8
      i32x4.shr_u
      local.get $vmask_ff
      v128.and
      f32x4.convert_i32x4_s
      local.get $tg
      f32x4.mul
      v128.store align=4

      local.get $ob
      local.get $q16
      i32.add
      local.get $px
      i32.const 16
      i32x4.shr_u
      local.get $vmask_ff
      v128.and
      f32x4.convert_i32x4_s
      local.get $tb
      f32x4.mul
      v128.store align=4

      local.get $oa
      local.get $q16
      i32.add
      local.get $ai
      f32x4.convert_i32x4_s
      local.get $ta
      f32x4.mul
      v128.store align=4

      local.get $qhdr
      i32.const 8
      i32.add
      local.set $qhdr

      local.get $q16
      i32.const 16
      i32.add
      local.tee $q16
      local.get $count
      i32.ne
      br_if $quad_loop
    )
  )
)
