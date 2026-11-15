#!/bin/bash
#
# fractalsql-valkey Docker build (v1: pure-C, vendored core archive).
#
# Drives docker/Dockerfile to produce fractalsql.so for one (arch, profile)
# combination per invocation:
#   dist/amd64/fractalsql.so              # modern (glibc 2.34+)
#   dist/amd64/fractalsql-musl.so         # musl
#
# Usage:
#   ./build.sh [amd64|arm64] [--profile=modern|musl]
#   PROFILE=musl ./build.sh amd64
#
# Profiles:
#   modern (default)  base rockylinux:9, glibc 2.34, GCC 11, no suffix
#   musl              base alpine:3.20, musl 1.2, -musl suffix
#
# glibc 2.34 is the floor. There is no glibc 2.28 ("legacy") profile.
#
# v1 architecture limitation:
#   The vendored libfractalsql-community-minimal-c.a in include/ is
#   single-arch (matches the host that ran core's `make
#   validated-drop-native`). v1 supports amd64 only. Multi-arch
#   tracking issue lives at the fleet level.
#
# Refresh the vendored archive from the foundry:
#   cd ../fractalsql-core && make validated-drop-native
#   ./scripts/deploy.sh --git fractalsql-valkey

set -euo pipefail

ARCH="${1:-amd64}"
case "${ARCH}" in
    amd64) ;;
    arm64) ;;  # Native build on ubuntu-24.04-arm runners.
    *) echo "unknown arch '${ARCH}' — expected amd64 or arm64" >&2; exit 2 ;;
esac

PROFILE="${PROFILE:-modern}"
# Everything after the arch positional must be a --profile=... flag. (A
# counter skips the first argument instead of "${@:2}", which breaks under
# `set -u` with zero or one args on old bash -- macOS ships 3.2.)
_arg_i=0
for arg in "$@"; do
    _arg_i=$((_arg_i + 1))
    [[ ${_arg_i} -eq 1 ]] && continue
    case "$arg" in
        --profile=*) PROFILE="${arg#--profile=}" ;;
        *) echo "unknown argument '${arg}' — expected --profile=modern|musl" >&2; exit 2 ;;
    esac
done
case "${PROFILE}" in
    modern|musl) ;;
    *) echo "unknown profile '${PROFILE}' — expected modern or musl" >&2; exit 2 ;;
esac

if [[ "${PROFILE}" = "musl" ]]; then
    BASE_IMAGE="alpine:3.20"
    OUTPUT_SUFFIX="-musl"
    GLIBC_CEILING=""
    LIBC_KIND="musl"
    SIZE_CEILING_BYTES="3145728"   # 3 MB
    CORE_VARIANT="community-sovereign-c-musl"
else
    BASE_IMAGE="rockylinux:9"
    OUTPUT_SUFFIX=""
    GLIBC_CEILING="34"
    LIBC_KIND="glibc"
    SIZE_CEILING_BYTES="3145728"   # 3 MB
    CORE_VARIANT="community-sovereign-c"
fi

DIST_DIR="${DIST_DIR:-./dist}"
DOCKERFILE="${DOCKERFILE:-docker/Dockerfile}"
PLATFORM="linux/${ARCH}"
OUT_DIR="${DIST_DIR}/${ARCH}"

mkdir -p "${OUT_DIR}"

echo "------------------------------------------"
echo "Building fractalsql-valkey"
echo "  arch:     ${PLATFORM}"
echo "  profile:  ${PROFILE}"
echo "  base:     ${BASE_IMAGE}"
echo "  output:   ${OUT_DIR}/fractalsql${OUTPUT_SUFFIX}.so (+ fractalsql-valkey-mcp${OUTPUT_SUFFIX})"
if [[ "${LIBC_KIND}" = "musl" ]]; then
    echo "  libc:     musl 1.2 (alpine 3.20)"
else
    echo "  glibc<=:  2.${GLIBC_CEILING}"
fi
echo "------------------------------------------"

DOCKER_BUILDKIT=1 docker buildx build \
    --platform "${PLATFORM}" \
    --target export \
    --output "type=local,dest=${OUT_DIR}" \
    --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
    --build-arg "OUTPUT_SUFFIX=${OUTPUT_SUFFIX}" \
    --build-arg "GLIBC_CEILING=${GLIBC_CEILING}" \
    --build-arg "LIBC_KIND=${LIBC_KIND}" \
    --build-arg "SIZE_CEILING_BYTES=${SIZE_CEILING_BYTES}" \
    --build-arg "CORE_VARIANT=${CORE_VARIANT}" \
    -f "${DOCKERFILE}" \
    .

# The daemon for the fractalsqld-valkey packages, built on the same base as the module (so the libc and OpenSSL
# match what the package declares).
DOCKER_BUILDKIT=1 docker buildx build \
    --platform "${PLATFORM}" \
    --target daemon-export \
    --output "type=local,dest=${OUT_DIR}" \
    --build-arg "BASE_IMAGE=${BASE_IMAGE}" \
    --build-arg "LIBC_KIND=${LIBC_KIND}" \
    --build-arg "OUTPUT_SUFFIX=${OUTPUT_SUFFIX}" \
    -f "${DOCKERFILE}" \
    .
ls -l "${OUT_DIR}/fractalsqld${OUTPUT_SUFFIX}"

echo
echo "Built artifact for ${ARCH}/${PROFILE}:"
ls -l "${OUT_DIR}/fractalsql${OUTPUT_SUFFIX}.so"
ls -l "${OUT_DIR}/fractalsql-valkey-mcp${OUTPUT_SUFFIX}"
file "${OUT_DIR}/fractalsql${OUTPUT_SUFFIX}.so" 2>/dev/null || true
