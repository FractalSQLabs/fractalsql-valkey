# syntax=docker/dockerfile:1.7
#
# fractalsql-valkey Factory build — pure-C, vendored core archive.
#
# v1 layout: the vendored libfractalsql-community-minimal-c.a is dropped
# into include/ by core's `./scripts/deploy.sh --git fractalsql-valkey`.
# This Dockerfile compiles src/module.c against that archive plus a
# pinned redismodule.h. No LuaJIT stage, no source-from-git build.
#
# Architecture note (v1 simplification):
#   The vendored .a is single-arch (matches the host that ran core's
#   `make validated-drop-native`). v1 supports amd64 only. Multi-arch
#   support comes in v1.x by either:
#     (a) extending core's deploy.sh to stage per-arch .a into
#         include/amd64/ + include/arm64/, OR
#     (b) c-api shipping source so each arch can compile from .c.
#   Both options are tracked separately; the v1 module ships amd64.
#
# Build:
#   DOCKER_BUILDKIT=1 docker buildx build \
#     --platform linux/amd64 \
#     --target export \
#     --output type=local,dest=./dist/amd64 \
#     -f docker/Dockerfile .
#
# build.sh drives this for the supported arch.

ARG BASE_IMAGE=rockylinux:9@sha256:d7be1c094cc5845ee815d4632fe377514ee6ebcf8efaed6892889657e5ddaaa6
# The Valkey release the fractalsqld and demo stages run on (7.2, 8.0 and 8.1 are all supported).
ARG VALKEY_VERSION=8.1
# Pinned by digest (the tag alone is mutable). If you override VALKEY_VERSION, override VALKEY_IMAGE_DIGEST too.
ARG VALKEY_IMAGE_DIGEST=sha256:8f836008e0986c36adf50da714c7060cf689dbb335f61fe077939d90fa650809
ARG OUTPUT_SUFFIX=
ARG GLIBC_CEILING=34
ARG LIBC_KIND=glibc
ARG SIZE_CEILING_BYTES=3145728

# Vendored core archive variant. Default "community-minimal-c" pulls
# the modern glibc-2.34 build. Override to "community-minimal-c-musl"
# for Alpine / distroless-musl hosts. glibc 2.34 is the floor.
ARG CORE_VARIANT=community-sovereign-c

# Valkey 8.0 ships a redismodule.h that exposes the full
# REDISMODULE_APIVER_1 surface used by 6.2 / 7.0 / 7.2 / 7.4 / 8.x.
# Hardened compile/link flags. Same posture as the pre-1.0 LuaJIT
# build, minus the LuaJIT-specific bits.
ARG EXT_CFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0 -ffunction-sections -fdata-sections -fvisibility=default"
ARG EXT_LDFLAGS="-static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all -Wl,--exclude-libs,ALL"

# --------------------------------------------------------------------
# Stage: fractalsql.so builder. Uses the vendored core .a — no LuaJIT,
# no upstream-source build.
# --------------------------------------------------------------------
FROM ${BASE_IMAGE} AS builder
# apk's build-base bundles gcc + g++ + make + binutils. file is added
# explicitly because alpine doesn't include it by default.
RUN if command -v apk >/dev/null 2>&1; then \
        apk add --no-cache build-base musl-dev binutils file openssl-dev ; \
    elif command -v dnf >/dev/null 2>&1; then \
        dnf install -y --setopt=install_weak_deps=False --enablerepo=crb \
            gcc gcc-c++ libstdc++-static make binutils file openssl-devel \
        && dnf clean all ; \
    else \
        yum install -y gcc gcc-c++ libstdc++-static make binutils file openssl-devel ; \
    fi
WORKDIR /build
COPY VERSION             /build/VERSION
COPY src/                /build/src/
COPY include/            /build/include/
COPY docker/assert_so.sh /build/docker/assert_so.sh

ARG EXT_CFLAGS
ARG EXT_LDFLAGS
ARG SIZE_CEILING_BYTES
ARG OUTPUT_SUFFIX
ARG GLIBC_CEILING
ARG LIBC_KIND
ARG CORE_VARIANT

