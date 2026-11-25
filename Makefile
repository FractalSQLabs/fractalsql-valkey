# fractalsql-valkey Makefile — local (non-Docker) development build.
#
# v1: pure-C path. The module statically links the vendored core
# archive at include/libfractalsql-community-minimal-c.a. No runtime
# LuaJIT dep, no pkg-config probe.
#
# Refresh include/ from the foundry:
#     cd ../fractalsql-core
#     make validated-drop-native           # modern (glibc 2.34)
#     ./scripts/deploy.sh --git fractalsql-valkey
#
# Build the module:
#     make                                  # picks the modern -c.a by default
#     make CORE_VARIANT=community-sovereign-c-musl   # musl build
#
# Install + load:
#     sudo make install
#     valkey-cli MODULE LOAD /usr/lib/valkey/modules/fractalsql.so

CC ?= gcc

# Vendored core archive selector. Default is the modern glibc-2.34
# build dropped by `make validated-drop-native` in core. Override to
# `community-sovereign-c-musl` for Alpine / musl hosts. glibc 2.34 is the
# floor; there is no glibc 2.28 variant.
CORE_VARIANT ?= community-sovereign-c
# v1.2 platform layout: link the per-platform subdir
# include/<os>-<arch>/ (os = uname -s lower-cased, arch = uname -m, which
# is x86_64/aarch64 on Linux and arm64 on Apple Silicon — no translation).
# Falls back to the flat include/ layout during the dual-write window so
# this builds whether or not the subdir has been deployed yet; when core
# drops the flat layout (v1.3) the fallback resolves to the subdir.
FSQL_PLATFORM := $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
CORE_ARCHIVE  := $(firstword \
    $(wildcard include/$(FSQL_PLATFORM)/libfractalsql-$(CORE_VARIANT).a) \
    include/libfractalsql-$(CORE_VARIANT).a)

CFLAGS  = -Wall -Wextra -O3 -flto -fPIC -Iinclude
# One version, in ./VERSION. `override` so it survives a command-line CFLAGS
# (build_test.sh passes its own for sanitizer builds).
FSQL_VERSION := $(strip $(shell cat VERSION))
override CFLAGS += -DFRACTALSQL_VERSION=\"$(FSQL_VERSION)\"
# Keep the static core out of the server's symbol table. GNU ld spells that
# --exclude-libs; Apple's ld takes an explicit export list instead.
# (The Darwin branch has not been run yet -- there is no macOS host here.)
ifeq ($(shell uname -s),Darwin)
LDFLAGS = -dynamiclib -undefined dynamic_lookup -Wl,-exported_symbol,_RedisModule_OnLoad -lm -lpthread
else
LDFLAGS = -shared -flto -Wl,--exclude-libs,ALL -lm -ldl -lpthread
endif

TARGET = fractalsql.so
SRCS   = src/module.c src/fv_pool.c src/fv_search.c src/fv_vector.c src/fv_analytics.c src/fv_daemon.c src/fv_agents.c src/fv_session.c src/fv_oom.c src/fv_index.c src/fv_feature.c
OBJS   = $(SRCS:.c=.o)

# The files that carry the float paths (the ones that include src/fsql_precision.h) are compiled with contraction and
# vectorisation OFF, so every distance, norm and cosine is a fixed sequence of IEEE double operations (the precision
# contract R2 and R3).
#
# These are TARGET-SPECIFIC variables, and that is not a style choice. The obvious form,
# $(FP_CFLAGS_$(*)) in the recipe, expands to NOTHING: make expands a variable NAME once, and the
# automatic variable inside that name is not expanded with it, so every float path was silently
# compiled with the compiler's default float behaviour. gcc contracts a*b+c into an FMA by default,
# and arm64 always has one, so the squared L2 distance stopped matching the sequential double
# reference there (and the gate that checks it failed on linux/arm64 and on the macOS sanitizer
# build). A target-specific variable does reach the recipe of a pattern rule, and it adds to
# whatever CFLAGS the caller passed, so a sanitizer build that passes its own CFLAGS on the
# command line keeps these.
FP_FLAGS   = -ffp-contract=off -fno-tree-vectorize
src/fv_vector.o src/fv_search.o src/fv_index.o src/fv_agents.o: FP_CFLAGS += $(FP_FLAGS)

# Default Valkey module dir on Debian / Ubuntu / most distros.
MODULE_DIR ?= /usr/lib/valkey/modules

all: verify-vendor $(TARGET)

# redismodule.h is vendored (include/redismodule.h, from the Redis 7.2 module
# API, which Valkey 7.2/8.x keep source-compatible). It is never fetched at
# build time: a moving URL with no checksum is a supply-chain hole.
include/redismodule.h:
	@echo "ERROR: include/redismodule.h is missing; restore it from git" >&2
	@exit 1

# Verify the vendored core archive is present BEFORE compiling. Cheaper
# to fail here than to surface as a confusing linker error.
$(CORE_ARCHIVE):
	@echo "ERROR: $(CORE_ARCHIVE) missing." >&2
	@echo "  Refresh from foundry:" >&2
	@echo "    cd ../fractalsql-core && make validated-drop-native && ./scripts/deploy.sh --git fractalsql-valkey" >&2
	@exit 1

# Static-link the core archive into the module. -Wl,--whole-archive is
# NOT needed — the linker pulls in only the symbols module.c references
# (fsql_init, fsql_new, fsql_search_ptr, fsql_last_error), keeping the
# resulting .so close to the size of the .a's used .text.

# Supply-chain verification (B5). The vendored archive is dropped
# from fractalsql-core's deploy.sh together with `.artifacts.sha256`
# (sha256sum of every shipped .h/.a/.so). Verify it matches the
# bytes on disk before linking — catches a tampered .a in this
# repo's include/ at build time, both in `make` and in CI.
.PHONY: verify-vendor
verify-vendor:
	@if [ ! -f include/.artifacts.sha256 ]; then \
		echo "ERROR: include/.artifacts.sha256 missing — re-deploy from core" >&2; \
		echo "  cd ../fractalsql-core && make validated-drop-native && ./scripts/deploy.sh --git fractalsql-valkey" >&2; \
		exit 1; \
	fi
	@cd include && sha256sum --quiet --check .artifacts.sha256 || { \
		echo "ERROR: vendored artifact checksum mismatch in include/." >&2; \
		echo "  Possible causes: tampered .a/.so, partial deploy, or stale .sha256." >&2; \
		echo "  Re-deploy from core to recover." >&2; \
		exit 1; \
	}

$(TARGET): $(OBJS) $(CORE_ARCHIVE) verify-vendor
	$(CC) -o $@ $(OBJS) $(CORE_ARCHIVE) $(LDFLAGS)

# -MMD writes src/*.d so a changed project header (fv_common.h) rebuilds every object that includes it; without
# that, objects built against an older struct layout link into a module that crashes at run time. VERSION is a
# prerequisite too: it is baked into the objects with -DFRACTALSQL_VERSION, so a bump must rebuild them.
%.o: %.c include/fractalsql.h include/redismodule.h VERSION
	$(CC) $(CFLAGS) $(FP_CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
	@# include/redismodule.h is vendored and tracked: clean and distclean leave it alone.

distclean: clean

install: $(TARGET)
	install -Dm0755 $(TARGET) $(MODULE_DIR)/fractalsql.so

.PHONY: all clean distclean install verify-vendor
