DURUM: DONE
HAT: A (platform)

# M0_1 — Arena, sunucu ve motor iskeleti

## Hazır olanlar (DOKUNMA — değişiklik gerekirse görev dosyasına not düş)
`Makefile`, `src/core/types.h`, `src/core/mem.c`, `src/core/log.h/.c`, `src/plat/platform.h`,
`tests/test.h`, `tools/host/test_main.c`, `tools/host/plat_host.c`.
Export'lar `EXPORT(isim)` makrosuyla yapılır (`--export-dynamic` kullanılmıyor).

## Dosyalar
- `src/core/arena.h/.c` — `Arena { u8* base; usize cap, used; }`, `arena_init(a, base, cap)`, `arena_push(a, size, align)` (sıfırlanmış bellek döner), `ARENA_PUSH_STRUCT(a,T)`, `ARENA_PUSH_ARRAY(a,T,n)`, `arena_reset`, `arena_mark/arena_pop_to`, `arena_sub(parent, cap)` (alt arena). Taşmada `PANIC`.
- `tests/test_arena.c` — hizalama (1..256), mark/pop, taşma sınırı (taşma testini PANIC çağırmadan, kalan kapasite kontrolüyle yap).
- `src/app/main.c` — `extern u8 __heap_base;` ile heap başlangıcı; `perm` (64 MB) ve `frame` (16 MB) arenaları; `engine_frame` başında `frame` sıfırlanır.
- `tools/serve.py` — `web/` kökünden statik sunucu, port 8080, `application/wasm` MIME, `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`, `Cache-Control: no-store`.

## Kabul kriterleri
- `make` ve `make test` uyarısız geçer.
- `wasm-objdump -x build/engine.wasm`: import'lar yalnızca `env.memory`, `env.__indirect_function_table`, `env.plat_*`.

## Notlar
- `src/core/arena.h/.c`: Doğrusal bellek ayırıcı (`arena_init`, `arena_push`, `arena_reset`, `arena_mark`, `arena_pop_to`, `arena_sub` ve `ARENA_PUSH_STRUCT`/`ARENA_PUSH_ARRAY` makroları) gerçeklendi. Ayrılan bölgeler `__builtin_memset` ile sıfırlanır, kapasite aşımında `PANIC` çağrılır.
- `tests/test_arena.c`: 1..256 arası ikinin kuvveti hizalamaları, sıfırlama garantisi, `mark`/`pop_to`, `arena_sub` ve kalan kapasite sınır testleri eklendi; tüm testler geçti.
- `src/app/main.c`: `__heap_base`'den itibaren 64 MB `perm` ve 16 MB `frame` arenaları kuruldu, `engine_frame` başında `frame` sıfırlanıyor.
- `tools/serve.py`: Port 8080'de `web/` dizinini `application/wasm` MIME, COOP/COEP ve `Cache-Control: no-store` başlıklarıyla sunan geliştirme sunucusu yazıldı.
- `wasm-objdump -x build/engine.wasm` ile import'ların yalnızca `env.memory`, `env.__indirect_function_table` ve `env.plat_log` olduğu doğrulandı.
