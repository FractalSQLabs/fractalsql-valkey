<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# FractalSQL Daemon

`fractalsqld` is the separate process that runs everything that can block on the
network, load third-party code, or hold licensed code: reasoning, embeddings, and the
enterprise ledger. The Valkey module speaks to it over a Unix socket using the FSQ v2
wire protocol (authenticated with HMAC-SHA256, spec in `src/fsq/`).

**Why a second process.** The module runs inside your datastore. A model provider that
stalls, a plugin that segfaults, or a library that leaks would otherwise take every key
and every client with it. In a separate process, the worst case is that reasoning
commands return an error until the daemon is back; search, discovery, analytics and
vector math never depend on it.

## What a daemon failure does (and doesn't) do

| Situation | Effect |
| --- | --- |
| daemon not configured | `FRACTAL.REASON`/`EMBED`/`LEDGER.*`/`AUDIT.*` answer "not configured"; everything else works |
| daemon down | those commands answer "not reachable"; everything else works; they recover when it is back, with no Valkey restart |
| daemon crashes mid-call | that call returns an error; Valkey is unaffected |
| wrong key | replies fail authentication and are refused; the key is never sent in a frame |


## Running it

```
fractalsqld [-c config]
```

The daemon is built from the `service/` tree (`make -C service daemon`) and ships in the `fractalsqld-valkey` package.

### Config file

One `key = value` per line. `socket_path` and `hmac_key_file` are required.

