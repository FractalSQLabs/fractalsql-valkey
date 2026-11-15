#!/usr/bin/env bash
#
# scripts/package.sh — fractalsql-valkey packaging.
#
# Assumes ./build.sh ${ARCH} has produced:
#   dist/${ARCH}/fractalsql.so
#
# The glibc profile also needs, for the fractalsqld-valkey packages:
#   dist/${ARCH}/fractalsqld               (the daemon, built by build.sh)
#   dist/${ARCH}/fractalsql-valkey-mcp     (the MCP server, built by build.sh)
#   include/linux-<arch>/fractalsql-reasoning-http.so and its .sig
#
# Emits into dist/packages/, per arch:
#   fractalsql-valkey-<arch>.deb / .rpm    the module only: glibc and the Valkey server are its only dependencies
#   fractalsqld-valkey-<arch>.deb / .rpm   the daemon, the reasoning plugin with its detached signature, the MCP
#                                          server, a systemd unit and a starter config (libcurl and OpenSSL)
#   fractalsql-valkey-musl-<arch>.apk      Alpine: the module only
#   fractalsqld-valkey-musl-<arch>.apk     Alpine: the same daemon package as the .deb and .rpm, built for musl,
#                                          with an OpenRC service
#
# One binary covers Valkey 7.2 / 8.0 / 8.1 — the Modules ABI
# (REDISMODULE_APIVER_1 / VALKEYMODULE_APIVER_1) was preserved across
# the April 2024 fork from Redis, so the package depends on
# valkey-server / valkey generically rather than pinning a major.
#
# Usage:
#   scripts/package.sh [amd64|arm64]              # modern (default)
#   PROFILE=musl scripts/package.sh amd64         # Alpine .apk

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="$(tr -d '[:space:]' < VERSION)"
ITERATION="1"
DIST_DIR="dist/packages"

# PROFILE selector — picks which build.sh artifact to package and what
# the resulting package is named + depends on. The GLIBC_DEP value
# matches the build target in build.sh: modern compiles inside
# rockylinux:9 (glibc 2.34), musl inside alpine:3.20 (musl 1.2).
#   modern (default)  → fractalsql-valkey,        libc6 >= 2.34, .deb+.rpm
#   musl              → fractalsql-valkey-musl,   apk only
PROFILE="${PROFILE:-modern}"
case "${PROFILE}" in
    modern) PKG_SUFFIX="" ; BIN_SUFFIX="" ; GLIBC_DEP="2.34" ;;
    musl)   PKG_SUFFIX="-musl"   ; BIN_SUFFIX="-musl"   ; GLIBC_DEP="" ;;
    *) echo "unknown profile '${PROFILE}' — expected modern or musl" >&2; exit 2 ;;
esac

PKG_NAME="fractalsql-valkey${PKG_SUFFIX}"
mkdir -p "${DIST_DIR}"

# Absolute repo root, captured before any -C chdir'd fpm invocation.
REPO_ROOT="$(pwd)"
for f in LICENSE THIRD-PARTY-NOTICES.md; do
    if [[ ! -f "${REPO_ROOT}/${f}" ]]; then
        echo "missing ${REPO_ROOT}/${f} — refusing to package without it" >&2
        exit 1
    fi
done

PKG_ARCH="${1:-amd64}"
case "${PKG_ARCH}" in
    amd64|arm64) ;;
    *)
        echo "unknown arch '${PKG_ARCH}' — expected amd64 or arm64" >&2
        exit 2
        ;;
esac

case "${PKG_ARCH}" in
    amd64) RPM_ARCH="x86_64"  ;;
    arm64) RPM_ARCH="aarch64" ;;
    *) echo "internal error: unhandled arch '${PKG_ARCH}' (validated above)" >&2; exit 2 ;;
esac

SO="dist/${PKG_ARCH}/fractalsql${BIN_SUFFIX}.so"
if [[ ! -f "${SO}" ]]; then
    echo "missing ${SO} — run ./build.sh ${PKG_ARCH} --profile=${PROFILE} first" >&2
    exit 1
fi

