# SPDX-License-Identifier: Apache-2.0
# FractalSQL service build (spec: FRACTALSQL_SPLIT_PROCESS_SPEC.md).
#
# Daemon target only: `make daemon` is the supported entry point.
# build/fractalsqld     Apache-2.0 daemon (separate process). Compiles the
#                       adapter sources in adapter/src against the vendored core
#                       archive in ../include -- every UDF body runs here,
#                       not inside the database server.

CC      ?= gcc

# Both overridable from the environment/command line for a cross build
# (e.g. an x86_64 build on an arm64 macOS runner, which has no x86_64
# `uname -m` to detect): FSQ_PLATFORM picks the vendored core archive,
# FSQ_ARCH_FLAGS (e.g. "-arch x86_64") is added to every compile and
# link below so the adapter objects, the daemon, and the core archive
# all end up the same architecture.
FSQ_PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
FSQ_ARCH_FLAGS ?=
CORE_ARCHIVE := $(firstword \
    $(wildcard ../include/$(FSQ_PLATFORM)/libfractalsql-community-sovereign-c.a) \
    ../include/libfractalsql-community-sovereign-c.a)

# OpenSSL for fractalsql_enterprise.c's Ed25519 check (via
# ADAPTER_OBJS/CORE_ARCHIVE below). Homebrew's openssl@3 is keg-only, so Darwin needs its own path.
# FSQ_OPENSSL_DIR is overridable too: arm-Homebrew's openssl@3 keg has
# no x86_64 objects, so an x86_64 cross build needs a separately built
# tree (see release.yml's build-macos job) passed in explicitly.
ifeq ($(shell uname -s),Darwin)
  FSQ_OPENSSL_DIR ?= $(shell brew --prefix openssl@3 2>/dev/null)
  ifeq ($(strip $(FSQ_OPENSSL_DIR)),)
    $(error darwin build needs Homebrew's openssl@3: brew install openssl@3)
  endif
  FSQ_OPENSSL_CFLAGS  := -I$(FSQ_OPENSSL_DIR)/include
  FSQ_OPENSSL_LDFLAGS := $(FSQ_OPENSSL_DIR)/lib/libcrypto.a
else
  FSQ_OPENSSL_CFLAGS  :=
  FSQ_OPENSSL_LDFLAGS := -lcrypto
endif

BUILD   := build
GEN     := $(BUILD)/gen
ADAPTER_SRCS := $(wildcard adapter/src/*.c)
ADAPTER_OBJS := $(patsubst adapter/src/%.c,$(BUILD)/daemon/%.o,$(ADAPTER_SRCS))

CFLAGS  ?= -O2 -g

# make COVERAGE=1 / ASAN=1 / UBSAN=1: instrument the daemon (adapter
# objects + fractalsqld.c), for build_test.sh
# --coverage/--asan/--ubsan/--tsan.
ifdef COVERAGE
  FSQ_SAN_FLAGS := --coverage
else ifdef ASAN
  FSQ_SAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer
else ifdef UBSAN
  FSQ_SAN_FLAGS := -fsanitize=undefined -fno-sanitize-recover=undefined
else ifdef TSAN
  FSQ_SAN_FLAGS := -fsanitize=thread
else
  FSQ_SAN_FLAGS :=
endif

DAEMON_SRC := daemon/fractalsqld.c common/fsq_sha256.h protocol/fsq_protocol.h protocol/opcodes.def

.PHONY: all daemon clean

all: daemon

daemon: $(BUILD)/fractalsqld

$(GEN)/daemon_udfs.h: protocol/opcodes.def protocol/functions.def scripts/gen_udf.py
	python3 scripts/gen_udf.py $(GEN)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/daemon:
	mkdir -p $@

$(BUILD)/daemon/%.o: adapter/src/%.c | $(BUILD)/daemon
	$(CC) -w -O2 -g -fPIC $(FSQ_ARCH_FLAGS) -I../include -Iadapter/src $(FSQ_OPENSSL_CFLAGS) $(FSQ_SAN_FLAGS) -c -o $@ $<

# The daemon TU is compiled to an object next to the adapter objects
# instead of on the link line, so its -coverage .gcno lands in
# $(BUILD)/daemon/ and lcov pairs it with the .gcda build_test.sh copies in.
$(BUILD)/daemon/fractalsqld.o: daemon/fractalsqld.c $(DAEMON_SRC) $(GEN)/daemon_udfs.h | $(BUILD)/daemon
	$(CC) -Wall -Wextra -Werror -std=gnu11 $(CFLAGS) $(FSQ_ARCH_FLAGS) -I../include -Iadapter/src -I$(GEN) $(FSQ_SAN_FLAGS) -c -o $@ $<

$(BUILD)/fractalsqld: $(BUILD)/daemon/fractalsqld.o $(ADAPTER_OBJS) $(CORE_ARCHIVE) | $(BUILD)
	$(CC) -Wall -Wextra -Werror -std=gnu11 $(CFLAGS) $(FSQ_ARCH_FLAGS) -I../include -Iadapter/src -I$(GEN) $(FSQ_SAN_FLAGS) \
	    -o $@ $(BUILD)/daemon/fractalsqld.o $(ADAPTER_OBJS) $(CORE_ARCHIVE) -lm -lpthread -ldl $(FSQ_OPENSSL_LDFLAGS)

clean:
	rm -rf $(BUILD)
