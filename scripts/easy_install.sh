#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# scripts/easy_install.sh
#
# The "easy button" for FractalSQL on Valkey. One command gets you from a Linux
# or macOS box to a module file on disk and a config snippet that loads it.
#
# Usage (fresh machine):
#   curl -fsSL https://github.com/FractalSQLabs/fractalsql-valkey/releases/latest/download/easy_install.sh | bash
#
# Usage (from a clone, or with a package you already downloaded):
#   ./scripts/easy_install.sh
#   ./scripts/easy_install.sh --from-file ./fractalsql-valkey-amd64.deb
#
# Flags (all optional; anything you omit is asked, or defaulted with --yes):
#   --version <X.Y.Z>          release to install (default: this script's own)
#   --from-file <path>         install this .deb / .rpm / .zip / module file instead of downloading
#   --conf <valkey.conf>       also add an `include` of the snippet to this server config
#   --threads <N>              worker threads for the module (default: the module's own, 4)
#   --daemon-socket <path>     fractalsqld socket   (enables FRACTAL.REASON, EMBED, the ledger)
#   --daemon-key-file <path>   fractalsqld HMAC key file (must be given with --daemon-socket)
#   --yes                      pre-confirm every prompt (needed for CI and other non-tty runs)
#   --dry-run                  print what would happen, change nothing
#   --uninstall                remove what this script installed
#   -h, --help
#
# What it does:
#   - Linux: installs the .deb or .rpm for your architecture with apt or dnf (the module lands in
#     /usr/lib/valkey/modules and a snippet in /etc/valkey/modules-available). On a system with
#     neither (Alpine, a bare container) it unpacks the module into /usr/local/lib/valkey instead.
#   - macOS: unpacks the release zip into /usr/local/lib/valkey, clears the Gatekeeper quarantine
#     flag on the unsigned module, and writes the snippet next to it.
#   - Writes a `loadmodule` snippet (with any options you gave) and tells you how to use it.
#   - Never restarts Valkey. Loading is yours to do: restart the server, or MODULE LOAD at runtime.
#
# No telemetry. This script never reports anything anywhere.

set -euo pipefail

FSQL_VERSION="@@FSQL_VERSION@@"
if [[ "${FSQL_VERSION}" == "@@FSQL_VERSION@@" ]]; then
    HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [[ -f "${HERE}/../VERSION" ]]; then
        FSQL_VERSION="$(tr -d '[:space:]' < "${HERE}/../VERSION")"
    else
        FSQL_VERSION="latest"
    fi
fi

REPO="FractalSQLabs/fractalsql-valkey"
LOCAL_DIR="/usr/local/lib/valkey"

if [[ -t 1 ]]; then G="\033[32m"; R="\033[31m"; Y="\033[33m"; B="\033[1m"; Z="\033[0m"; else G=""; R=""; Y=""; B=""; Z=""; fi
log()  { printf "${B}==>${Z} %s\n" "$1"; }
ok()   { printf "  ${G}✓${Z} %s\n" "$1"; }
warn() { printf "  ${Y}!${Z} %s\n" "$1" >&2; }
err()  { printf "  ${R}✗${Z} %s\n" "$1" >&2; }
die()  { err "$1"; exit 1; }