# The MCP server, built by build.sh next to the module for the same (arch, profile). Optional: without it the packages
# carry the module alone, as before.
MCP_BIN="dist/${PKG_ARCH}/fractalsql-valkey-mcp${BIN_SUFFIX}"
HAVE_MCP=0
if [[ -f "${MCP_BIN}" ]]; then
    HAVE_MCP=1
else
    echo "warning: ${MCP_BIN} not found — packaging without the MCP server (run ./build.sh ${PKG_ARCH} --profile=${PROFILE})" >&2
fi

DEB_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.deb"
RPM_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.rpm"
APK_OUT="${DIST_DIR}/${PKG_NAME}-${PKG_ARCH}.apk"

# Build per-format staging roots. Valkey modules are loaded via
# `loadmodule <absolute-path>` in valkey.conf, so the library path
# itself is arbitrary — but RHEL-family packaging conventionally
# installs 64-bit modules under /usr/lib64/ rather than /usr/lib/.
# Matching each distro's convention keeps admins unsurprised and
# the shipped `.conf` snippet pointing at the real install location
# on both distros.
#
#   Debian/Ubuntu (apt valkey-server):
#       /usr/lib/valkey/modules/fractalsql.so
#   RHEL/CentOS/Oracle Linux (yum valkey):
#       /usr/lib64/valkey/modules/fractalsql.so
#
# The `.conf` file must match wherever the `.so` actually lives on
# the target distro, so we emit per-distro snippets by rewriting
# the `loadmodule` line from the checked-in template.
#
# LICENSE ledger: staged into /usr/share/doc/<pkg>/ via install -Dm0644
# BEFORE running fpm. Explicit fpm src=dst mappings break here — fpm's
# -C chroots absolute source paths too, so ${REPO_ROOT}/LICENSE gets
# resolved as ${STAGE}${REPO_ROOT}/LICENSE and fpm bails with
# "Cannot chdir to ...".
STAGE_DEB="$(mktemp -d)"
STAGE_RPM="$(mktemp -d)"
STAGE_APK="$(mktemp -d)"
trap 'rm -rf "${STAGE_DEB}" "${STAGE_RPM}" "${STAGE_APK}"' EXIT

emit_conf() {
    # Rewrite the `loadmodule <path>` line from the template to point
    # at the per-distro install path. Everything else (comments,
    # systemd hints, post-install probe commands) is preserved.
    local module_path="$1"
    local out_path="$2"
    mkdir -p "$(dirname "${out_path}")"
    sed -e "s|^loadmodule .*|loadmodule ${module_path}|" \
        "${REPO_ROOT}/scripts/load_module.conf" \
        > "${out_path}"
    chmod 0644 "${out_path}"
}

stage_common_docs() {
    local stage="$1"
    install -Dm0644 "${REPO_ROOT}/LICENSE" \
        "${stage}/usr/share/doc/${PKG_NAME}/LICENSE"
    install -Dm0644 "${REPO_ROOT}/THIRD-PARTY-NOTICES.md" \
        "${stage}/usr/share/doc/${PKG_NAME}/LICENSE-THIRD-PARTY"
}

# Debian layout.
install -Dm0755 "${SO}" \
    "${STAGE_DEB}/usr/lib/valkey/modules/fractalsql.so"
emit_conf "/usr/lib/valkey/modules/fractalsql.so" \
    "${STAGE_DEB}/etc/valkey/modules-available/fractalsql.conf"
stage_common_docs "${STAGE_DEB}"

# RHEL layout (/usr/lib64/).
install -Dm0755 "${SO}" \
    "${STAGE_RPM}/usr/lib64/valkey/modules/fractalsql.so"
emit_conf "/usr/lib64/valkey/modules/fractalsql.so" \
    "${STAGE_RPM}/etc/valkey/modules-available/fractalsql.conf"
stage_common_docs "${STAGE_RPM}"

# Alpine layout. Alpine's `valkey` package (3.20+) follows Debian's
# tree convention (/usr/lib/) rather than RHEL's lib64.
install -Dm0755 "${SO}" \
    "${STAGE_APK}/usr/lib/valkey/modules/fractalsql.so"
