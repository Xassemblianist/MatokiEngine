# S1 — Ofset tabanlı durum bölgesi prototipi (D-112, ARCH_V2 §2/§6)

Durum: TODO

## Amaç
`src/state/` altında: taban adres saklamayan `RegionArena` (ofset döner), `StateRef<T>` benzeri tipli ofset/tutamaç
makroları, sürüm/nesil kontrollü tutamaçlar; durum başlıklarında (`state_*.h`) işaretçi alanı yasağı için `make check`
denetimi (betik + `_Static_assert`). Küçük kanıt: ECS benzeri SoA depo + fizik gövde dizisi dilimi işaretçisiz; anlık
görüntü = bölgenin kopyası; bölge **farklı taban adrese** kopyalanıp orada da çalışır ve hash aynı kalır (asıl kanıt).
Ölçüm: 16 MB kopya + hash süresi (F0_D ile karşılaştır).

## Dosyalar
- `src/state/*` (yeni), `tests/test_state.c` (yeni), `tools/host/check_state_headers.sh` (yeni), `Makefile` (yalnız denetim + kaynaklar)

## Kabul kriterleri
- [ ] `make check` hatasız; işaretçi denetimi kasıtlı hatalı örnekte başarısız olur (test)
- [ ] Farklı taban adreste aynı hash; ölçümler Notlar'da
