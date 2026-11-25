<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Reasoning Setup Guide

`FRACTAL.REASON`, `FRACTAL.EMBED` and the agents that write a narrative or embed text call a language model. This guide
takes you from "not configured" to a working provider: a local Ollama or vLLM server, any OpenAI-compatible service, or a
hosted one (AWS Bedrock, Azure OpenAI, Google Vertex AI). Everything else in the module works without any of it.

## 🧠 The Cognition Model

Three pieces, in three places:

| Piece | Where it runs | What it does |
| --- | --- | --- |
| The `fractalsql` module | inside `valkey-server` | registers the commands and forwards the model calls |
| `fractalsqld` | its own process, on the same host | holds the provider settings and credentials, and makes the HTTP calls |
| The reasoning plugin (`fractalsql-reasoning-http.so`) | loaded by `fractalsqld` | speaks the OpenAI chat-completions and embeddings shapes over HTTPS |

The split is the point: a provider that stalls, a plugin that crashes, or a model SDK that leaks can never take your datastore down, and your
provider credentials never pass through a Valkey command, `MONITOR` or the slowlog. See the [FractalSQL Daemon](fractalsql-daemon.md).

## 🛠️ Prerequisites

### 1. The Reasoning Plugin

The plugin is `fractalsql-reasoning-http.so`, built per platform (see `include/<platform>/` in the source tree). It needs `libcurl` 7.75 or newer on
the host. Use an absolute path with no `..` segments when you point the daemon at it.

### 2. Technical Requirements

