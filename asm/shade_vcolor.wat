;; asm/shade_vcolor.wat — vcolor.mtk (köşe rengi * uniform tint) için elle yazılmış SIMD gölgelendirici.
;; ShadeBatch ABI (src/raster/shade_abi.h) sözleşmesini birebir uygular.
;; Optimizasyonlar:
;; 1) Uniform tint bileşenleri v128.load32_splat ile tek komutla (f32.load + f32x4.splat yerine) yüklenir.
;; 2) Bozuk/aşırı girdi denetimi (NULL işaretçi, count <= 0 veya taşma) döngü öncesinde yapılır.
;; 3) 4x döngü açma (offset=0/16/32/48): her kanal için taban+ofset adresi 4 quad'da bir kez
;;    hesaplanarak iç döngüdeki adres toplama (i32.add) ve dallanma maliyeti 4 kat azaltılır.
(module
  (import "env" "memory" (memory 1))
  (func $shade (export "shade") (param $batch i32)
    (local $count i32)
    (local $stride i32)
    (local $varyings i32)
    (local $uniforms i32)
    (local $out_rgba i32)
    (local $vr i32)
    (local $vg i32)
    (local $vb i32)
    (local $va i32)
    (local $or i32)
    (local $og i32)
    (local $ob i32)
    (local $oa i32)
    (local $q16 i32)
    (local $end64 i32)
    (local $src i32)
    (local $dst i32)
    (local $tr v128)
    (local $tg v128)
    (local $tb v128)
    (local $ta v128)

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

    (local.set $tr (v128.load32_splat offset=0  (local.get $uniforms)))
    (local.set $tg (v128.load32_splat offset=4  (local.get $uniforms)))
    (local.set $tb (v128.load32_splat offset=8  (local.get $uniforms)))
    (local.set $ta (v128.load32_splat offset=12 (local.get $uniforms)))

    (local.set $stride (i32.shl (local.get $count) (i32.const 4)))
    (local.set $vr (local.get $varyings))
    (local.set $vg (i32.add (local.get $vr) (local.get $stride)))
    (local.set $vb (i32.add (local.get $vg) (local.get $stride)))
    (local.set $va (i32.add (local.get $vb) (local.get $stride)))

    (local.set $or (local.get $out_rgba))
    (local.set $og (i32.add (local.get $or) (local.get $stride)))
    (local.set $ob (i32.add (local.get $og) (local.get $stride)))
    (local.set $oa (i32.add (local.get $ob) (local.get $stride)))

    (local.set $q16 (i32.const 0))
    (local.set $end64 (i32.and (local.get $stride) (i32.const -64)))

    (if (i32.gt_u (local.get $end64) (i32.const 0))
      (then
        (loop $loop4
          (local.set $src (i32.add (local.get $vr) (local.get $q16)))
          (local.set $dst (i32.add (local.get $or) (local.get $q16)))
          (v128.store offset=0  (local.get $dst) (f32x4.mul (v128.load offset=0  (local.get $src)) (local.get $tr)))
          (v128.store offset=16 (local.get $dst) (f32x4.mul (v128.load offset=16 (local.get $src)) (local.get $tr)))
          (v128.store offset=32 (local.get $dst) (f32x4.mul (v128.load offset=32 (local.get $src)) (local.get $tr)))
          (v128.store offset=48 (local.get $dst) (f32x4.mul (v128.load offset=48 (local.get $src)) (local.get $tr)))

          (local.set $src (i32.add (local.get $vg) (local.get $q16)))
          (local.set $dst (i32.add (local.get $og) (local.get $q16)))
          (v128.store offset=0  (local.get $dst) (f32x4.mul (v128.load offset=0  (local.get $src)) (local.get $tg)))
          (v128.store offset=16 (local.get $dst) (f32x4.mul (v128.load offset=16 (local.get $src)) (local.get $tg)))
          (v128.store offset=32 (local.get $dst) (f32x4.mul (v128.load offset=32 (local.get $src)) (local.get $tg)))
          (v128.store offset=48 (local.get $dst) (f32x4.mul (v128.load offset=48 (local.get $src)) (local.get $tg)))

          (local.set $src (i32.add (local.get $vb) (local.get $q16)))
          (local.set $dst (i32.add (local.get $ob) (local.get $q16)))
          (v128.store offset=0  (local.get $dst) (f32x4.mul (v128.load offset=0  (local.get $src)) (local.get $tb)))
          (v128.store offset=16 (local.get $dst) (f32x4.mul (v128.load offset=16 (local.get $src)) (local.get $tb)))
          (v128.store offset=32 (local.get $dst) (f32x4.mul (v128.load offset=32 (local.get $src)) (local.get $tb)))
          (v128.store offset=48 (local.get $dst) (f32x4.mul (v128.load offset=48 (local.get $src)) (local.get $tb)))

          (local.set $src (i32.add (local.get $va) (local.get $q16)))
          (local.set $dst (i32.add (local.get $oa) (local.get $q16)))
          (v128.store offset=0  (local.get $dst) (f32x4.mul (v128.load offset=0  (local.get $src)) (local.get $ta)))
          (v128.store offset=16 (local.get $dst) (f32x4.mul (v128.load offset=16 (local.get $src)) (local.get $ta)))
          (v128.store offset=32 (local.get $dst) (f32x4.mul (v128.load offset=32 (local.get $src)) (local.get $ta)))
          (v128.store offset=48 (local.get $dst) (f32x4.mul (v128.load offset=48 (local.get $src)) (local.get $ta)))

          (local.set $q16 (i32.add (local.get $q16) (i32.const 64)))
          (br_if $loop4 (i32.lt_u (local.get $q16) (local.get $end64)))
        )
      )
    )

    (if (i32.lt_u (local.get $q16) (local.get $stride))
      (then
        (loop $loop1
          (v128.store (i32.add (local.get $or) (local.get $q16))
            (f32x4.mul (v128.load (i32.add (local.get $vr) (local.get $q16))) (local.get $tr)))
          (v128.store (i32.add (local.get $og) (local.get $q16))
            (f32x4.mul (v128.load (i32.add (local.get $vg) (local.get $q16))) (local.get $tg)))
          (v128.store (i32.add (local.get $ob) (local.get $q16))
            (f32x4.mul (v128.load (i32.add (local.get $vb) (local.get $q16))) (local.get $tb)))
          (v128.store (i32.add (local.get $oa) (local.get $q16))
            (f32x4.mul (v128.load (i32.add (local.get $va) (local.get $q16))) (local.get $ta)))
          (local.set $q16 (i32.add (local.get $q16) (i32.const 16)))
          (br_if $loop1 (i32.lt_u (local.get $q16) (local.get $stride)))
        )
      )
    )
  )
)
