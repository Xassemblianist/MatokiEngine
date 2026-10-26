;; asm/fill_u32.wat — Framebuffer 32-bit RGBA temizleme çekirdeği.
;; Clang'ı yenme stratejisi:
;; 1) Tekrarlayan bayt deseni tespiti ((val & 0xFF) * 0x01010101 == val): Siyah (0x00000000),
;;    beyaz (0xFFFFFFFF) veya gri tonlu temizlemelerde döngüyü tamamen atlayıp tek bir
;;    WebAssembly bulk-memory `memory.fill` komutuyla (motorun native memset'i) belleği doldurur.
;; 2) Genel 32-bit RGBA değerleri için: clang -O2 tekli `v128.store` (16B/adım) ve çift sayaç
;;    üretirken, burada 4x açılmış (64 bayt = 16 piksel/adım, offset=0/16/32/48) `v128.store`
;;    döngüsü ve tek bitiş işaretçisi ($end64) karşılaştırması kullanılır.
(module
  (import "env" "memory" (memory 1 16384 shared))
  (func $fill_u32 (export "fill_u32") (param $dst i32) (param $val i32) (param $count i32)
    (local $b0 i32)
    (local $end64 i32)
    (local $end16 i32)
    (local $end4 i32)
    (local $v v128)

    (if (i32.eqz (local.get $count))
      (then (return))
    )

    ;; (val & 0xFF) * 0x01010101 == val ise doğrudan memory.fill kullan
    (local.set $b0 (i32.and (local.get $val) (i32.const 255)))
    (if (i32.eq (local.get $val) (i32.mul (local.get $b0) (i32.const 0x01010101)))
      (then
        (memory.fill
          (local.get $dst)
          (local.get $b0)
          (i32.shl (local.get $count) (i32.const 2)))
        (return)
      )
    )

    (local.set $v (i32x4.splat (local.get $val)))
    (local.set $end4 (i32.add (local.get $dst) (i32.shl (local.get $count) (i32.const 2))))
    (local.set $end64 (i32.add (local.get $dst) (i32.shl (i32.and (local.get $count) (i32.const -16)) (i32.const 2))))

    (block $break64
      (loop $loop64
        (br_if $break64 (i32.ge_u (local.get $dst) (local.get $end64)))
        (v128.store offset=0  (local.get $dst) (local.get $v))
        (v128.store offset=16 (local.get $dst) (local.get $v))
        (v128.store offset=32 (local.get $dst) (local.get $v))
        (v128.store offset=48 (local.get $dst) (local.get $v))
        (local.set $dst (i32.add (local.get $dst) (i32.const 64)))
        (br $loop64)
      )
    )

    (local.set $end16 (i32.sub (local.get $end4) (i32.shl (i32.and (local.get $count) (i32.const 3)) (i32.const 2))))
    (block $break16
      (loop $loop16
        (br_if $break16 (i32.ge_u (local.get $dst) (local.get $end16)))
        (v128.store (local.get $dst) (local.get $v))
        (local.set $dst (i32.add (local.get $dst) (i32.const 16)))
        (br $loop16)
      )
    )

    (block $break4
      (loop $loop4
        (br_if $break4 (i32.ge_u (local.get $dst) (local.get $end4)))
        (i32.store (local.get $dst) (local.get $val))
        (local.set $dst (i32.add (local.get $dst) (i32.const 4)))
        (br $loop4)
      )
    )
  )
)