- `fractalsqld` running, with a socket path and an HMAC key file, and the module loaded with `daemon-socket` and `daemon-key-file`
  ([Getting Started](getting-started.md#3-turn-on-reasoning)).
- A model endpoint you can reach from the daemon's host: a chat-completions URL for reasoning, and an embeddings URL if you use `FRACTAL.EMBED`.
- `https://` unless you explicitly allow plain HTTP for a LAN or development endpoint.

## 🚀 Setup Sequence

## Step 1: Point fractalsqld at the plugin

```ini
# /etc/fractalsql/fractalsqld.conf   (chmod 600: it may hold a token)
socket_path     = /run/fractalsql/fractalsqld.sock
hmac_key_file   = /etc/fractalsql/hmac.key
allowed_uids    = 999

reasoning_plugin = /usr/lib/fractalsql/fractalsql-reasoning-http.so
```

Start the daemon, then check the whole chain from the Valkey side:

```bash
fsqlctl -s /run/fractalsql/fractalsqld.sock -k /etc/fractalsql/hmac.key check   # key, socket and ping
valkey-cli FRACTAL.REASON "ping"                                               # end to end
```

A daemon whose config file can be read by other users refuses to start (it may hold a token): `chmod 600` it.

## Step 2: Universal LLM Connectivity

Add the endpoint to the same file. The settings below are the ones most installs need; the others are in
[Advanced Configuration](#-advanced-configuration).

## Ollama (Local or Private Network)

```ini
reasoning_url   = http://127.0.0.1:11434/v1/chat/completions
reasoning_model = gpt-oss:20b
embed_url       = http://127.0.0.1:11434/v1/embeddings
embed_model     = nomic-embed-text
reasoning_allow_plaintext = on        # a loopback or LAN endpoint without TLS
```

Pull the models once on the Ollama host (`ollama pull gpt-oss:20b`, `ollama pull nomic-embed-text`); a large chat model is a download of many
gigabytes. Embeddings need an **embedding** model: a chat model is not one.

## OpenAI-Compatible (OpenAI, Together AI, Fireworks, vLLM)

```ini
reasoning_url   = https://api.openai.com/v1/chat/completions
reasoning_token = sk-...
reasoning_model = gpt-4o-mini
embed_url       = https://api.openai.com/v1/embeddings
embed_model     = text-embedding-3-small
```

Point `reasoning_url` at any service that speaks the same shape. Keep the token out of the file when you can: set `FSQL_REASONING_HTTP_TOKEN_FILE` in the
daemon's environment to a file holding the credential. It is re-read on every call, so rotating it needs no restart.

## AWS Bedrock

Bedrock's OpenAI-compatible surface is signed with AWS credentials instead of a bearer token. Set these in the daemon's environment:

```bash
FSQL_REASONING_HTTP_AUTH_TYPE=aws-sigv4
FSQL_REASONING_HTTP_AWS_REGION=us-east-1
AWS_ACCESS_KEY_ID=...  AWS_SECRET_ACCESS_KEY=...  AWS_SESSION_TOKEN=...      # the token only for temporary credentials
```

with `reasoning_url` and `reasoning_model` set as above for your Bedrock endpoint and model.

Credentials come from the standard AWS environment of the daemon: `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`, plus `AWS_SESSION_TOKEN` when you use temporary (IAM role) credentials. With `aws-sigv4` both the access key and the secret key are required. If either is missing, the plugin fails to initialize and prints `AUTH_TYPE=aws-sigv4 requires AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY` to stderr, so a missing credential shows up when the plugin loads rather than as a generic HTTP 403 from Bedrock. A credential or region that contains a control character (for example a stray newline pasted with a secret) is refused instead of being sent.

## Azure OpenAI

```bash
FSQL_REASONING_HTTP_AUTH_TYPE=api-key         # sent as the api-key header
```

with `reasoning_url` set to your deployment's chat-completions URL and `reasoning_token` (or the token file) holding the key.

## Google Vertex AI

Vertex AI's OpenAI-compatible surface takes a bearer token, the default auth type. Use `reasoning_url` for the compatible endpoint and refresh the
short-lived access token through `FSQL_REASONING_HTTP_TOKEN_FILE`, which is re-read on every call.

## ⚖️ Hardware & Performance (Local Reasoning)

A local model is as fast as its hardware. The plugin's limits keep a slow model from ever holding a worker for long:

| Setting | Default | Meaning |
| --- | --- | --- |
| `FSQL_REASONING_HTTP_TIMEOUT_MS` | 60000 | total time for one call, connect and transfer included |
| `FSQL_REASONING_HTTP_CONNECT_MS` | 10000 | TCP and TLS handshake |
| `FSQL_REASONING_HTTP_LOW_SPEED_SECS` | 20 | abort a transfer that stays under the minimum speed this long |
| `FSQL_REASONING_HTTP_MAX_RESPONSE_BYTES` | 8 MiB | largest reply accepted |
| `FSQL_REASONING_HTTP_MAX_PROMPT_BYTES` | 8 MiB | largest request sent |

The module has its own per-call ceiling too: `daemon-timeout-s` (default 60, 1 to 3600) at load time.

### Handling Constrained Hardware

A thinking model on a small GPU can spend most of its time and memory on its internal trace. Throttle it with [Reasoning Effort](#reasoning-effort)
and, for Ollama, cap the context window with `think_num_ctx`. Raise the timeout rather than retrying: model calls are **never retried automatically**.

## 🛠️ Advanced Configuration

### Where each setting lives: `fractalsqld.conf` vs. environment

The config file is the preferred source and reloads live (`fsqlctl reload`; the new value reaches the next call). A key absent from the file falls back
to the daemon's process environment, captured once at start.

| `fractalsqld.conf` key | Environment fallback | Notes |
| --- | --- | --- |
| `reasoning_plugin` | `FRACTALSQL_REASONING_PLUGIN` | absolute path, no `..` |
| `reasoning_url` | `FRACTALSQL_HTTP_URL` | chat-completions endpoint |
| `reasoning_token` | `FSQL_REASONING_HTTP_TOKEN` | a secret; prefer the token file |
| `reasoning_model` | `FRACTALSQL_HTTP_MODEL` | chat model |
| `reasoning_allow_plaintext` | `FRACTALSQL_HTTP_ALLOW_PLAINTEXT` | any non-empty value except `0` allows plain HTTP |
| `embed_url` | `FRACTALSQL_HTTP_EMBED_URL` | embeddings endpoint |
| `embed_model` | `FRACTALSQL_HTTP_EMBED_MODEL` | embedding model |
| `think` | `FSQL_REASONING_HTTP_THINK` | see [Reasoning Effort](#reasoning-effort) |
| `think_provider` | `FSQL_REASONING_HTTP_THINK_PROVIDER` | `openai`, `ollama`, `anthropic` or `vllm` |
| `think_native_url` | `FSQL_REASONING_HTTP_NATIVE_URL` | the native endpoint for `ollama` / `anthropic` |
| `think_num_ctx` | `FSQL_REASONING_HTTP_NUM_CTX` | Ollama only, 1 to 10000000 |

A bad value in the file (out of range, or empty) refuses startup and reload, naming the key in the daemon log. The plugin's own knobs have no config key and stay in
the daemon's environment: `FSQL_REASONING_HTTP_RESPONSE_MODE`, `_AUTH_TYPE`, `_AWS_REGION`, `_TIMEOUT_MS`, `_CONNECT_MS`, `_LOW_SPEED_SECS`, `_CA_BUNDLE`,
`_SYSTEM_PROMPT`, `_SYSTEM_TAG`, `_TOKEN_FILE`.

### The plugin's signature

The plugin is a shared object the daemon maps into its own process, so the daemon checks where it came from before it loads it. The plugin must carry a
detached Ed25519 signature from the FractalSQLabs reasoning-plugin signing key — a `<plugin>.sig` file next to the plugin of exactly 64 raw bytes. The release
packages ship the plugin and its `.sig` together; keep the two together. The file is checked again after the load, so a plugin swapped underneath the load
is caught rather than quietly used. A plugin without a valid signature is refused, and the call that wanted it says so. An operator testing their own build can
set `FSQL_REASONING_REQUIRE_SIGNATURE=0` to load a plugin that has no `.sig`; one warning goes to stderr when that happens, and a `.sig` that is present but wrong is still refused.

### Response Modes

The daemon loads the reasoning plugin lazily, once for each kind of call, and fixes the plugin's mode for that call kind so one cannot
disturb another:

| Call kind | Used by | Response mode |
| --- | --- | --- |
| Reasoning (chat) | `FRACTAL.REASON` and the narrative step of every agent | `text` by default; `code` or `json` if you set it (below) |
| Embedding | `FRACTAL.EMBED`, `SEARCH_AGENT`, `RAG_AGENT`, `PLAN_EXPLORE` | always the embeddings mode; the response mode does not apply |
| Query generation | `FRACTAL.AGENT.DATA_ANALYST` | always `code` (it needs exactly one fenced block), whatever you set for reasoning |

`FSQL_REASONING_HTTP_RESPONSE_MODE`, set in the daemon's environment before it starts, controls how the plugin post-processes the model's reply
for the reasoning call:

- **`text`** (default): the reply as-is, fence markers and all.
- **`code`**: the reply must be a single fenced code block; its interior is returned and the language tag and any prose are dropped.
  Two or more blocks fail the call rather than guessing which is right.
- **`json`**: as `code`, plus a structural validity check on the interior, so a reply that is not valid JSON fails the call.

It is read when the daemon starts, so a change needs a daemon restart (the module reconnects on its own).
Embeddings and `DATA_ANALYST` are unaffected by it. The reasoning call is shared with the agents' narratives, so leave it at `text` if you use
them: in `code` or `json` mode a narrative that is not a single fenced block fails its reasoning step, and the agent returns what it computed
with `reasoning_step_failed`.

### Reasoning Effort

Hybrid-thinking models emit a long internal trace before a short answer. `think` (`none` by default, or `low`, `medium`, `high`) throttles it, and
`think_provider` picks the request shape: `openai` (a `reasoning_effort` field), `vllm`, `anthropic` (a token budget) or `ollama` (its native endpoint, set with
`think_native_url`, because its OpenAI-compatible endpoint ignores thinking control). Thinking traces are discarded: only the answer is returned. Effort
applies to `FRACTAL.REASON` only; embeddings ignore it.

### Target-System Hints

`FSQL_REASONING_HTTP_SYSTEM_TAG` adds one sentence to the system message ("Target system: ... Use syntax appropriate for this target"). It is an opaque string,
useful when you ask the model to write commands for a specific system.

## 🧭 Setting names on every host

The same setting is spelled by the convention of the host it lives in: a SQL setting name where the extension runs inside the database, a `fractalsqld.conf` key where a daemon holds the model client. This table is the same in every host's guide; **—** means the host does not have the setting, and the note says why.

| Setting | SQLite, DuckDB (`fractal_config_set`) | PostgreSQL (`SET` / `ALTER SYSTEM`) | MariaDB (`fractal_config_set`, `fractalsqld.conf`) | Valkey (`fractalsqld.conf`, module argument) | Druid (`fractalsqld.conf`, property) | Plugin environment variable |
| --- | --- | --- | --- | --- | --- | --- |
| Reasoning plugin path | `reasoning_plugin` | `fractalsql.reasoning_plugin` | `reasoning_plugin` | `reasoning_plugin` | `reasoning_plugin` | `FRACTALSQL_REASONING_PLUGIN` |
| Chat endpoint URL | `http_url` | `fractalsql.http_url` | `reasoning_url` | `reasoning_url` | `reasoning_url` | `FSQL_REASONING_HTTP_URL` |
| Bearer token (a secret, never read back) | `http_token` | `fractalsql.http_token` | `reasoning_token` | `reasoning_token` | `reasoning_token` | `FSQL_REASONING_HTTP_TOKEN` |
| Chat model | `http_model` | `fractalsql.http_model` | `reasoning_model` | `reasoning_model` | `reasoning_model` | `FSQL_REASONING_HTTP_MODEL` |
| Allow plain HTTP | `http_allow_plaintext` | `fractalsql.http_allow_plaintext` | `reasoning_allow_plaintext` | `reasoning_allow_plaintext` | `reasoning_allow_plaintext` | `FSQL_REASONING_HTTP_ALLOW_PLAINTEXT` |
| Embedding endpoint URL | `http_embed_url` | `fractalsql.http_embed_url` | `embed_url` | `embed_url` | `embed_url` | set per call by the host |
| Embedding model | `http_embed_model` | `fractalsql.http_embed_model` | `embed_model` | `embed_model` | `embed_model` | set per call by the host |
| Reasoning effort | `http_think` | `fractalsql.http_think` | `think` | `think` | `think` | `FSQL_REASONING_HTTP_THINK` |
| Effort provider | `http_think_provider` | `fractalsql.http_think_provider` | `think_provider` | `think_provider` | `think_provider` | `FSQL_REASONING_HTTP_THINK_PROVIDER` |
| Native endpoint URL | `http_native_url` | `fractalsql.http_native_url` | `think_native_url` | `think_native_url` | `think_native_url` | `FSQL_REASONING_HTTP_NATIVE_URL` |
| Context window | `http_num_ctx` | `fractalsql.http_num_ctx` | `think_num_ctx` | `think_num_ctx` | `think_num_ctx` | `FSQL_REASONING_HTTP_NUM_CTX` |
| Response mode | `http_response_mode` (SQLite, DuckDB) | `fractalsql.http_response_mode` | environment of `fractalsqld` | environment of `fractalsqld` | environment of `fractalsqld` | `FSQL_REASONING_HTTP_RESPONSE_MODE` |
| Text-to-SQL attempts | `text_to_sql_max_attempts` (SQLite); DuckDB makes 3 | `fractalsql.text_to_sql_max_attempts` | `t2s_max_attempts` | — no text-to-SQL | `t2s_max_attempts` | — |
| Text-to-SQL statements allowed | `text_to_sql_allowed_statements` | `fractalsql.text_to_sql_allowed_statements` | `t2s_allowed_statements` | — no text-to-SQL | `t2s_allowed_statements` | `FRACTALSQL_TEXT_TO_SQL_ALLOWED_STATEMENTS` (DuckDB) |
| Text-to-SQL review pass | `text_to_sql_use_review` | `fractalsql.text_to_sql_use_review` | `t2s_use_review` | — no text-to-SQL | `t2s_use_review` | `FRACTALSQL_TEXT_TO_SQL_USE_REVIEW` (DuckDB) |
| Enterprise library | `enterprise_lib` | `fractalsql.enterprise_lib` | `enterprise_lib` | `enterprise_lib` | `enterprise_lib` | `FRACTALSQL_ENTERPRISE_LIB` |
| Ledger file | — the ledger is a table (SQLite) or held in memory (DuckDB) | — the ledger is a table | `enterprise_ledger_path` | `enterprise_ledger_path` | `enterprise_ledger_path` | `FRACTALSQL_ENTERPRISE_LEDGER_PATH` |
| Ledger authentication key (a secret) | `enterprise_ledger_key` (SQLite); — DuckDB holds the ledger in memory | `fractalsql.enterprise_ledger_key` | `enterprise_ledger_key` | `enterprise_ledger_key` | `enterprise_ledger_key` | `FRACTALSQL_ENTERPRISE_LEDGER_KEY` |
| Require the library's signature | `enterprise_require_signature` (DuckDB: can only be turned on) | `fractalsql.enterprise_require_signature` | `enterprise_require_signature` | `enterprise_require_signature` | `enterprise_require_signature` | `FRACTALSQL_ENTERPRISE_REQUIRE_SIGNATURE` |
| Ledger retention (`fade` or `exact`) | `ledger_retention` | `fractalsql.ledger_retention` | environment of `fractalsqld` | environment of `fractalsqld` | environment of `fractalsqld` | `FRACTALSQL_LEDGER_RETENTION` |
| Ledger size cap | `ledger_max_rows` (SQLite, rows; 0 = unbounded) or `ledger_max_bytes` (DuckDB, bytes; 0 = unbounded) | `fractalsql.ledger_max_rows` (rows) | environment of `fractalsqld`: `FRACTALSQL_LEDGER_MAX_BYTES` (bytes, default 1 GiB; 0 = unbounded) | environment of `fractalsqld`: `FRACTALSQL_LEDGER_MAX_BYTES` | environment of `fractalsqld`: `FRACTALSQL_LEDGER_MAX_BYTES` | `FRACTALSQL_LEDGER_MAX_BYTES` |
| Partial agent results | `agent_partial_reasoning_fail` (SQLite and DuckDB) | `fractalsql.agent_partial_reasoning_fail` | `fractal_agent_partial_fail(session, 'on')` | `agent-partial-reasoning-fail` | the request argument `partial` | — |
| Agents search through an index (`off`, `on`, `auto`) | `agent_use_index` | `fractalsql.agent_use_index` | `agent_use_index` | `agent-use-index` (`no`, `yes`, `auto`) | `druid.fractalsql.agent_use_index` | `FRACTALSQL_AGENT_USE_INDEX` (DuckDB) |
| Smallest table `auto` uses an index for (not used on MariaDB and Druid, where `auto` scans exactly until they calibrate an index) | `agent_index_min_rows` | `fractalsql.agent_index_min_rows` | `agent_index_min_rows` | `agent-index-min-rows` | `druid.fractalsql.agent_index_min_rows` | `FRACTALSQL_AGENT_INDEX_MIN_ROWS` (DuckDB) |
| Longest text cell an agent passes to the model | `agent_context_row_bytes` | `fractalsql.agent_context_row_bytes` | `agent_context_row_bytes` | — the total is capped at 1 MiB | `druid.fractalsql.agent_context_row_bytes` | `FRACTALSQL_AGENT_CONTEXT_ROW_BYTES` (DuckDB) |

Values that name an on/off choice accept `on`, `off`, `1`, `0`, `true`, `false`, `yes` and `no` everywhere they are a setting. `fractal_config_keys()` (where the host has it) lists the keys the running build knows.

---

## 🔒 Security & Governance

### The Sovereign Guardrail: Dedicated Accounts

Calls out to a model are in the `@dangerous` ACL category, so a user granted only `+@read` cannot make them. Give an application account exactly the commands
it needs:

```
ACL SETUSER app on >secret ~app:* -@all +fractal.search +fractal.agent.anomaly_triage
```

Keep the daemon's socket private: it is created with mode `0660`, only the `allowed_uids` you list may connect, and every request is authenticated with the HMAC key.

### Prompt Injection (OWASP LLM01)

Text a model reads can contain instructions. The plugin sends a default system message telling the model to answer using only the data it was given and to
ignore instructions inside it; set `FSQL_REASONING_HTTP_SYSTEM_PROMPT` to replace it with your own, or to `""` to send none. This is a floor, not a guarantee: treat
a model's output as untrusted, and never run it as a command without validating it.

### Data leaving your network

A reasoning call sends its query and context to the provider you configured, and an embedding call sends its text. Nothing else leaves. For a fully
air-gapped install point both URLs at a local server.

## 📋 Production Checklist

- [ ] `fractalsqld.conf` is mode `600`; the HMAC key file is mode `600`; `allowed_uids` lists only your Valkey user.
- [ ] The endpoint is `https://` (or a deliberate `reasoning_allow_plaintext` on a trusted network).
- [ ] The credential comes from a token file or a secret store, not from a world-readable file.
- [ ] `FSQL_REASONING_HTTP_TIMEOUT_MS` and `daemon-timeout-s` are sized for your model.
- [ ] Application accounts hold only the `+command` grants they need, and none holds `+@dangerous` by accident.
- [ ] `valkey-cli FRACTAL.REASON "ping"` and `FRACTAL.EMBED "ping"` succeed from the Valkey host.
- [ ] You tested a provider outage: the other commands keep working, and the agents return their computed fields with `reasoning_step_failed`.