emit_conf "/usr/lib/valkey/modules/fractalsql.so" \
    "${STAGE_APK}/etc/valkey/modules-available/fractalsql.conf"
stage_common_docs "${STAGE_APK}"

echo "------------------------------------------"
echo "Packaging ${PKG_NAME} (${PKG_ARCH})"
echo "------------------------------------------"

DESC="FractalSQL for Valkey 7.2 / 8.0 / 8.1: Stochastic Fractal Search retrieval, sessions, vector math, analytics and agents as ordinary commands"

# Pure-C SFS core is statically linked — no libluajit dependency.
# Depends on valkey-server (Debian) / valkey (RHEL/Alpine). Valkey is
# a drop-in Redis replacement but the upstream package name differs.
#
# musl profile skips .deb / .rpm — those declare libc6/glibc deps
# that don't apply on Alpine. Alpine consumers get the .apk.
# The MCP server links libssl/libcrypto dynamically (libstdc++ and libgcc are static), so a package that includes it
# must also pull in OpenSSL's runtime library.
# The module packages stay free of OpenSSL and libcurl on every platform: the MCP server ships in the
# fractalsqld-valkey package (.deb, .rpm and Alpine .apk), with the daemon and the signed reasoning plugin.

if [[ "${PROFILE}" != "musl" ]]; then
fpm -s dir -t deb \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${PKG_ARCH}" \
    --iteration "${ITERATION}" \
    --description "${DESC}" \
    --license "Apache-2.0 AND BSD-2-Clause" \
    --maintainer "FractalSQLabs" \
    --vendor "FractalSQLabs" \
    --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
    --depends "libc6 (>= ${GLIBC_DEP})" \
    --depends "valkey-server" \
    --deb-suggests "fractalsqld-valkey" \
    --config-files /etc/valkey/modules-available/fractalsql.conf \
    --after-install "${REPO_ROOT}/packaging/debian/postinst" \
    -C "${STAGE_DEB}" \
    -p "${DEB_OUT}" \
    usr etc

fpm -s dir -t rpm \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${RPM_ARCH}" \
    --iteration "${ITERATION}" \
    --description "${DESC}" \
    --license "Apache-2.0 AND BSD-2-Clause" \
    --maintainer "FractalSQLabs" \
    --vendor "FractalSQLabs" \
    --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
    --depends "valkey" \
    --config-files /etc/valkey/modules-available/fractalsql.conf \
    --after-install "${REPO_ROOT}/packaging/debian/postinst" \
    -C "${STAGE_RPM}" \
    -p "${RPM_OUT}" \
    usr etc
fi

# Alpine .apk — only built for the musl profile. Initial release is
# unsigned (consumer installs with `apk add --allow-untrusted`).
if [[ "${PROFILE}" = "musl" ]]; then
fpm -s dir -t apk \
    -n "${PKG_NAME}" \
    -v "${VERSION}" \
    -a "${PKG_ARCH}" \
    --iteration "${ITERATION}" \
    --description "${DESC}" \
    --license "Apache-2.0 AND BSD-2-Clause" \
    --maintainer "FractalSQLabs" \
    --vendor "FractalSQLabs" \
    --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
    --depends "valkey" \
    --config-files /etc/valkey/modules-available/fractalsql.conf \
    --after-install "${REPO_ROOT}/packaging/debian/postinst" \
    -C "${STAGE_APK}" \
    -p "${APK_OUT}" \
    usr etc
fi


# ---------------------------------------------------------------------
# fractalsqld-valkey packages: .deb and .rpm on the glibc profile, an .apk with an OpenRC service on musl. The daemon runs
# reasoning, embeddings and the enterprise ledger outside the
# Valkey process; it loads the reasoning plugin, which loads only with its detached Ed25519 signature beside it. The MCP
# server rides along: it shares the OpenSSL dependency and, like reasoning, is optional to Valkey.
# ---------------------------------------------------------------------
DAEMON_BIN="dist/${PKG_ARCH}/fractalsqld${BIN_SUFFIX}"
case "${PKG_ARCH}" in
    amd64) PLUGIN_INC="linux-x86_64" ;;
    arm64) PLUGIN_INC="linux-aarch64" ;;
