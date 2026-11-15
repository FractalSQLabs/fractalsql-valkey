#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# --after-install hook shared by the .deb and the .rpm. Creates the fractalsql
# system user, a shared HMAC key and a starter fractalsqld.conf when none exist
# yet, and enables fractalsqld.
#
# Valkey runs as its own user (valkey) and the daemon runs as the fractalsql
# user. Valkey reaches the daemon's socket under /run/fractalsql through its
# membership in the fractalsql group. The module refuses a key file that group or
# others can read, so the shared secret is kept as two private copies of the same
# value: one the daemon owns (hmac.key) and one the valkey user owns (valkey.key).
set -e

getent group fractalsql >/dev/null 2>&1 || groupadd --system fractalsql
getent passwd fractalsql >/dev/null 2>&1 || \
    useradd --system --gid fractalsql --no-create-home \
        --shell /usr/sbin/nologin --comment "FractalSQL daemon" fractalsql

if getent passwd valkey >/dev/null 2>&1; then
    usermod -aG fractalsql valkey || true
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

# The module's copy, owned by the valkey user (mode 0600). Created only once, from the
# daemon's key, and only when the valkey user exists.
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

# Where this package put the reasoning plugin (Debian and RHEL differ).
PLUGIN=/usr/lib/fractalsql/fractalsql-reasoning-http.so
[ -f "${PLUGIN}" ] || PLUGIN=/usr/lib64/fractalsql/fractalsql-reasoning-http.so

# The daemon's own config: the socket, the key and the peer uid allowed to
# connect. Reasoning is off until a provider is configured below.
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

systemctl daemon-reload >/dev/null 2>&1 || true
systemctl enable fractalsqld.service >/dev/null 2>&1 || true
systemctl restart fractalsqld.service >/dev/null 2>&1 || true

cat <<EOF2

fractalsqld is installed, enabled and (re)started.
valkey was added to the fractalsql group so the server can reach the daemon's
socket and read the shared key. A valkey-server that is already running keeps its
old group list until it restarts; restart it once:

    sudo systemctl restart valkey-server     # Debian / Ubuntu
    sudo systemctl restart valkey            # RHEL / Fedora

Connect the module to the daemon by loading it with the daemon options (valkey.key is the
valkey user's private copy of the same key; if valkey was installed after this package, copy
hmac.key to /etc/fractalsql/valkey.key owned by valkey, mode 0600):

    loadmodule <path to fractalsql.so> \\
        daemon-socket /run/fractalsql/fractalsqld.sock \\
        daemon-key-file /etc/fractalsql/valkey.key

Configure a model provider in /etc/fractalsql/fractalsqld.conf, then reload.

EOF2