# The vendored core is laid out per platform: include/<os>-<arch>/, where
# arch is what `uname -m` prints inside the build container (x86_64 or
# aarch64). redismodule.h is vendored too (include/redismodule.h): nothing
# is fetched at build time.
RUN set -eu; \
    plat="linux-$(uname -m)"; \
    a="/build/include/${plat}/libfractalsql-${CORE_VARIANT}.a"; \
    if [ ! -f "${a}" ]; then \
        echo "ERROR: vendored core archive missing: include/${plat}/libfractalsql-${CORE_VARIANT}.a" >&2; \
        echo "  Refresh from foundry:" >&2; \
        echo "    cd ../fractalsql-core && make validated-drop-native && ./scripts/deploy.sh --git fractalsql-valkey" >&2; \
        exit 1; \
    fi; \
    test -f /build/include/redismodule.h; \
    echo "${plat}" > /platform.txt

RUN set -eux; \
    ver="$(tr -d '[:space:]' < /build/VERSION)"; \
    mkdir -p /build/obj; \
    # The float paths (the shared precision header, the vector functions, the search, the index and the agents) are
    # built with contraction and vectorisation OFF, so every distance, norm and cosine is a fixed sequence of IEEE
    # double operations and the same input gives the same bits on every platform. The other files are built as before.
    # The list matches the Makefile's FP_FLAGS targets: the same files carry the float paths in both builds, so the
    # module CI tests is the module a release ships.
    for f in src/fv_vector.c src/fv_search.c src/fv_index.c src/fv_agents.c; do \
        gcc -Wall -Wextra -O3 -fPIC -ffp-contract=off -fno-tree-vectorize \
            -Iinclude \
            "-DFRACTALSQL_VERSION=\"${ver}\"" \
            ${EXT_CFLAGS} \
            -c "$f" -o "/build/obj/$(basename "$f" .c).o"; \
    done; \
    for f in $(ls src/*.c | grep -v -e 'src/fv_vector.c' -e 'src/fv_search.c' -e 'src/fv_index.c' -e 'src/fv_agents.c'); do \
        gcc -Wall -Wextra -O3 -fPIC \
            -Iinclude \
            "-DFRACTALSQL_VERSION=\"${ver}\"" \
            ${EXT_CFLAGS} \
            -c "$f" -o "/build/obj/$(basename "$f" .c).o"; \
    done; \
    gcc -shared -o "/fractalsql${OUTPUT_SUFFIX}.so" \
        /build/obj/*.o \
        "/build/include/$(cat /platform.txt)/libfractalsql-${CORE_VARIANT}.a" \
        -lm -ldl -lpthread \
        ${EXT_LDFLAGS}; \
    sh /build/docker/assert_so.sh "/fractalsql${OUTPUT_SUFFIX}.so" \
        "${SIZE_CEILING_BYTES}" "${LIBC_KIND}"

# The MCP server (fractalsql-valkey-mcp), built for the same (arch, profile) as the module and shipped beside it. It
# is an ordinary C++17 client of Valkey (it speaks RESP itself): it needs a compiler and OpenSSL's headers only.
# libstdc++ is linked statically (libgcc_s stays dynamic: a static libgcc pulls in _dl_find_object and raises the glibc floor to 2.35),
# so the one binary runs on any distro that has libssl.
COPY mcp/ /build/mcp/
RUN make -C /build/mcp CXXFLAGS="-O2" SANFLAGS="-static-libstdc++" \
        BUILD=/build/mcp-obj OUT="/fractalsql-valkey-mcp${OUTPUT_SUFFIX}" && \
    strip "/fractalsql-valkey-mcp${OUTPUT_SUFFIX}" && \
    "/fractalsql-valkey-mcp${OUTPUT_SUFFIX}" --version

# --------------------------------------------------------------------
# Export stage — scratch, so `buildx --output=local` drops just the .so.
# --------------------------------------------------------------------
FROM scratch AS export
ARG OUTPUT_SUFFIX
COPY --from=builder "/fractalsql${OUTPUT_SUFFIX}.so" "/fractalsql${OUTPUT_SUFFIX}.so"
COPY --from=builder "/fractalsql-valkey-mcp${OUTPUT_SUFFIX}" "/fractalsql-valkey-mcp${OUTPUT_SUFFIX}"
# The MCP server's tool-surface and safety-annotation locks, beside the
# binary that reads them: without them the server refuses to start,
# because a missing lock is not a pass.
COPY --from=builder "/build/mcp/.mcp-schema-lock" "/.mcp-schema-lock"
COPY --from=builder "/build/mcp/.mcp-safety-annotations-lock" "/.mcp-safety-annotations-lock"

# --------------------------------------------------------------------
# daemon-pkg-builder / daemon-export: the daemon for the fractalsqld-valkey
# packages (.deb, .rpm and the Alpine .apk), built on the same base as the module
# so the libc floor and the OpenSSL soname match what the package declares. The
# musl profile builds it on Alpine against the musl core archive.
# --------------------------------------------------------------------
FROM ${BASE_IMAGE} AS daemon-pkg-builder
ARG LIBC_KIND
RUN if command -v apk >/dev/null 2>&1; then \
        apk add --no-cache build-base binutils openssl-dev python3 ; \
    else \
        dnf install -y --setopt=install_weak_deps=False --enablerepo=crb \
            gcc make binutils openssl-devel python3 \
        && dnf clean all ; \
    fi
WORKDIR /build
COPY include/ /build/include/
COPY service/ /build/service/
# The musl profile links the musl core archive (the glibc one pulls in glibc-only fortify symbols).
RUN if [ "${LIBC_KIND}" = "musl" ]; then \
        make -C service clean daemon \
            "CORE_ARCHIVE=../include/linux-$(uname -m)/libfractalsql-community-sovereign-c-musl.a" ; \
    else \
        make -C service clean daemon ; \
    fi

FROM scratch AS daemon-export
ARG OUTPUT_SUFFIX
COPY --from=daemon-pkg-builder /build/service/build/fractalsqld "/fractalsqld${OUTPUT_SUFFIX}"

# --------------------------------------------------------------------
# service-builder: the Apache-2.0 daemon fractalsqld (service/ plus the adapter
# it compiles). It is
# the only process that links the core archive and speaks to a model.
# Built on the runtime base so glibc and libcrypto match what runs it.
# --------------------------------------------------------------------
FROM valkey/valkey:${VALKEY_VERSION}@${VALKEY_IMAGE_DIGEST} AS service-builder
USER root
ARG VALKEY_VERSION
# The digest is authoritative over the tag, so a VALKEY_VERSION override that was not paired with its
# digest would silently build on the pinned 8.1 image. Fail loudly instead.
RUN valkey-server --version | grep -q "v=${VALKEY_VERSION}\." \
 || { echo "VALKEY_VERSION=${VALKEY_VERSION} does not match the base image's server version; set VALKEY_IMAGE_DIGEST to that version's digest (docker buildx imagetools inspect valkey/valkey:${VALKEY_VERSION})"; exit 1; }
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential libssl-dev python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY include/ /build/include/
COPY service/ /build/service/
RUN make -C service clean daemon

# --------------------------------------------------------------------
# fractalsqld: the daemon container for docker-compose.yml. It holds the
# core and the reasoning plugin and talks to the valkey container over a
# unix socket in the shared volume /run/fractalsql. It runs as the valkey
# user (uid 999), the uid its allowed_uids admits.
# --------------------------------------------------------------------
FROM valkey/valkey:${VALKEY_VERSION}@${VALKEY_IMAGE_DIGEST} AS fractalsqld
USER root
RUN apt-get update \
 && { apt-get install -y --no-install-recommends libcurl4t64 || apt-get install -y --no-install-recommends libcurl4; } \
 && rm -rf /var/lib/apt/lists/*
COPY --from=service-builder /build/service/build/fractalsqld /usr/local/bin/fractalsqld
COPY include/linux-x86_64/fractalsql-reasoning-http.so  /usr/lib/fractalsql/fractalsql-reasoning-http.x86_64.so
COPY include/linux-aarch64/fractalsql-reasoning-http.so /usr/lib/fractalsql/fractalsql-reasoning-http.aarch64.so
# Each plugin's detached signature travels with it and is renamed the same way, so the installed
# plugin finds <plugin>.sig beside it.
COPY include/linux-x86_64/fractalsql-reasoning-http.so.sig  /usr/lib/fractalsql/fractalsql-reasoning-http.x86_64.so.sig
COPY include/linux-aarch64/fractalsql-reasoning-http.so.sig /usr/lib/fractalsql/fractalsql-reasoning-http.aarch64.so.sig
RUN set -e \
 && ARCH=$(uname -m) \
 && mv "/usr/lib/fractalsql/fractalsql-reasoning-http.${ARCH}.so" /usr/lib/fractalsql/fractalsql-reasoning-http.so \
 && mv "/usr/lib/fractalsql/fractalsql-reasoning-http.${ARCH}.so.sig" /usr/lib/fractalsql/fractalsql-reasoning-http.so.sig \
 && rm -f /usr/lib/fractalsql/fractalsql-reasoning-http.*.so /usr/lib/fractalsql/fractalsql-reasoning-http.*.so.sig \
 && install -d -m 0750 -o valkey -g valkey /run/fractalsql
COPY docker/fsql-fractalsqld-entrypoint.sh /usr/local/bin/fsql-fractalsqld-entrypoint.sh
RUN chmod 0755 /usr/local/bin/fsql-fractalsqld-entrypoint.sh
USER valkey
ENTRYPOINT ["/usr/local/bin/fsql-fractalsqld-entrypoint.sh"]

# --------------------------------------------------------------------
# mcp-builder: the MCP server (fractalsql-valkey-mcp). It is an ordinary C++17 client of Valkey: it speaks RESP
# itself and links neither a Valkey client library nor the module, so it builds with a compiler and OpenSSL only.
# --------------------------------------------------------------------
FROM valkey/valkey:${VALKEY_VERSION}@${VALKEY_IMAGE_DIGEST} AS mcp-builder
USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY mcp/ /build/mcp/
RUN make -C mcp CXXFLAGS="-O2" BUILD=/build/obj OUT=/build/fractalsql-valkey-mcp

# --------------------------------------------------------------------
# demo: a RUNNABLE Valkey with the module installed, for docker-compose.yml.
# The core and the reasoning plugin run in the fractalsqld container; this
# one loads only the module and reaches the daemon over the shared socket.
# /demo and /bench ride along so `docker compose exec valkey /demo/run_demo.sh`
# works. They need python3 (standard library only) and, for bench/head_to_head.py
# alone, numpy.
# --------------------------------------------------------------------
FROM valkey/valkey:${VALKEY_VERSION}@${VALKEY_IMAGE_DIGEST} AS demo
USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 python3-numpy libssl3t64 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /fractalsql.so /usr/lib/valkey/fractalsql.so
# The MCP server, so `docker compose exec -T valkey fractalsql-valkey-mcp` works out of the box.
COPY --from=mcp-builder /build/fractalsql-valkey-mcp /usr/local/bin/fractalsql-valkey-mcp
# The MCP server's tool-surface and safety-annotation locks, beside the
# binary that reads them: without them the server refuses to start,
# because a missing lock is not a pass.
COPY --from=mcp-builder /build/mcp/.mcp-schema-lock /usr/local/bin/.mcp-schema-lock
COPY --from=mcp-builder /build/mcp/.mcp-safety-annotations-lock /usr/local/bin/.mcp-safety-annotations-lock
COPY bench/ /bench/
# Every demo, runnable on demand: python3 /demo/<demo>.py, or all of them in one pass with
#   docker compose exec valkey /demo/run_demo.sh
COPY demo/ /demo/
# The exec bit is set HERE, not carried in by COPY: file modes are not tracked, so the
# checked-in scripts may arrive 644 and be unrunnable in the image.
RUN chmod 0755 /demo/run_demo.sh /demo/demo-workload.sh /demo/*.py /bench/*.py /bench/scale_sweep.sh \
 && chown -R valkey:valkey /demo /bench
USER valkey