esac
[[ "${PROFILE}" = "musl" ]] && PLUGIN_INC="linux-musl-${PLUGIN_INC#linux-}"
PLUGIN_SRC="include/${PLUGIN_INC}/fractalsql-reasoning-http.so"
PLUGIN_SIG="${PLUGIN_SRC}.sig"
[[ -f "${DAEMON_BIN}" ]] || { echo "missing ${DAEMON_BIN} — run ./build.sh ${PKG_ARCH} --profile=${PROFILE} first" >&2; exit 1; }
[[ -f "${PLUGIN_SRC}" ]] || { echo "missing ${PLUGIN_SRC}: re-run the vendored-artifact deploy step" >&2; exit 1; }
# The plugin loads only with its signature (exactly 64 bytes) beside it, so a package must never carry one
# without the other.
if [[ ! -f "${PLUGIN_SIG}" || "$(wc -c < "${PLUGIN_SIG}")" -ne 64 ]]; then
    echo "missing or invalid ${PLUGIN_SIG} (needs 64 bytes): re-run the vendored-artifact deploy step" >&2
    exit 1
fi

D_NAME="fractalsqld-valkey${PKG_SUFFIX}"
D_DESC="FractalSQL daemon for Valkey: reasoning, embeddings and the enterprise ledger outside the server process, with the signed reasoning plugin and the MCP server"
D_STAGE_DEB="$(mktemp -d)"
D_STAGE_RPM="$(mktemp -d)"
D_STAGE_APK="$(mktemp -d)"
trap 'rm -rf "${STAGE_DEB}" "${STAGE_RPM}" "${STAGE_APK}" "${D_STAGE_DEB}" "${D_STAGE_RPM}" "${D_STAGE_APK}"' EXIT

# $1 = staging root, $2 = plugin directory (/usr/lib/fractalsql on Debian and Alpine, /usr/lib64/fractalsql on RHEL)
stage_daemon() {
    local stage="$1" plugdir="$2"
    install -Dm0755 "${REPO_ROOT}/${DAEMON_BIN}" "${stage}/usr/libexec/fractalsql/fractalsqld"
    install -Dm0755 "${REPO_ROOT}/${PLUGIN_SRC}" "${stage}${plugdir}/fractalsql-reasoning-http.so"
    install -Dm0644 "${REPO_ROOT}/${PLUGIN_SIG}" "${stage}${plugdir}/fractalsql-reasoning-http.so.sig"
    install -Dm0644 "${REPO_ROOT}/LICENSE" "${stage}/usr/share/doc/${D_NAME}/LICENSE"
    install -Dm0644 "${REPO_ROOT}/THIRD-PARTY-NOTICES.md" "${stage}/usr/share/doc/${D_NAME}/LICENSE-THIRD-PARTY"
    if [[ "${HAVE_MCP}" = "1" ]]; then
        install -Dm0755 "${REPO_ROOT}/${MCP_BIN}" "${stage}/usr/bin/fractalsql-valkey-mcp"
        install -Dm0644 "${REPO_ROOT}/mcp/.mcp-schema-lock" "${stage}/usr/bin/.mcp-schema-lock"
        install -Dm0644 "${REPO_ROOT}/mcp/.mcp-safety-annotations-lock" "${stage}/usr/bin/.mcp-safety-annotations-lock"
        install -Dm0644 "${REPO_ROOT}/mcp/README.md" "${stage}/usr/share/doc/${D_NAME}/mcp/README.md"
        install -Dm0644 "${REPO_ROOT}/mcp/RUN.txt" "${stage}/usr/share/doc/${D_NAME}/mcp/RUN.txt"
        install -Dm0644 "${REPO_ROOT}/mcp/manifest.json" "${stage}/usr/share/doc/${D_NAME}/mcp/manifest.json"
    fi
}