# Every download is a release asset fetched over the network; pin curl to https.
fetch() {
    curl -fsSL --proto '=https' "$@" || return $?
    # Release assets are verified against the release's SHA256SUMS before anything installs them.
    # Set FSQL_ALLOW_UNVERIFIED=1 only for a release that predates SHA256SUMS.
    local url="" out="" prev="" a
    for a in "$@"; do
        [[ "$prev" == "-o" ]] && out="$a"
        [[ "$a" == https://* ]] && url="$a"
        prev="$a"
    done
    [[ -n "$url" && -n "$out" && "$url" == */releases/download/* && "${url##*/}" != "SHA256SUMS" ]] || return 0
    local sums="${out}.SHA256SUMS.check" want got
    if ! curl -fsSL --proto '=https' "${url%/*}/SHA256SUMS" -o "$sums" 2>/dev/null; then
        rm -f "$sums"
        if [[ "${FSQL_ALLOW_UNVERIFIED:-0}" == "1" ]]; then echo "WARNING: no SHA256SUMS for this release; installing unverified (FSQL_ALLOW_UNVERIFIED=1)" >&2; return 0; fi
        echo "ERROR: ${url%/*}/SHA256SUMS could not be fetched; refusing to install an unverified download (set FSQL_ALLOW_UNVERIFIED=1 to override)" >&2
        rm -f "$out"; return 1
    fi
    want="$(awk -v f="${url##*/}" '{n=$2; sub(/^\*/,"",n); if (n==f) {print $1; exit}}' "$sums")"
    rm -f "$sums"
    got="$(sha256sum "$out" 2>/dev/null | awk '{print $1}')"; [[ -n "$got" ]] || got="$(shasum -a 256 "$out" | awk '{print $1}')"
    if [[ -z "$want" || "$want" != "$got" ]]; then
        echo "ERROR: checksum mismatch or missing entry for ${url##*/} (expected '${want:-none}', got '$got'); removing the download" >&2
        rm -f "$out"; return 1
    fi
}

YES=0; DRY_RUN=0; UNINSTALL=0
FROM_FILE=""; CONF=""; THREADS=""; DSOCK=""; DKEY=""

usage() { sed -n '5,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)         FSQL_VERSION="${2:?--version needs a value}"; shift ;;
        --from-file)       FROM_FILE="${2:?--from-file needs a path}"; shift ;;
        --conf)            CONF="${2:?--conf needs a path}"; shift ;;
        --threads)         THREADS="${2:?--threads needs a value}"; shift ;;
        --daemon-socket)   DSOCK="${2:?--daemon-socket needs a path}"; shift ;;
        --daemon-key-file) DKEY="${2:?--daemon-key-file needs a path}"; shift ;;
        --yes|-y)          YES=1 ;;
        --dry-run)         DRY_RUN=1 ;;
        --uninstall)       UNINSTALL=1 ;;
        -h|--help)         usage; exit 0 ;;
        *)                 die "unknown argument '$1' (try --help)" ;;
    esac
    shift
done

# --- validate what we will write into a config file ------------------------
[[ -z "${THREADS}" || "${THREADS}" =~ ^[0-9]+$ && "${THREADS}" -ge 1 && "${THREADS}" -le 64 ]] \
    || die "--threads must be an integer from 1 to 64"
