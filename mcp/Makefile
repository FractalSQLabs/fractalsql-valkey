# SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# fractalsql-valkey-mcp: the MCP server. A plain C++17 client of a Valkey that has the module loaded;
# it links no Valkey client library and none of the module (it speaks RESP itself).
#
#   make            build ./fractalsql-valkey-mcp   (OUT=path and BUILD=dir move the outputs)
#   make probes     build the standalone unit probes under build/
#   make clean
CXX      ?= c++
CXXFLAGS ?= -O2
STD      := -std=c++17 -Wall -Wextra
# CPPEXTRA / LDEXTRA: extra include and library flags (macOS: Homebrew's keg-only OpenSSL, -I<prefix>/include and -L<prefix>/lib).
CPPEXTRA ?=
LDEXTRA  ?=
INC      := -Isrc -Ivendor $(CPPEXTRA)
LDLIBS   := $(LDEXTRA) -lssl -lcrypto -lpthread
BUILD    ?= build
OUT      ?= fractalsql-valkey-mcp

SRCS := $(wildcard src/*.cpp) $(wildcard src/adapters/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))

.DEFAULT_GOAL := $(OUT)

$(OUT): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS) $(SANFLAGS)

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(CXXFLAGS) $(SANFLAGS) $(INC) -MMD -c -o $@ $<

-include $(OBJS:.o=.d)

all: $(OUT)

probes: $(BUILD)/vectors_probe $(BUILD)/concurrency_probe $(BUILD)/db_probe $(BUILD)/sanitize_probe

$(BUILD)/vectors_probe: tests/vectors_probe.cpp src/vectors.cpp src/errors.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(STD) $(CXXFLAGS) $(INC) -o $@ $^ $(LDLIBS)
$(BUILD)/concurrency_probe: tests/concurrency_probe.cpp src/errors.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(STD) $(CXXFLAGS) $(INC) -o $@ $^ $(LDLIBS)
$(BUILD)/db_probe: tests/db_probe.cpp src/db.cpp src/errors.cpp src/logging.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(STD) $(CXXFLAGS) $(INC) -o $@ $^ $(LDLIBS)
$(BUILD)/sanitize_probe: tests/sanitize_probe.cpp src/errors.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(STD) $(CXXFLAGS) $(INC) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: probes clean
