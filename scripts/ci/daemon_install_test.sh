#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# Installs the module package and the fractalsqld-valkey package (deb on Debian/Ubuntu, rpm on RHEL
# family, apk on Alpine) in a clean container and checks the whole chain: files, the service account and keys, the
# starter config, a Valkey server that loads the module against the daemon, the reasoning plugin's
# signature requirement, and the MCP server.
#
# Run as root inside the container with the repository mounted at /work:
#   PKGDIR=/work/dist/packages bash /work/scripts/ci/daemon_install_test.sh
set -euo pipefail

PKGDIR="${PKGDIR:-/work/dist/packages}"
ARCH="${ARCH:-amd64}"          # package architecture suffix: amd64 or arm64
SKIP_MCP="${SKIP_MCP:-0}"      # 1 when the packages under test were built without the MCP server
FAILS=0
ok()   { echo "  ok: $*"; }
bad()  { echo "  FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
# expect <description> <haystack> <needle>   /   refuse <description> <haystack> <needle>
expect() { if grep -qF -- "$3" <<< "$2"; then ok "$1"; else bad "$1 (wanted '$3' in: ${2:0:300})"; fi; }
refuse() { if grep -qF -- "$3" <<< "$2"; then bad "$1 (found '$3' in: ${2:0:300})"; else ok "$1"; fi; }

# Run a command as another user: runuser where it exists (Debian, RHEL), busybox su on Alpine.
run_as() {
    local u="$1"; shift
    if command -v runuser >/dev/null 2>&1; then runuser -u "$u" -- "$@"
    else su -s /bin/sh "$u" -c "$(printf '%q ' "$@")"; fi
}

if command -v apt-get >/dev/null 2>&1; then
    FAMILY=deb
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null 2>&1
    apt-get install -qq -y valkey-server valkey-tools openssl python3 procps >/dev/null 2>&1
    MODULE=/usr/lib/valkey/modules/fractalsql.so
    PLUGDIR=/usr/lib/fractalsql
    apt-get install -qq -y "${PKGDIR}/fractalsql-valkey-${ARCH}.deb" "${PKGDIR}/fractalsqld-valkey-${ARCH}.deb" >/dev/null 2>&1
elif command -v apk >/dev/null 2>&1; then
    FAMILY=apk
    apk add --no-cache valkey valkey-cli python3 openssl procps >/dev/null 2>&1
    MODULE=/usr/lib/valkey/modules/fractalsql.so
    PLUGDIR=/usr/lib/fractalsql
    apk add --no-cache --allow-untrusted "${PKGDIR}/fractalsql-valkey-musl-${ARCH}.apk" "${PKGDIR}/fractalsqld-valkey-musl-${ARCH}.apk" >/dev/null 2>&1
else
    FAMILY=rpm
    dnf install -qy epel-release >/dev/null 2>&1
    dnf install -qy valkey openssl python3 procps-ng >/dev/null 2>&1
    MODULE=/usr/lib64/valkey/modules/fractalsql.so
    PLUGDIR=/usr/lib64/fractalsql
    dnf install -qy "${PKGDIR}/fractalsql-valkey-${ARCH}.rpm" "${PKGDIR}/fractalsqld-valkey-${ARCH}.rpm" >/dev/null 2>&1
fi
echo "== ${FAMILY} install"

echo "== files and the signature beside the plugin"
test -x /usr/libexec/fractalsql/fractalsqld && ok "daemon installed" || bad "daemon missing"
test -f "${PLUGDIR}/fractalsql-reasoning-http.so" && ok "plugin installed" || bad "plugin missing"
SIGSZ="$(wc -c < "${PLUGDIR}/fractalsql-reasoning-http.so.sig" 2>/dev/null || echo 0)"
[ "${SIGSZ}" -eq 64 ] && ok "plugin .sig is 64 bytes, beside the plugin" || bad "plugin .sig missing or not 64 bytes (${SIGSZ})"
if [ "${FAMILY}" = apk ]; then
    test -x /etc/init.d/fractalsqld && ok "OpenRC service installed" || bad "OpenRC service missing"
else
    test -f /usr/lib/systemd/system/fractalsqld.service && ok "systemd unit installed" || bad "systemd unit missing"
fi

echo "== service account, keys and starter config"
id fractalsql >/dev/null 2>&1 && ok "fractalsql user exists" || bad "fractalsql user missing"
id -nG valkey | tr ' ' '\n' | grep -qx fractalsql && ok "valkey is in the fractalsql group" || bad "valkey not in the fractalsql group"
[ "$(stat -c '%a %U' /etc/fractalsql/hmac.key)" = "600 fractalsql" ] && ok "daemon key is private to fractalsql" || bad "daemon key mode/owner: $(stat -c '%a %U' /etc/fractalsql/hmac.key)"
[ "$(stat -c '%a %U' /etc/fractalsql/valkey.key)" = "600 valkey" ] && ok "module key is private to valkey" || bad "module key mode/owner: $(stat -c '%a %U' /etc/fractalsql/valkey.key)"
[ "$(cat /etc/fractalsql/hmac.key)" = "$(cat /etc/fractalsql/valkey.key)" ] && ok "both keys hold the same secret" || bad "the two keys differ"
CONF="$(cat /etc/fractalsql/fractalsqld.conf)"
expect "config names the socket" "${CONF}" "socket_path = /run/fractalsql/fractalsqld.sock"
expect "config allows the valkey uid" "${CONF}" "allowed_uids = $(id -u valkey)"

# Valkey refuses to start on an emulated arm64 kernel (it detects a copy-on-write quirk of the emulator); the
# documented switch only silences that check.
VALKEY_EXTRA=""
[ "$(uname -m)" = "aarch64" ] && VALKEY_EXTRA="--ignore-warnings ARM64-COW-BUG"
echo "== start the daemon (no systemd in the container) and Valkey against it"
install -d -m 0750 -o fractalsql -g fractalsql /run/fractalsql /var/lib/fractalsql
start_daemon() {
    pkill fractalsqld 2>/dev/null || true; sleep 0.5; rm -f /run/fractalsql/fractalsqld.sock
    (cd /var/lib/fractalsql && run_as fractalsql /usr/libexec/fractalsql/fractalsqld -c /etc/fractalsql/fractalsqld.conf >/tmp/fsqld.log 2>&1 &)
    for _ in $(seq 1 50); do [ -S /run/fractalsql/fractalsqld.sock ] && break; sleep 0.2; done
    [ -S /run/fractalsql/fractalsqld.sock ] || { bad "daemon socket did not appear"; cat /tmp/fsqld.log >&2; }
}
start_daemon
run_as valkey valkey-server --daemonize yes ${VALKEY_EXTRA} --dir /tmp --logfile /tmp/valkey.log \
    --loadmodule "${MODULE}" daemon-socket /run/fractalsql/fractalsqld.sock daemon-key-file /etc/fractalsql/valkey.key >/dev/null
for _ in $(seq 1 50); do valkey-cli ping >/dev/null 2>&1 && break; sleep 0.3; done
valkey-cli ping >/dev/null 2>&1 && ok "valkey is up with the module and the daemon options" || { bad "valkey did not start"; tail -5 /tmp/valkey.log >&2; }

echo "== reasoning and the signature requirement"
OUT="$(valkey-cli FRACTAL.REASON 'say ok' 2>&1 || true)"
expect "no provider configured: the daemon answers (module and daemon are connected)" "${OUT}" "reasoning provider"
echo "reasoning_plugin = ${PLUGDIR}/fractalsql-reasoning-http.so" >> /etc/fractalsql/fractalsqld.conf
start_daemon
valkey-cli FRACTAL.REASON 'say ok' >/dev/null 2>&1 || true
LOG="$(cat /tmp/fsqld.log)"
refuse "plugin + .sig present: the signature check passes" "${LOG}" "signature"
expect "plugin + .sig present: it loads and stops at its own missing-URL init" "${LOG}" "init returned -2"
mv "${PLUGDIR}/fractalsql-reasoning-http.so.sig" /tmp/plugin.sig.bak
start_daemon
valkey-cli FRACTAL.REASON 'say ok' >/dev/null 2>&1 || true
LOG="$(cat /tmp/fsqld.log)"
expect "plugin without its .sig is refused" "${LOG}" "no signature found for reasoning plugin"
mv /tmp/plugin.sig.bak "${PLUGDIR}/fractalsql-reasoning-http.so.sig"

if [ "${SKIP_MCP}" = "1" ]; then
    echo "== MCP server: skipped (SKIP_MCP=1)"
else
    echo "== MCP server"
    test -x /usr/bin/fractalsql-valkey-mcp && ok "MCP server installed" || bad "MCP server missing"
    fractalsql-valkey-mcp --version >/dev/null 2>&1 && ok "MCP server runs" || bad "MCP server does not run"
    [ "$(fractalsql-valkey-mcp --list-tools | wc -l)" -ge 69 ] && ok "MCP server lists its tools" || bad "MCP tool list too short"
    REPLY="$( (printf '%s\n' \
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"install-test","version":"0"}}}' \
        '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
        '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"fractal_version","arguments":{}}}'; sleep 2) \
        | timeout 20 fractalsql-valkey-mcp 2>/dev/null || true)"
    expect "MCP server answers a tool call against the live server" "${REPLY}" '"isError":false'

fi

if [ "${FAILS}" -ne 0 ]; then echo "FAILED: ${FAILS} check(s)" >&2; exit 1; fi
if [ "${SKIP_MCP}" = "1" ]; then MCPNOTE=""; else MCPNOTE=" and MCP server"; fi
echo "ok: ${FAMILY} install of the module and fractalsqld-valkey, daemon, signed plugin${MCPNOTE}"
