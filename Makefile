# MATOKI Makefile
#  make        -> build/engine.wasm + build/engine_relaxed.wasm (+ web/ kopyaları)
#  make host   -> build/host/test_runner (native)
#  make test   -> native testler + wasm-validate (engine.wasm + engine_relaxed.wasm)
#  make serve  -> tools/serve.py
#  make check  -> test + kural denetimi (src/ altında libc include yok)
#  make status -> görev durum tablosu (tasks/*.md DURUM satırları)
#
# Kurallar:
#  - src/**/*.c otomatik toplanır.
#  - *_wasm.c dosyaları sadece wasm'a girer (SIMD intrinsic vb.).
#  - src/app/ ve src/core/mem.c host'a girmez.
#  - tests/*.c + tools/host/test_main.c + tools/host/plat_host.c host test koşucusunu oluşturur.

CC := clang

WASM_CFLAGS := --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib \
               -msimd128 -matomics -mbulk-memory -mnontrapping-fptoint -msign-ext \
               -DPLAT_SHARED_MEMORY=1 \
               -Wall -Wextra -Werror -Isrc -MMD -MP
WASM_RELAXED_CFLAGS := $(WASM_CFLAGS) -mrelaxed-simd
WASM_LDFLAGS := --target=wasm32 -nostdlib \
               -Wl,--no-entry -Wl,--shared-memory -Wl,--import-memory -Wl,--import-table -Wl,--growable-table \
               -Wl,--initial-memory=268435456 -Wl,--max-memory=1073741824 \
               -Wl,--stack-first -z stack-size=1048576 -Wl,--strip-debug

HOST_CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Isrc -Itests -MMD -MP \
               -fsanitize=address,undefined -fno-omit-frame-pointer
HOST_LDFLAGS := -fsanitize=address,undefined

SRC              := $(shell find src -name '*.c' | sort)
WASM_OBJ         := $(SRC:%.c=build/wasm/%.o)
WASM_RELAXED_OBJ := $(SRC:%.c=build/wasm_relaxed/%.o)

# shaders/*.mtk -> build/gen/embed_shaders.c (sadece wasm; bkz. src/core/embed.h)
.DEFAULT_GOAL := all
SHADERS  := $(sort $(wildcard shaders/*.mtk))
WASM_OBJ         += build/wasm/build/gen/embed_shaders.o
WASM_RELAXED_OBJ += build/wasm_relaxed/build/gen/embed_shaders.o

build/gen/embed_shaders.c: $(SHADERS) tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $@ $(SHADERS)

HOST_LIB_SRC := $(filter-out src/core/mem.c src/app/% %_wasm.c,$(SRC))
HOST_SRC     := $(HOST_LIB_SRC) $(sort $(wildcard tests/*.c)) \
                tools/host/test_main.c tools/host/plat_host.c
HOST_OBJ     := $(HOST_SRC:%.c=build/host/%.o)

# Native araçlar: tools/host/tool_X.c -> build/host/X  (src kütüphanesi + plat_host ile linklenir)
TOOL_SRC     := $(sort $(wildcard tools/host/tool_*.c))
TOOL_BINS    := $(TOOL_SRC:tools/host/tool_%.c=build/host/%)
HOST_LIB_OBJ := $(HOST_LIB_SRC:%.c=build/host/%.o) build/host/tools/host/plat_host.o

.PHONY: all host test serve clean check status detcheck gpucheck
.SECONDARY:

all: web/engine.wasm web/engine_relaxed.wasm

build/engine.wasm: $(WASM_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(WASM_LDFLAGS) $(WASM_OBJ) -o $@

web/engine.wasm: build/engine.wasm
	@mkdir -p web
	cp $< $@

build/engine_relaxed.wasm: $(WASM_RELAXED_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(WASM_LDFLAGS) $(WASM_RELAXED_OBJ) -o $@

web/engine_relaxed.wasm: build/engine_relaxed.wasm
	@mkdir -p web
	cp $< $@

# D-112 / F0_G: Sim-kritik birimlerde determinizm için FMA daralmasını engelle
build/%/src/sim/%.o: EXTRA_CFLAGS += -ffp-contract=off
build/%/src/phys/%.o: EXTRA_CFLAGS += -ffp-contract=off
build/%/src/scene/%.o: EXTRA_CFLAGS += -ffp-contract=off

build/wasm/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WASM_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

build/wasm_relaxed/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(WASM_RELAXED_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

host: build/host/test_runner $(TOOL_BINS)

build/host/test_runner: $(HOST_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_LDFLAGS) $(HOST_OBJ) -o $@

build/host/%: build/host/tools/host/tool_%.o $(HOST_LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_LDFLAGS) $^ -o $@

build/host/bench_memory: build/host/tools/host/bench_memory.o $(HOST_LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_LDFLAGS) -lpthread $^ -o $@

bench_mem: build/host/bench_memory
	./build/host/bench_memory

build/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@


test: all host
	wasm-validate --enable-all build/engine.wasm
	wasm-validate --enable-all build/engine_relaxed.wasm
	./build/host/test_runner

# Belirlenimcilik denetimi (F0_G): host, Node wasm ve tarayıcı ortamlarında 10 000 tick hash doğrulaması
detcheck: all host
	node tools/host/det_check.mjs

# Kural denetimi: src/ altında libc başlığı yasak (derleyici intrinsic başlığı wasm_simd128.h serbest),
# state_*.h başlıklarında işaretçi alanı yasak (D-112).
check: test
	@bad=$$(grep -rn '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' src | grep -v 'wasm_simd128\.h' || true); \
	if [ -n "$$bad" ]; then echo "HATA: src/ altında libc include:"; echo "$$bad"; exit 1; fi
	@./tools/host/check_state_headers.sh
	@echo "check: tamam"

GPUCHECK_PORT ?= 8095
ifeq ($(GPU),1)
GPUCHECK_FLAGS ?= --enable-features=Vulkan --use-angle=vulkan
else
GPUCHECK_FLAGS ?= --use-webgpu-adapter=swiftshader
endif

gpucheck: all build/host/test_runner
	@if [ ! -f build/wgsl/solid.wgsl ]; then ./build/host/test_runner matoki_wgsl; fi
	@python3 tools/host/serve_coi.py $(GPUCHECK_PORT) >/dev/null 2>&1 & \
	srv_pid=$$!; \
	trap "kill $$srv_pid 2>/dev/null || true" EXIT INT TERM; \
	sleep 0.4; \
	node tools/host/webgpu_check.mjs http://127.0.0.1:$(GPUCHECK_PORT)/gpu_test.html DEFAULT $(GPUCHECK_FLAGS)

status:
	@for f in $(sort $(wildcard tasks/*.md)); do \
		printf '%-34s %s\n' "$$(basename $$f .md)" "$$(grep -m1 '^DURUM:' $$f | sed 's/^DURUM: *//')"; \
	done

serve: all
	python3 tools/serve.py

clean:
	rm -rf build web/engine.wasm web/engine_relaxed.wasm

-include $(WASM_OBJ:.o=.d) $(WASM_RELAXED_OBJ:.o=.d) $(HOST_OBJ:.o=.d)