if [[ -n "${DSOCK}" || -n "${DKEY}" ]]; then
    [[ -n "${DSOCK}" && -n "${DKEY}" ]] || die "--daemon-socket and --daemon-key-file must be given together"
    [[ "${DSOCK}" == /* && "${DKEY}" == /* ]] || die "--daemon-socket and --daemon-key-file must be absolute paths"
fi
# A path ends up on a config line: refuse anything that could change what the line means.
for v in "${DSOCK}" "${DKEY}" "${CONF}"; do
    [[ -z "${v}" || "${v}" =~ ^[A-Za-z0-9_./+@:-]+$ ]] || die "'${v}' has characters that are not allowed in a path here"
done
[[ "${FSQL_VERSION}" == "latest" || "${FSQL_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?$ ]] \
    || die "--version '${FSQL_VERSION}' is not a version number"

run() { if [[ "${DRY_RUN}" -eq 1 ]]; then printf "  [dry-run] %s\n" "$*"; else "$@"; fi; }
sudo_run() {
    if [[ "$(id -u)" -eq 0 ]]; then run "$@"
    elif command -v sudo >/dev/null 2>&1; then run sudo "$@"
    else die "needs root to run: $* (install sudo, or run this script as root)"; fi
}

confirm() {
    [[ "${YES}" -eq 1 ]] && { ok "$1 -> yes (--yes)"; return 0; }
    [[ -e /dev/tty ]] || die "'$1' needs an answer but there is no terminal (pass --yes)"
    local reply; read -r -p "$1 [Y/n] " reply < /dev/tty || true
    [[ -z "${reply}" || "${reply}" =~ ^[Yy] ]]
}

# --- platform ---------------------------------------------------------------
OS="$(uname -s)"; MACH="$(uname -m)"
case "${MACH}" in
    x86_64|amd64)  ARCH=amd64;  OSX=osx_amd64 ;;
    aarch64|arm64) ARCH=arm64;  OSX=osx_arm64 ;;
    *) die "unsupported architecture '${MACH}' (the module is built for amd64 and arm64)" ;;
esac
case "${OS}" in
    Linux)  SNIPPET_DIR="/etc/valkey/modules-available" ;;
    Darwin) SNIPPET_DIR="${LOCAL_DIR}" ;;
    *) die "unsupported OS '${OS}' (Valkey runs on Linux and macOS)" ;;
esac
SNIPPET="${SNIPPET_DIR}/fractalsql.conf"
MARK="# added by fractalsql easy_install"

# --- uninstall --------------------------------------------------------------
if [[ "${UNINSTALL}" -eq 1 ]]; then
    log "Uninstalling FractalSQL for Valkey"
    if [[ "${OS}" == "Linux" ]] && command -v dpkg >/dev/null 2>&1 && dpkg -s fractalsql-valkey >/dev/null 2>&1; then
        sudo_run apt-get remove -y fractalsql-valkey
    elif [[ "${OS}" == "Linux" ]] && command -v rpm >/dev/null 2>&1 && rpm -q fractalsql-valkey >/dev/null 2>&1; then
        sudo_run dnf remove -y fractalsql-valkey
    fi
    for f in "${LOCAL_DIR}/fractalsql.so" "${LOCAL_DIR}/fractalsql.dylib" "${SNIPPET}"; do
        [[ -e "${f}" ]] && sudo_run rm -f "${f}"
    done
    if [[ -n "${CONF}" && -f "${CONF}" ]]; then
        sudo_run sed -i.bak "\|${MARK}|,+1d" "${CONF}"
        ok "removed the include from ${CONF} (backup: ${CONF}.bak)"
    fi
    ok "done. Running servers keep the module until you MODULE UNLOAD fractalsql or restart them."
    exit 0
fi

# --- find the package -------------------------------------------------------
log "FractalSQL for Valkey ${FSQL_VERSION} on ${OS}/${MACH}"
TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
BASE="https://github.com/${REPO}/releases"
if [[ "${FSQL_VERSION}" == "latest" ]]; then URL_BASE="${BASE}/latest/download"; else URL_BASE="${BASE}/download/v${FSQL_VERSION}"; fi

pm=""
if [[ "${OS}" == "Linux" ]]; then
    if   command -v apt-get >/dev/null 2>&1 && command -v dpkg >/dev/null 2>&1; then pm=deb
    elif command -v dnf     >/dev/null 2>&1; then pm=rpm
    elif command -v yum     >/dev/null 2>&1; then pm=rpm
    fi
fi

PKG=""
if [[ -n "${FROM_FILE}" ]]; then
    [[ -f "${FROM_FILE}" ]] || die "--from-file: no such file '${FROM_FILE}'"
    PKG="${FROM_FILE}"
    ok "using ${PKG}"
else
    if [[ "${OS}" == "Darwin" ]]; then asset="fractalsql-valkey-${OSX}.zip"
    elif [[ -n "${pm}" ]]; then asset="fractalsql-valkey-${ARCH}.${pm}"
    else asset="fractalsql-valkey-musl-${ARCH}.apk"; fi
    log "Downloading ${asset}"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        printf "  [dry-run] curl -fsSL --proto '=https' -o %s %s/%s\n" "${asset}" "${URL_BASE}" "${asset}"
        PKG="${TMP}/${asset}"
    else
        fetch -o "${TMP}/${asset}" "${URL_BASE}/${asset}" || die "could not download ${URL_BASE}/${asset}"
        PKG="${TMP}/${asset}"
        ok "downloaded ${asset}"
    fi
fi

# --- install ----------------------------------------------------------------
INSTALLED_SO=""
case "${PKG}" in
    *.deb)
        confirm "Install ${PKG##*/} with apt?" || die "cancelled"
        sudo_run apt-get install -y "${PKG}"
        INSTALLED_SO="/usr/lib/valkey/modules/fractalsql.so" ;;
    *.rpm)
        confirm "Install ${PKG##*/} with dnf?" || die "cancelled"
        sudo_run "$(command -v dnf || command -v yum)" install -y "${PKG}"
        INSTALLED_SO="/usr/lib/valkey/modules/fractalsql.so" ;;
    *.apk)
        confirm "Install ${PKG##*/} with apk?" || die "cancelled"
        sudo_run apk add --allow-untrusted "${PKG}"
        INSTALLED_SO="/usr/lib/valkey/modules/fractalsql.so" ;;
    *.zip)
        command -v unzip >/dev/null 2>&1 || die "unzip is required for the macOS archive"
        confirm "Unpack the module into ${LOCAL_DIR}?" || die "cancelled"
        run unzip -qo "${PKG}" -d "${TMP}/unz"
        sudo_run mkdir -p "${LOCAL_DIR}"
        sudo_run cp "${TMP}/unz/fractalsql.dylib" "${LOCAL_DIR}/fractalsql.dylib"
        INSTALLED_SO="${LOCAL_DIR}/fractalsql.dylib"
        if [[ "${OS}" == "Darwin" ]]; then
            sudo_run xattr -dr com.apple.quarantine "${INSTALLED_SO}" || true
            ok "cleared the Gatekeeper quarantine flag (the module is unsigned)"
        fi ;;
    *.so|*.dylib)
        confirm "Copy ${PKG##*/} into ${LOCAL_DIR}?" || die "cancelled"
        sudo_run mkdir -p "${LOCAL_DIR}"
        sudo_run cp "${PKG}" "${LOCAL_DIR}/${PKG##*/}"
        INSTALLED_SO="${LOCAL_DIR}/${PKG##*/}" ;;
    *) die "do not know how to install '${PKG}' (expected .deb, .rpm, .apk, .zip, .so or .dylib)" ;;
