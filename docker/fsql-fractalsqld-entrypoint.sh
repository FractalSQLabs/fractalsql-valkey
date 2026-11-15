#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# Demo entrypoint for fractalsqld. Writes the daemon config and the shared HMAC key from
# FSQL_HMAC_KEY into /run/fractalsql, then runs the daemon as the valkey user, so the peer-uid
# check admits the valkey server. The container runs as valkey (user: 999:999 in compose), so
# no capabilities are needed.
set -eu
: "${FSQL_HMAC_KEY:?set FSQL_HMAC_KEY (hex, e.g. openssl rand -hex 32); the valkey service needs the same volume}"
RUN_DIR=/run/fractalsql
VALKEY_UID=$(id -u)
umask 077
mkdir -p "$RUN_DIR"
rm -f "$RUN_DIR/fractalsqld.sock"
printf '%s\n' "$FSQL_HMAC_KEY" > "$RUN_DIR/hmac.key"
cat > "$RUN_DIR/fractalsqld.conf" <<CONF
socket_path = $RUN_DIR/fractalsqld.sock
hmac_key_file = $RUN_DIR/hmac.key
allowed_uids = $VALKEY_UID
max_connections = 64
idle_timeout_secs = 300
CONF
exec /usr/local/bin/fractalsqld -c "$RUN_DIR/fractalsqld.conf"
