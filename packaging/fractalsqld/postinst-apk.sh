#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# Post-install hook for the Alpine .apk (busybox tools). The counterpart of postinst.sh: creates the fractalsql
# service account, the two private copies of the shared key and a starter fractalsqld.conf, then enables the
# OpenRC service. Valkey runs as the valkey user and reaches the daemon's socket through the fractalsql group; the
# module refuses a key that group or others can read, so the secret is kept as two private copies of one value.
set -e

getent group fractalsql >/dev/null 2>&1 || addgroup -S fractalsql
getent passwd fractalsql >/dev/null 2>&1 || \
    adduser -S -D -H -G fractalsql -s /sbin/nologin -g "FractalSQL daemon" fractalsql

if getent passwd valkey >/dev/null 2>&1; then
    addgroup valkey fractalsql >/dev/null 2>&1 || true
fi

install -d -m 0750 -o fractalsql -g fractalsql /etc/fractalsql

KEY=/etc/fractalsql/hmac.key
if [ ! -f "${KEY}" ]; then
    oldumask=$(umask)
    umask 077
    openssl rand -hex 32 > "${KEY}"
    umask "${oldumask}"
fi
chown fractalsql:fractalsql "${KEY}"
chmod 0600 "${KEY}"

MODKEY=/etc/fractalsql/valkey.key
if getent passwd valkey >/dev/null 2>&1 && [ ! -f "${MODKEY}" ]; then
    oldumask=$(umask)
    umask 077
    cp "${KEY}" "${MODKEY}"
    umask "${oldumask}"
    chown valkey:fractalsql "${MODKEY}"
    chmod 0600 "${MODKEY}"
fi

VALKEY_UID="$(id -u valkey 2>/dev/null || echo 0)"
PLUGIN=/usr/lib/fractalsql/fractalsql-reasoning-http.so

if [ ! -f /etc/fractalsql/fractalsqld.conf ]; then
    cat > /etc/fractalsql/fractalsqld.conf <<CONF
socket_path = /run/fractalsql/fractalsqld.sock
hmac_key_file = /etc/fractalsql/hmac.key
allowed_uids = ${VALKEY_UID}

# Reasoning and embeddings: uncomment and fill in to enable FRACTAL.REASON and
# FRACTAL.EMBED. The plugin loads only with its .sig beside it, which this
# package installs next to the plugin.
# reasoning_plugin = ${PLUGIN}
# reasoning_url = https://your-model-host/v1/chat/completions
# reasoning_model = your-model
# embed_url = https://your-model-host/v1/embeddings
# embed_model = your-embedding-model
CONF
    chown fractalsql:fractalsql /etc/fractalsql/fractalsqld.conf
    chmod 0640 /etc/fractalsql/fractalsqld.conf
fi

rc-update add fractalsqld default >/dev/null 2>&1 || true
rc-service fractalsqld restart >/dev/null 2>&1 || true

cat <<EOF2

fractalsqld is installed, added to the default runlevel and (re)started.
valkey was added to the fractalsql group so the server can reach the daemon's socket. A valkey that is
already running keeps its old group list until it restarts:

    rc-service valkey restart

Connect the module to the daemon by loading it with the daemon options (valkey.key is the valkey user's
private copy of the same key; if valkey was installed after this package, copy hmac.key to
/etc/fractalsql/valkey.key owned by valkey, mode 0600):

    loadmodule /usr/lib/valkey/modules/fractalsql.so \\
        daemon-socket /run/fractalsql/fractalsqld.sock \\
        daemon-key-file /etc/fractalsql/valkey.key

Configure a model provider in /etc/fractalsql/fractalsqld.conf, then restart the service.

EOF2