esac
ok "module: ${INSTALLED_SO}"

# --- the snippet ------------------------------------------------------------
LINE="loadmodule ${INSTALLED_SO}"
[[ -n "${THREADS}" ]] && LINE+=" threads ${THREADS}"
[[ -n "${DSOCK}"   ]] && LINE+=" daemon-socket ${DSOCK} daemon-key-file ${DKEY}"
log "Writing ${SNIPPET}"
if [[ "${DRY_RUN}" -eq 1 ]]; then
    printf "  [dry-run] %s  <-  %s\n" "${SNIPPET}" "${LINE}"
else
    sudo_run mkdir -p "${SNIPPET_DIR}"
    printf '%s\n' "# FractalSQL for Valkey: written by easy_install (re-run it to change the options)" "${LINE}" > "${TMP}/snippet"
    sudo_run cp "${TMP}/snippet" "${SNIPPET}"
    ok "snippet written"
fi

if [[ -n "${CONF}" ]]; then
    if [[ -f "${CONF}" ]] && grep -qF "include ${SNIPPET}" "${CONF}"; then
        ok "${CONF} already includes the snippet"
    elif confirm "Add 'include ${SNIPPET}' to ${CONF}?"; then
        if [[ "${DRY_RUN}" -eq 1 ]]; then
            printf "  [dry-run] append include to %s\n" "${CONF}"
        else
            printf '%s\n%s\n' "${MARK}" "include ${SNIPPET}" > "${TMP}/inc"
            sudo_run sh -c "cat '${TMP}/inc' >> '${CONF}'"
            ok "added the include to ${CONF}"
        fi
    fi
fi

cat <<EOF

Done. To use it:

  - Add this line to your valkey.conf and restart Valkey:
        include ${SNIPPET}
    (pass --conf <valkey.conf> and this script adds it for you), or
  - Load it into a running server (not persistent):
        valkey-cli MODULE LOAD ${INSTALLED_SO}

Then check it:

  valkey-cli FRACTAL.VERSION
  valkey-cli FRACTAL.EDITION

Reasoning, embeddings and the enterprise ledger need the fractalsqld daemon (the fractalsqld-valkey package); point the module at it with
--daemon-socket and --daemon-key-file (see docs/reasoning-setup.md). Search, vector math, analytics and
most agents need nothing else.
EOF