if [[ "${PROFILE}" != "musl" ]]; then
    D_DEB_OUT="${DIST_DIR}/${D_NAME}-${PKG_ARCH}.deb"
    D_RPM_OUT="${DIST_DIR}/${D_NAME}-${PKG_ARCH}.rpm"
    stage_daemon "${D_STAGE_DEB}" "/usr/lib/fractalsql"
    stage_daemon "${D_STAGE_RPM}" "/usr/lib64/fractalsql"
    install -Dm0644 "${REPO_ROOT}/packaging/fractalsqld/fractalsqld.service" "${D_STAGE_DEB}/usr/lib/systemd/system/fractalsqld.service"
    install -Dm0644 "${REPO_ROOT}/packaging/fractalsqld/fractalsqld.service" "${D_STAGE_RPM}/usr/lib/systemd/system/fractalsqld.service"

    fpm -s dir -t deb \
        -n "${D_NAME}" \
        -v "${VERSION}" \
        -a "${PKG_ARCH}" \
        --iteration "${ITERATION}" \
        --description "${D_DESC}" \
        --license "Apache-2.0" \
        --maintainer "FractalSQLabs" \
        --vendor "FractalSQLabs" \
        --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
        --depends "libc6 (>= ${GLIBC_DEP})" \
        --depends "libcurl4 | libcurl4t64" \
        --depends "libssl3 | libssl3t64" \
        --depends "openssl" \
        --deb-recommends "fractalsql-valkey" \
        --after-install "${REPO_ROOT}/packaging/fractalsqld/postinst.sh" \
        --before-remove "${REPO_ROOT}/packaging/fractalsqld/prerm.sh" \
        -C "${D_STAGE_DEB}" \
        -p "${D_DEB_OUT}" \
        usr

    fpm -s dir -t rpm \
        -n "${D_NAME}" \
        -v "${VERSION}" \
        -a "${RPM_ARCH}" \
        --iteration "${ITERATION}" \
        --description "${D_DESC}" \
        --license "Apache-2.0" \
        --maintainer "FractalSQLabs" \
        --vendor "FractalSQLabs" \
        --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
        --depends "libcurl.so.4()(64bit)" \
        --depends "libcrypto.so.3()(64bit)" \
        --depends "openssl-libs" \
        --depends "openssl" \
        --after-install "${REPO_ROOT}/packaging/fractalsqld/postinst.sh" \
        --before-remove "${REPO_ROOT}/packaging/fractalsqld/prerm.sh" \
        -C "${D_STAGE_RPM}" \
        -p "${D_RPM_OUT}" \
        usr
else
    # Alpine: the same contents, an OpenRC service instead of a systemd unit, and busybox install hooks.
    D_APK_OUT="${DIST_DIR}/${D_NAME}-${PKG_ARCH}.apk"
    stage_daemon "${D_STAGE_APK}" "/usr/lib/fractalsql"
    install -Dm0755 "${REPO_ROOT}/packaging/fractalsqld/fractalsqld.openrc" "${D_STAGE_APK}/etc/init.d/fractalsqld"

    fpm -s dir -t apk \
        -n "${D_NAME}" \
        -v "${VERSION}" \
        -a "${PKG_ARCH}" \
        --iteration "${ITERATION}" \
        --description "${D_DESC}" \
        --license "Apache-2.0" \
        --maintainer "FractalSQLabs" \
        --vendor "FractalSQLabs" \
        --url "https://github.com/FractalSQLabs/fractalsql-valkey" \
        --depends "libcurl" \
        --depends "libcrypto3" \
        --depends "libssl3" \
        --depends "openssl" \
        --after-install "${REPO_ROOT}/packaging/fractalsqld/postinst-apk.sh" \
        --before-remove "${REPO_ROOT}/packaging/fractalsqld/prerm-apk.sh" \
        -C "${D_STAGE_APK}" \
        -p "${D_APK_OUT}" \
        usr etc
fi

rm -rf "${STAGE_DEB}" "${STAGE_RPM}" "${STAGE_APK}" "${D_STAGE_DEB}" "${D_STAGE_RPM}" "${D_STAGE_APK}"
trap - EXIT

echo
echo "Done. Packages in ${DIST_DIR}:"
ls -l "${DIST_DIR}"
