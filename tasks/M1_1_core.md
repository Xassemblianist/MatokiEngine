DURUM: DONE
HAT: B (çekirdek)

# M1_1 — Matematik, sabit nokta, RNG, profiler

## Hazır olanlar (DOKUNMA)
`Makefile`, `src/core/types.h`, `src/core/log.h/.c`, `tests/test.h`, `tools/host/*`.
Yeni `tests/*.c` dosyaları otomatik olarak `make test`'e girer.

## Dosyalar
- `src/core/math.h/.c`
  - `f32` için kendi `m_sin/m_cos/m_tan/m_atan2/m_exp2/m_log2/m_pow` (minimax polinom + aralık indirgeme); `m_sqrt/m_floor/m_ceil/m_abs/m_min/m_max/m_trunc` → `__builtin_sqrtf` vb. (wasm opcode'una iner; host'ta da derleyici halleder, libc'ye bağımlı olmamalı — gerekirse `static inline`).
  - `Vec2/Vec3/Vec4/Mat3/Mat4`, `IVec2`; `mat4_perspective` (derinlik 0..1), `mat4_look_at`, `mat4_mul`, `mat4_inverse`, `mat3_2d` (öteleme+dönme+ölçek)
- `src/core/fixed.h` — 24.8 sabit nokta (`Fx8`) ve 16.16 (`Fx16`) yardımcıları, i64 ara çarpım
- `src/core/rng.h/.c` — PCG32, `rng_seed`, `rng_u32`, `rng_f32` [0,1), `rng_range`
- `src/core/profiler.h/.c` — `PROF_BEGIN("isim")` / `PROF_END()` (iç içe, en fazla 64 bölge), zaman kaynağı `plat_time_ms`; frame başına ms toplamları; `prof_frame_end()`, `prof_report()` LOG'a hizalı tablo basar
- `tests/test_math.c`, `tests/test_rng.c`, `tests/test_fixed.c`

## Kabul kriterleri
- `make` ve `make test` uyarısız geçer.
- `m_sin/m_cos` max hatası [-100, 100]'de < 2e-6 (referans: test dosyasında kendi `double` hassas implementasyonun — libm kullanma).
- `m_exp2/m_log2` göreli hata < 1e-6; `mat4_inverse(m) * m ≈ I` (tol 1e-4, 100 rastgele matris).
- PCG32 ilk 6 çıktısı resmi pcg32 örneğiyle eşleşir (seed 42, seq 54).
- Notlara: her fonksiyonun ölçülen max hatası.

## Notlar
- `src/core/math.h/.c`:
  - `m_sqrt`, `m_abs`, `m_floor`, `m_ceil`, `m_trunc`, `m_min`, `m_max`, `m_clamp`, `m_lerp` fonksiyonları `static inline` olarak tanımlandı. Wasm32 hedefinde doğrudan wasm float opcode'larına (`f32.sqrt`, `f32.floor`, `f32.ceil`, `f32.trunc`, `f32.abs`) iner; native host derlemesinde `-lm` gerektirmeden çalışır.
  - `m_sin`, `m_cos`, `m_tan`: 3 sabitli Cody-Waite ($\pi/2$) aralık indirgemesi ile $[-\pi/4, \pi/4]$ aralığına indirgendi ve Remez/Chebyshev minimax polinomlarıyla hesaplandı.
  - `m_atan2`: Oktant indirgemesi + $\tan(\pi/8) = \sqrt{2}-1$ eşiğiyle $[0, 0.41421356]$ aralığına indirgeme ve 9. derece tek-kuvvet minimax polinomu kullanıldı.
  - `m_exp2`, `m_log2`, `m_pow`: IEEE-754 üs/mantissa ayrıştırması ($m \in [\sqrt{2}/2, \sqrt{2}]$, $t = (m-1)/(m+1)$ dönüşümü) ve minimax polinomları ile gerçeklendi.
  - Sütun-ana (`column-major`) `Mat3` ve `Mat4` işlemleri (`mat3_2d`, `mat4_perspective` [0..1 derinlik], `mat4_look_at`, `mat4_mul`, kofaktör açılımlı `mat4_inverse`) eklendi.
- `src/core/fixed.h`: `Fx8` (24.8) ve `Fx16` (16.16) dönüşüm, yuvarlama ve `i64` ara çarpım/bölüm yardımcıları UBSan-temiz şekilde yazıldı.
- `src/core/rng.h/.c`: PCG32 (`pcg_onseq_64_xsh_rr_32`) gerçeklendi; `seed=42, seq=54` için ilk 6 çıktı resmi referans dizisiyle (`0xa15c02b7`, `0x7b47f409`, `0xba1d3330`, `0x83d2f293`, `0xbfa4784b`, `0xcbed606e`) birebir doğrulandı.
- `src/core/profiler.h/.c`: `PROF_BEGIN`/`PROF_END` ile 64 bölgeye kadar iç içe zaman ölçümü, `prof_frame_end()` ve `prof_report()` ile hizalı tablo çıktısı eklendi.

### Ölçülen Maksimum Hata Tablosu (`f64` Taylor referansına karşı)

| Fonksiyon | Test Aralığı / Koşul | Hata Türü | Ölçülen Max Hata | Kabul Kriteri |
| :--- | :--- | :--- | :--- | :--- |
| `m_sin` | $[-100, 100]$ (200.000 örnek) | Mutlak hata | `8.763e-08` | `< 2.0e-06` |
| `m_cos` | $[-100, 100]$ (200.000 örnek) | Mutlak hata | `8.955e-08` | `< 2.0e-06` |
| `m_tan` | $[-\pi/4, \pi/4]$ (40.000 örnek) | Mutlak hata | `7.550e-08` | — |
| `m_tan` | $[-1.3, 1.3]$ (40.000 örnek) | Mutlak hata | `4.357e-07` | — |
| `m_atan2` | $x, y \in [-41, 41]^2 \setminus \{(0,0)\}$ | Mutlak hata | `2.564e-07` | — |
| `m_exp2` | $[-100, 100]$ (200.000 örnek) | Göreli hata | `8.623e-08` | `< 1.0e-06` |
| `m_log2` | $[2^{-80}, 2^{80}]$ (200.000 örnek) | Göreli hata | `1.814e-07` | `< 1.0e-06` |
| `m_pow` | $x \in [0.1, 10.1], y \in [-5, 5]$ | Göreli hata | `1.190e-06` | — |
| `mat4_inverse` | 100 rastgele matris (`inv(M)*M - I`) | Maks bileşen hatası | `3.576e-07` | `< 1.0e-04` |