| Key | Notes |
| --- | --- |
| `socket_path` | a Unix socket path (under about 107 characters) |
| `hmac_key_file` | a file holding a hex key shared with the module: `openssl rand -hex 32 > key; chmod 600 key` |
| `allowed_uids` | comma-separated uids allowed to connect (default: the daemon's own); set it to the uid Valkey runs as |
| `max_connections`, `idle_timeout_secs`, `log_file` | defaults 64, 300, stderr |
| `reasoning_plugin`, `reasoning_url`, `reasoning_token`, `reasoning_model`, `reasoning_allow_plaintext` | the reasoning provider |
| `embed_url`, `embed_model` | the embeddings provider |
| `think`, `think_provider`, `think_native_url`, `think_num_ctx` | reasoning-effort controls |
| `enterprise_lib`, `enterprise_ledger_path`, `enterprise_ledger_key`, `enterprise_require_signature` | the [enterprise tier](enterprise.md) |

The config file may hold secrets (`reasoning_token`, `enterprise_ledger_key`), so the
daemon refuses to start, or to reload, if it is readable by group or others.

### Connecting Valkey to it

```
loadmodule /usr/lib/valkey/modules/fractalsql.so \
    daemon-socket /run/fractalsql/fractalsqld.sock \
    daemon-key-file /etc/fractalsql/hmac.key \
    daemon-timeout-s 60
```

The two daemon options come as a pair. The module refuses to load if the key file is
readable by anyone but its owner, is not hexadecimal, or the socket path is relative.
They are load-time options on purpose: nothing about the daemon connection can be set
or read back through a command, so it never appears in `MONITOR` or the slowlog.

### Provider keys

The remaining keys configure the reasoning, embedding and enterprise tiers. The config file is the **preferred** source for all of them; a key absent from the
file falls back to the daemon's process environment, captured once at start. All of them reload live with `fsqlctl reload`, and the new value reaches that
tier on its **next** call.

| Key | Default | Notes |
| --- | --- | --- |
| `reasoning_plugin` | none | Path to the reasoning plugin (`.so`). Absolute, with no `..` segments. |
| `reasoning_url` | none | HTTP chat-completions endpoint. |
| `reasoning_token` | none | **Secret.** Bearer token for the reasoning endpoint. |
| `reasoning_model` | none | Model name sent to the reasoning endpoint. |
| `reasoning_allow_plaintext` | off | Permit a plain-HTTP (non-TLS) endpoint. Any non-empty value except `0` turns it on. |
| `embed_url` | none | Embeddings endpoint. |
| `embed_model` | none | Model name sent to the embeddings endpoint. |
| `think` | `none` | Reasoning effort for hybrid-thinking models (`none`, `low`, `medium`, `high`). |
| `think_provider` | `openai` | The request shape `think` uses (`openai`, `ollama`, `anthropic`, `vllm`). |
| `think_native_url` | none | The native endpoint for the `ollama` and `anthropic` shapes. |
| `think_num_ctx` | none | Ollama context-window cap, an integer from 1 to 10000000. |
| `enterprise_lib` | none | Path to the FractalSQL Enterprise library. Absolute, with no `..` segments. |
| `enterprise_ledger_path` | none | Ledger file path. It is opened per operation, so it can be swapped live. |
| `enterprise_ledger_key` | none | **Secret.** HMAC-SHA256 ledger key; unset means structural-only validation. |
| `enterprise_require_signature` | on | Refuse to load an enterprise library that has no detached signature file; set `off` to load an unsigned development build. |

The full provider guide, including what only the environment can set, is in [reasoning-setup.md](reasoning-setup.md). Because `reasoning_token` and
`enterprise_ledger_key` may sit in the file as plaintext, the config file is gated like the HMAC key file: a file readable by other users refuses
startup and reload (`chmod 600` it).

### Path-shaped keys are validated at load and reload

`reasoning_plugin`, `enterprise_lib` and `enterprise_ledger_path` must be absolute and contain no `..` segment. A relative or traversing path is refused with
the key named in the daemon log, so a reload can never be pointed at an unexpected file by a relative path.

### Reloading

`fsqlctl reload` re-reads the file. The swap is all-or-nothing: an unparseable file, or one that changes `socket_path` or `hmac_key_file` (clients read both
once), is refused and the running configuration stays in effect. A reload never loads a plugin or library itself, so a bad `reasoning_plugin` or
`enterprise_lib` path surfaces at that tier's next call, with the path in the daemon log. Removing a key reverts that setting to the environment fallback.
An unknown key is a warning naming the key; an out-of-range or empty value is refused by name. An `enterprise_lib` change is refused while an enterprise
library is already loaded: restart the daemon to change it.

## Installing the package

The `fractalsqld-valkey` `.deb`, `.rpm` and Alpine `.apk` install the daemon as a service next to the module package. It carries the daemon, the reasoning plugin with its detached signature, the MCP server, a systemd unit and a starter config, and depends on `libcurl` and OpenSSL, which the module package does not.

```
sudo apt install ./fractalsql-valkey-amd64.deb ./fractalsqld-valkey-amd64.deb     # Debian / Ubuntu
sudo dnf install ./fractalsql-valkey-amd64.rpm ./fractalsqld-valkey-amd64.rpm     # RHEL family
sudo apk add --allow-untrusted ./fractalsql-valkey-musl-amd64.apk ./fractalsqld-valkey-musl-amd64.apk   # Alpine
```

Installing it creates the `fractalsql` service account, a shared key, and `/etc/fractalsql/fractalsqld.conf`, then enables and starts `fractalsqld.service` (an OpenRC service, `rc-service fractalsqld`, on Alpine). The socket is `/run/fractalsql/fractalsqld.sock`. The daemon keeps its key in `/etc/fractalsql/hmac.key`; the module refuses a key that group or others can read, so the `valkey` user gets its own private copy of the same value in `/etc/fractalsql/valkey.key`. Load the module against the daemon with:

```
loadmodule /usr/lib/valkey/modules/fractalsql.so \
    daemon-socket /run/fractalsql/fractalsqld.sock \
    daemon-key-file /etc/fractalsql/valkey.key
```

The plugin is installed with its `.sig` beside it (`/usr/lib/fractalsql/` on Debian and Alpine, `/usr/lib64/fractalsql/` on RHEL) and loads only with that file present. To turn reasoning on, set `reasoning_plugin` and a provider in the config (the starter file has the lines commented) and restart or reload the daemon.

## Running it as a service

On Linux, the package above installs the service. To run the daemon yourself, use your service manager and let it restart on failure. A minimal systemd unit:

```ini
[Unit]
Description=FractalSQL daemon
After=network.target

[Service]
User=valkey
ExecStart=/usr/local/bin/fractalsqld -c /etc/fractalsql/fractalsqld.conf
RuntimeDirectory=fractalsql
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Valkey reconnects to the daemon on the next call: there is no explicit reconnect step and no Valkey restart.

## `fsqlctl`

`fsqlctl` is a small control CLI that speaks the wire protocol directly.

```
fsqlctl [-s socket] [-k keyfile] COMMAND [ARGS...]
```

| Flag | Default | Environment variable |
| --- | --- | --- |
| `-s socket` | `/run/fractalsql/fractalsqld.sock` | `FSQLCTL_SOCKET` |
| `-k keyfile` | `/etc/fractalsql/hmac.key` | `FSQLCTL_KEY` |

### Commands

- **`ping`**: checks that the daemon answers and prints the protocol version.
- **`version`**: prints the daemon's version string.
- **`check`**: checks the key file's permissions and the socket, then pings. It is the one command that reports a missing or unsafe key file instead of failing.
- **`call FUNCTION [ARG...]`**: runs any registered function and prints its result. Arguments are `s:TEXT` (a string; also the default for a bare word), `i:N`
  (an integer), `r:X` (a real) or `NULL`.
- **`cancel HANDLE_ID`**: cooperatively cancels a running call at its next loop boundary; code inside the core library cannot be interrupted this way.
- **`reload`**: re-reads the config file (see [Reloading](#reloading)).

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Failure reported by the daemon or the transport |
| `2` | Usage error |
| `3` | Authentication failure, or the daemon closed the connection |
| `4` | Unknown function name (checked locally, before contacting the daemon) |

## Reliability

### The core guarantee

Search, discovery, analytics, vector math and every agent's **computation** never depend on the daemon. When the daemon is absent, down or slow, the commands
that need it answer with an error and every other command keeps working at full speed.

### What happens when fractalsqld is unreachable

The commands that call it answer "not reachable" (or "not configured" if you never set it up) and recover on their own when the daemon is back. An agent that
only needed the daemon for its narrative still returns its computed result, with `reasoning_step_failed` naming the error.

### What happens when a call runs too long

A call is bounded by `daemon-timeout-s` (default 60, 1 to 3600) and by the provider's own limits ([reasoning-setup.md](reasoning-setup.md#-hardware--performance-local-reasoning)).
On timeout the call returns an error; it is not retried, because a model call is expensive and may not be repeatable.

## Retries and idempotency

Each function has a retry class. **Pure** calls retry up to three times across a dropped
connection. **Stateful** calls (the ledger) resend once with the same idempotency token,
so the daemon replays its stored answer instead of repeating a side effect. **Expensive**
calls (reasoning, embeddings) are never retried automatically.

## Known limits

- Each daemon call holds one of the module's worker threads while it waits. Raise
  `threads` for heavy reasoning concurrency.
- A daemon call is bounded by `daemon-timeout-s` (default 60, 1 to 3600).
- The core's search code is not interruptible once it starts; only the daemon's own
  loops honour a cancel.
