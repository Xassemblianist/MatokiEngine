DURUM: TODO

# M1_1 — Matematik kütüphanesi

## Dosyalar
- `src/core/math.h/.c`
  - `f32` için kendi `sinf/cosf/tanf/sqrtf/floorf/fabsf/powf/expf` (sqrt/floor/abs → `__builtin_*` wasm opcode'larına iner, diğerleri minimax polinom)
  - `Vec2/Vec3/Vec4/Mat4/Quat`, `IVec3`
  - `mat4_perspective` (**WebGPU derinlik aralığı 0..1**, reverse-Z tercih edilir), `mat4_look_at`, `mat4_mul`, `mat4_inverse`
  - Frustum: 6 düzlem çıkarma + AABB testi
- `src/core/rng.h/.c` — PCG32, `rng_f32`, `rng_range`
- `src/core/hash.h` — `hash_u32`, `hash_ivec3` (dünya üretimi için deterministik)

## Kabul kriterleri
- `src/tests/test_math.c`: `sinf` hatası [-π, π] aralığında < 1e-5; mat4 inverse * mat = I (tol 1e-4). Testler `game_run_tests()` export'u ile tarayıcı konsolunda çalışır.
