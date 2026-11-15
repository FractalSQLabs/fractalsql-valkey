<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# MCP Plugin

`fractalsql-valkey-mcp` is a Model Context Protocol (MCP) server that gives an AI assistant the FractalSQL commands as **69 typed tools**.
It is a small standalone C++17 program. It is an ordinary client of a Valkey that has the FractalSQL module loaded: it speaks RESP itself,
links no Valkey client library and none of the module, and holds no state of its own. Whatever the assistant is allowed to do is decided by the
Valkey ACL of the user the server connects as.

```
assistant  --MCP (stdio or HTTP/TLS)-->  fractalsql-valkey-mcp  --RESP-->  Valkey + fractalsql.so  --socket-->  fractalsqld (optional)
```

The tools are the module's commands: `fractal_search` is `FRACTAL.SEARCH`, `fractal_agent_regime_triage` is `FRACTAL.AGENT.REGIME_TRIAGE`, and so on.
The server's job is the part an assistant is bad at: it turns JSON numbers into the packed float32 blobs the commands take, validates every argument before a
byte reaches Valkey, and turns the replies back into JSON. Tool names follow the other FractalSQL MCP servers wherever the same capability exists.

## Quick start

```bash
make -C mcp                                   # needs a C++17 compiler and OpenSSL headers; builds mcp/fractalsql-valkey-mcp
VALKEY_HOST=127.0.0.1 VALKEY_PORT=6379 mcp/fractalsql-valkey-mcp --list-tools
```

Register it with an MCP client as a stdio server. For Claude Desktop (`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "fractalsql-valkey": {
      "command": "/path/to/fractalsql-valkey-mcp",
      "env": { "VALKEY_HOST": "127.0.0.1", "VALKEY_PORT": "6379", "VALKEY_USERNAME": "assistant", "VALKEY_PASSWORD_FILE": "/etc/fractalsql/assistant.pw" }
    }
  }
}
```

In the Docker demo (`docker compose up -d`) the binary is already in the Valkey container: `docker compose exec -T valkey fractalsql-valkey-mcp`.

The `fractalsqld-valkey` `.deb`, `.rpm` and Alpine `.apk` packages install it too, as `/usr/bin/fractalsql-valkey-mcp`, with its schema and safety lock files (`.mcp-schema-lock`, `.mcp-safety-annotations-lock`) in `/usr/bin` beside it, and the manifest and this README under `/usr/share/doc/`. The module's own `.deb` and `.rpm` do not carry it, so they keep to glibc and the Valkey server (the Alpine module `.apk` likewise). These packages depend on OpenSSL's runtime library (`libssl3`, or `openssl-libs` on RHEL); the C++ runtime is linked into the binary. The binary is built for the same glibc floor as the module (2.34).

## Connecting to Valkey

| Variable | Default | Meaning |
| --- | --- | --- |
| `VALKEY_HOST` / `VALKEY_PORT` | `127.0.0.1` / `6379` | TCP endpoint |
| `VALKEY_SOCKET` | none | A Unix socket path; wins over host and port |
| `VALKEY_USERNAME` | none | An ACL user. Empty means the `default` user |
| `VALKEY_PASSWORD` / `VALKEY_PASSWORD_FILE` | none | The password; prefer the file (first line, whitespace trimmed) |
| `VALKEY_DB` | `0` | Database number, 0 to 65535 |
| `VALKEY_TLS` | `false` | `true` to speak TLS to Valkey's `tls-port`. TCP only; it cannot be combined with `VALKEY_SOCKET` |
| `VALKEY_TLS_CA_FILE` | system trust store | The CA certificate(s) that must have signed the server's certificate |
| `VALKEY_TLS_CERT_FILE` / `VALKEY_TLS_KEY_FILE` | none | A client certificate and its key (for a server with `tls-auth-clients yes`); set both or neither |
| `VALKEY_TLS_SERVER_NAME` | `VALKEY_HOST` | The name the server's certificate must carry, and the SNI sent |
| `VALKEY_TLS_INSECURE` | `false` | `true` encrypts but does not verify the server. For testing only |

The three timeouts that govern the connection itself — `FSQL_MCP_VALKEY_CONNECT_TIMEOUT_MS` (`10000`, TCP connect), `FSQL_MCP_VALKEY_TIMEOUT_MS`
(`600000`, one reply: a model-backed agent can take minutes, so size it for yours) and `FSQL_MCP_VALKEY_MAX_REPLY_MB` (`64`, the largest reply accepted;
a server claiming more is refused and the connection dropped) — are settings of the server rather than of the endpoint, and are listed with the rest in
[Operating it](#operating-it).

Each MCP session gets its own connection (over stdio there is exactly one). A connection that died while idle is replaced before the next command is
sent, so a Valkey restart costs nothing; a connection that dies *during* a call returns a connectivity error and the next call reconnects. With `VALKEY_TLS=true` the
server verifies the certificate (TLS 1.2 or later) and never falls back to plaintext: a failed handshake is a connectivity error, and a TLS setting
without `VALKEY_TLS=true` is refused at startup.

## Security model

- **The ACL is the boundary.** Create a dedicated user holding only the commands and key patterns the assistant should have:

  ```
  ACL SETUSER assistant on >a-long-secret ~docs:* ~mcpt:* -@all +fractal.search +fractal.explore +fractal.agent.recall_hybrid +fractal.vector.* +fractal.version +get +strlen +type
  ```

  A refusal comes back as an `engine` error with Valkey's own `NOPERM` text. Anything that sends data to a model (`fractal_reason`, `fractal_embed` and the agents that write
  narrative) is in the `@dangerous` category, so a user without `+@dangerous` cannot reach a model at all ([reasoning-setup.md](reasoning-setup.md#-security--governance)).
- **Read-only mode.** `FSQL_MCP_READ_ONLY=true` refuses every tool annotated destructive before it reaches Valkey, and latches the module's destructive ledger commands
  for the connection (`FRACTAL.LEDGER.LOCK`).
- **Keyspace tools.** `fractal_store_vectors`, `fractal_store_series`, `fractal_store_hash` and `fractal_delete_key` are the only tools that touch keys that are not FractalSQL's own.
  Set `FSQL_MCP_KEY_PREFIX` (for example `mcpt:`) and they refuse any other key. The FractalSQL commands themselves are limited by the ACL's key patterns, not by the prefix.
- **Untrusted data.** What an assistant sends is untrusted input and what it reads from your data is a possible prompt-injection channel. Every argument is
  type-checked, bounded (arrays at 4 Mi elements, replies at 64 MiB) and sanitised in logs: secrets, tokens and long strings are redacted or hashed.
- **HTTP.** `--http PORT` requires a credential — `FSQL_MCP_AUTH_TOKEN`, `FSQL_MCP_AUTH_TOKEN_FILE` (re-read as it changes) or
  `FSQL_MCP_TOKEN_POLICY_FILE`; it binds `127.0.0.1` unless told otherwise, and with `FSQL_MCP_HTTP_TLS=true`, `FSQL_MCP_TLS_CERT` and
  `FSQL_MCP_TLS_KEY` it serves TLS only and refuses to start on a bad certificate rather than fall back to plaintext. See
  [Authentication](#authentication) for how a client presents a token and what the server does with one it does not accept.

## Authentication

A bearer token is the credential, and it applies to the HTTP transport only: the stdio transport inherits the client's own process boundary and has no
credential of its own. TLS and the token are independent controls, and neither substitutes for the other.

**The deployment token.** `FSQL_MCP_AUTH_TOKEN` holds one secret for the whole deployment; every holder of it is the same identity and it keeps every
tool. `FSQL_MCP_AUTH_TOKEN_FILE` points at a file that holds it instead (its first line, with the trailing newline trimmed). The file is re-read
whenever it changes on disk, so a secrets manager can rotate the token in place without a restart, and an unreadable or emptied file never disables
authentication: the last known token keeps working.

**Per-client credentials.** `FSQL_MCP_TOKEN_POLICY_FILE` points at a JSON file that names clients, so each caller can be revoked on its own and can be
given its own tool allowlist:

```json
{
  "tokens": [
    { "name": "analyst", "sha256": "<hex sha256 of that client's token>",
      "tools": ["fractal_search", "fractal_agent_data_analyst"],
      "expires": "2027-01-31T00:00:00Z" },
    { "name": "batch", "sha256": "<hex sha256>", "tools": "*" }
  ]
}
```

The file holds the SHA-256 of each token, never the token itself, so it is safe to keep in a repository or a config store. The digest is taken over the
token exactly as the client presents it, after the surrounding whitespace is trimmed.

A client that presents its own token *is* that client: its name is the label its calls carry in the log, and its allowlist bounds what it can call.
`expires` is optional and is an ISO-8601 date (`YYYY-MM-DD`) or date-time (`YYYY-MM-DDTHH:MM:SSZ`); an expired credential is refused. `tools` is either
`"*"` or a list of tool names. A policy file that cannot be read or parsed stops the server at startup (exit 2); a file that goes bad later keeps the last
good policy, so a typo never widens access.

**How a client presents it.** The `Authorization` header, and only the `Authorization` header:

```
Authorization: Bearer <token>
```

A token in the URL's query string is not a credential: the query string takes no part in the request routing and is never read for a token, because a query
string ends up in proxy logs and browser history.

What the server answers at the JSON-RPC path:

| Situation | Answer |
| --- | --- |
| No `Authorization` header, or a header that is not a bearer credential | `401` with `WWW-Authenticate: Bearer realm="fractalsql-mcp"` |
| A token that matches nothing | `401` with `WWW-Authenticate: Bearer realm="fractalsql-mcp", error="invalid_token"` |
| A token whose policy entry has expired | `401` with the same `error="invalid_token"` challenge |
| A token outside its own tool allowlist | The tool is not listed by `tools/list`, and a call to it is refused as a schema-class error: `tool 'X' is not permitted for this credential` |
| A browser `Origin` header that is not a loopback origin and is not in `FSQL_MCP_ALLOWED_ORIGINS` | `403`, checked **before** the token, so an unapproved origin never reaches the credential check |
| A chunked request body | `411` — send a `Content-Length` instead |
| A declared `Content-Length` over 32 MiB | `413` |
| A repeated `Content-Length`, or both `Content-Length` and `Transfer-Encoding` | `400` — the framing is ambiguous and is never resolved in favour of one reading |
| A path the server does not serve | `404`, before the credential is looked at |
| `GET /healthz` and `GET /readyz` | served without a token |

The comparison itself is constant-time, and the token is never written to a log line: the log lines identify the caller by a short digest, never by the
token itself. Repeated failed authentications are slowed down (the wait grows by 100 ms per consecutive failure, up to two seconds), so a stolen-token
guessing attempt is not free, and a success pays one failure back. `SIGHUP` reloads the token and the policy file without a restart.

**Plaintext off the loopback is refused.** A non-loopback bind without TLS sends the bearer token and every tool result in clear text, so the server
refuses to start on one unless `FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK` is set. TLS 1.2 or later is the floor; a certificate that will not load, or a key
that does not match it, stops the server rather than falling back to plaintext.

There is no request-signing scheme, and that is deliberate: a standard MCP client cannot sign its requests, so making a signature mandatory would break
every client while a replay of a captured request stays possible either way. The controls that do work are the ones a deployment can actually operate: a
token that can be rotated without downtime, credentials that can expire, credentials that can be revoked one client at a time, and per-client allowlists
that bound what a leaked token can reach.

## Protocol versions

The server speaks two protocol eras at once, and the era a call arrived in is on the operator's log line for that call.

**The legacy era** is the handshake-based one: `initialize` first, an `Mcp-Session-Id` for the calls that follow, and the protocol revision carried in
the handshake. The revisions served are `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25` and `2026-07-28`. The `initialize` reply echoes the
revision the client asked for when it is one of those, and `2025-06-18` when it is not.

**The modern era** is the stateless one: every request carries its own metadata, and there is no protocol session at all. A modern request must carry
three headers — `MCP-Protocol-Version`, `Mcp-Method`, and, for the methods that address something by name (`tools/call`, `prompts/get`,
`resources/read`), `Mcp-Name` — and the headers must agree with the body. The body carries its revision as well, as
`_meta["io.modelcontextprotocol/protocolVersion"]` (the nested spelling `_meta["io.modelcontextprotocol"]["protocolVersion"]` is accepted too), and it
must match the header. The revisions the modern era covers are `2025-11-25` and `2026-07-28`.

What the modern era answers when a request does not satisfy those rules:

| Mismatch | Answer |
| --- | --- |
| A required header is missing, or a header disagrees with the body | `-32020` (HeaderMismatch), HTTP `400` |
| The revision is not one this server serves | `-32022` (UnsupportedProtocolVersionError), HTTP `400`, with `data.supported` (the revisions served) and `data.requested` (the one asked for) |
| A method this server does not have | `-32601`, HTTP `404` |
| A `GET` or a `DELETE` on the modern era | HTTP `405` — the modern era has no stream and no session to tear down |

The era is chosen by the request, not by the client. A request whose method is `initialize` is always the legacy handshake. Any other request is modern
when it carries the `MCP-Protocol-Version` header or the `_meta` protocol version, and legacy otherwise.

`FSQL_MCP_PROTOCOL_ERA` chooses what is served: `dual` (the default) serves both, `modern` serves the modern era only and refuses the legacy handshake
with an HTTP `400` (`-32000`) whose message names the revisions it speaks. The tools, the arguments and the results are the same in both eras; only the
framing differs. The modern era also adds the discovery method `server/discover`, and every result carries `resultType` (`complete`); every list result
carries `ttlMs` (`0`) and `cacheScope` (`session`).

**Discovering what the server can do.** `server/discover` (modern era) returns the revisions the server speaks, its name and version, and its
capabilities (`tools`, `resources`, `prompts`, `logging`). The legacy handshake's `initialize` returns the same name and version with the `tools`,
`resources` and `prompts` capabilities. `tools/list`, `resources/list`, `resources/templates/list` and `prompts/list` then enumerate the surface, narrowed
to whatever the operator enabled and to whatever the presenting credential is allowed to call.

**Conformance.** The server conforms to the MCP revisions it names above: `2026-07-28` in the modern era, and `2025-06-18` in the legacy era (the
revisions `2024-11-05`, `2025-03-26` and `2025-11-25` are served in the legacy era as well). A conformance suite in `mcp/tests/` drives a built server over
its real transports and checks the behaviour against those specifications — both eras, the authentication and framing rules, the shape of the tool
catalog, and the session-state handles.

## Session state

Two kinds of server-side state exist, and both are bounded: the engine connection a context-scoped call runs on, and the engine connection a stateless
call runs on. Each is held in the same registry, so the same two settings bound both.

A **context handle** (`context_id`) is an opaque 128-bit hex string the server mints. A caller of a context-scoped tool that names no handle gets one
back with the result — the `context_id` field of the `tools/call` result — and a caller that passes one back keeps using the same engine state. The
handle is bound to the credential that created it: a different token is told the handle does not exist, so a client can neither use nor probe for a
handle it does not own. In the legacy era there is no handle to pass: the protocol session supplies the context, and a `context_id` naming a different
one is refused.

| Setting | Default | What it bounds |
| --- | --- | --- |
| `FSQL_MCP_SESSION_TTL_SECS` | `3600` | How long an idle handle survives. |
| `FSQL_MCP_MAX_SESSIONS` | `64` | How many handles exist at once. |

Each handle owns a Valkey connection of its own, so the state is dropped in one of four ways: the handle goes idle for longer than
`FSQL_MCP_SESSION_TTL_SECS` and is evicted together with its connection; the server is shut down, which drops every handle and closes every connection
after the drain window; a legacy session is closed with `DELETE /mcp` carrying its `Mcp-Session-Id` (the modern era has no session to tear down, so
`DELETE` is refused); or the cap is reached, in which case a request that needs a new handle is refused rather than served.

The tools that act on state belonging to a caller rather than to a connection are the context-scoped ones: `fractal_diversify_enable`,
`fractal_diversify_disable`, `fractal_diversify_params`, `fractal_explain_result`, `fractal_feedback_report`,
`fractal_isolate_background`, `fractal_agent_recommend_diverse`, `fractal_agent_feedback_audit` and
`fractal_session_close`. In the modern era these run against the handle's own connection; every other tool runs against the credential's own
connection.

Valkey's Diversify state lives in a named session, and the session tools take that name in a `session` argument. The argument is optional: leave it
out and the call acts on the caller's own session, whose name is `ctx-` followed by the caller's context id. Two callers that never name a session
therefore never share state, even though they may call the same tools on the same server. Name a session explicitly and the call acts on that one
instead, which is how a client that keeps its own session names keeps full control of them. A session that does not exist yet is created the first
time a call writes to it.

## Data at rest

**On the server's own disk.** The server keeps almost nothing. It reads four files and writes three, and only when you ask it to:

| File | Direction | What it is |
| --- | --- | --- |
| The schema lock (`mcp/.mcp-schema-lock`) | read | The SHA-256 over the canonical JSON of the tool surface, compared at startup |
| The safety-annotation lock (`mcp/.mcp-safety-annotations-lock`) | read | The reviewed destructive / read-only / idempotent / open-world hints, compared at startup |
| The bearer token file (`FSQL_MCP_AUTH_TOKEN_FILE`) | read | The deployment token, re-read when it changes |
| The credential policy file (`FSQL_MCP_TOKEN_POLICY_FILE`) | read | Per-client SHA-256 digests, tool allowlists and expiry dates |
| The schema lock, the safety-annotation lock and the manifest | written | Only by `--write-schema-lock`, `--write-safety-annotations-lock` and `--write-manifest` |

The token file is the one of those that holds a secret in the clear: the deployment token itself, on one line. The policy file holds only SHA-256
digests, never a token.

There is no server-managed log file: the server writes one JSON line per tool call to stderr, and what you do with stderr — a journal, a file, a
collector — is your deployment's decision. The lock files are looked for in the working directory first, then next to the executable, then in the
package's documentation directory.

**In the Valkey keyspace.** The server writes to it in three places, and all three are ordinary Valkey data, governed by the ACL of the user the server
connects as:

- the corpus, series and hash keys the store tools write (`fractal_store_vectors`, `fractal_store_series`, `fractal_store_hash`) and the keys
  `fractal_delete_key` removes. With `FSQL_MCP_KEY_PREFIX` set they are confined to that prefix;
- the key-ownership hash `fsqlmcp:owners`, which records which MCP client wrote each key so one client cannot overwrite or delete another's key (turn it
  off with `FSQL_MCP_KEY_OWNERSHIP=off`);
- the scratch key `fsqlmcp:tmp:…` an inline `corpus` is written to for a search. It carries a 120-second expiry and is deleted again as soon as the
  search returns.

The engine's own state — the diversify sessions, the agent decision documents and the decision ledger — lives behind the module's commands, in the
module's memory and, for the ledger, in the daemon's chain. The MCP server holds none of it itself.

**In the daemon's data directory.** The decision ledger is not in Valkey and not in this server. The daemon writes three files, all next to each other
in the directory its `enterprise_ledger_path` points at (by default `fractalsql_ledger.dat`, relative to the daemon's working directory):

| File | What it is |
| --- | --- |
| The ledger (`<ledger>`) | The authoritative hash chain, appended to as records are written |
| The CSV mirror (`<ledger>.csv`) | The same records in a queryable CSV, one row per record. A read surface, not a second source of truth |
| The head file (`<ledger>.head`) | One record per chain kind saying how many rows that kind was ever appended to, so a truncated tail is visible from outside the chain |

All three are created mode `0600` — owner read and write, nothing for the group or the world — and an existing file with wider permissions is tightened to
the same on the next write. The head file holds at most one record per chain kind, and the chain kinds run from 1 to 16; it is rewritten atomically on
every append, so a reader never sees a half-written head.

The daemon's own settings live beside them: `fractalsqld.conf`, which may hold a provider token and a ledger key, and the HMAC key file, which holds the
key that authenticates the daemon's replies to the module. The daemon refuses to start, or to reload, if the config file is readable by group or others.

**The audit text is stored verbatim.** An audit record holds the caller's JSON payload — for an agent, the whole decision document including the model's
written analysis — plus a server-assigned `_actor` block (the Valkey client id and the ACL user) and, for a call that came through this server, the
`mcp_client` field naming the session or token that made it.

**Confidentiality at rest comes from the storage layer.** Nothing here encrypts a record:

- **The ledger, the CSV mirror and the head file are covered by volume encryption only.** There is no setting that encrypts a record inside them, and
  this project does not encrypt records inside the ledger. The ledger's key is a chain key: it proves who wrote a record and that it was not altered in
  the middle, and it does not hide what the record says. Encrypt the volume, the disk or the backups those three files live on, and they are covered;
  with the volume unencrypted, anyone who can read the files can read the audit text — the question an agent was asked, the analysis it wrote, the rows
  it read — in the clear.
- **The Valkey keyspace is covered by whatever your Valkey deployment offers.** Upstream Valkey has no record-level encryption: there is no setting that
  encrypts a key or a value inside the dataset, and its documented security controls are TLS for traffic in transit, ACLs, network isolation and protected
  mode. A managed Valkey service may add its own at-rest encryption for the data it holds — Amazon ElastiCache, for example, supports at-rest encryption
  on replication groups running Valkey 7.2 and later and enables it by default for Valkey — and where the deployment you run offers it, that is what
  protects the keyspace. Check the product's own documentation for the version you run before assuming it is on. Where it is not, the keyspace is covered
  by the encryption of the volume it lives on, like everything else here.
- **The bearer token file is covered by the volume and by its permissions.** It holds a secret in the clear, so it belongs on the same footing as the
  daemon's key file.

**Redaction is the lever for a sensitive deployment.** `FSQL_AUDIT_REDACT=hash` replaces the audit text with a fingerprint — `sha256:<hex> len=<bytes>` —
so the chain still shows that the same question was asked twice and that the answer did not change, without recording what the question was. `truncate`
keeps the first 64 characters instead. Both apply to the persisted copy only: the agent still answers with the real question, the real SQL and the real
analysis, and the values handed back to the caller are never redacted. For a deployment where the trail has to be kept but the text may not be, `hash` is
the setting to reach for.

**Anchoring the chain.** A hash chain cannot show that its newest records were cut off the end of the file. `FSQL_LEDGER_ANCHOR_LOG=stderr` (or
`syslog`) publishes one JSON line per append — `{"event":"ledger_head","kind":K,"rows":N,"hash":"<hex>"}` — so the chain's head can be shipped to a place
whose administrator cannot rewrite it. It is off by default and publishes nothing while it is off.

## How arguments map

| You send | The server sends Valkey |
| --- | --- |
| a JSON array of numbers | a packed little-endian float32 blob (strings that parse as numbers are accepted; `NaN`, `Infinity` and non-numbers are refused, naming the element) |
| an array of equal-length arrays | the rows flattened row-major; the row width becomes the `dim` the command wants, so you never pass it |
| an array of integers (faces, edges) | packed little-endian uint32 |
| a hex string, or an array of equal-length hex strings | raw bytes, and the per-item byte length where the command wants it |
| a key name | the key name |
| an object (`context`, `params`, `payload`) | its JSON text |
| `filter: {hash, value}` | `FILTER hash value` |

`fractal_search` and `fractal_search_explore` accept either `corpus_key` (a stored corpus) or `corpus` (an inline array of rows). An inline corpus is written to a scratch key
that expires after 120 seconds, searched, and deleted; it is never left behind. Optional arguments you skip are left off, and an optional argument that sits before one you did give is
filled with the command's documented default.

Results are always a JSON object (`structuredContent`): a command's JSON reply is returned as is, a number or string as `{"value": ...}`, a vector as `{"vector": [...], "dim": n}`,
raw bytes as `{"hex": ..., "bytes": n}`, a list as `{"items": [...]}`.

## Errors

Every failure has a class, five classes:

| `error_class` | Meaning | Examples |
| --- | --- | --- |
| `schema` | The call was refused before anything was sent | wrong type, missing argument, non-numeric element, a key outside the prefix, read-only mode |
| `engine` | It reached the module or Valkey and was refused | `ERR series key missing`, `NOPERM`, `WRONGTYPE`, "the fractalsqld daemon is not configured" |
| `connectivity` | Valkey or the daemon could not be reached | refused or unresolvable host, bad credentials, a timeout, the module not loaded |
| `cancelled` | The client sent `$/cancel` | |
| `server` | A bug in this server; the detail is logged, never sent | |

Agents that call a model degrade honestly: when the daemon or model is unavailable they return the computed half of their answer plus `reasoning_step_failed`, exactly as the commands do.

## Tools

The catalog below is generated from the server (`tools/list`). The flags are the MCP annotations the server sends for every tool
(`readOnlyHint`, `destructiveHint`, `idempotentHint`, `openWorldHint`); `model` means the call can reach the reasoning model.

Each tool's arguments are described by the `inputSchema` the server publishes in `tools/list`; that schema is the source of truth, and the schema lock hashes it, so a change to any tool's arguments shows up as a lock change.

**Every tool marked `destructiveHint: true` is refused in read-only mode.** That is the set read-only mode (`FSQL_MCP_READ_ONLY=true`) refuses before anything reaches Valkey, and
the set a client that honours the annotation asks about before it calls. The rule is the annotation itself: every tool marked destructive is refused in
read-only mode, and nothing else is. The set is larger than the set that obviously writes, because the agents are in it
too: a successful agent records its decision document in the audit ledger, so it changes state. The two search tools change the state of a diversify
session but are not annotated destructive; the `Flags` column says which is which.


### Discovery (search, sessions, diversification)

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_search` | Sniper-mode Stochastic Fractal Search: the K nearest rows of a corpus to a query, minimizing cosine distance. | writes session state |
| `fractal_search_explore` | Scout-mode SFS: the whole converged population (every particle's position and fitness) rather than only the top K, for clustering and diversity analysis. | writes session state |
| `fractal_diversify_enable` | Start (or reset) a named diversify session: later searches given the same session avoid the neighbourhoods it has been told to reject. | **destructive** |
| `fractal_diversify_disable` | Turn diversification off for a session (the session and its history stay). | **destructive** |
| `fractal_diversify_params` | Read a session's diversify parameters, or set some of them with a JSON object (window_n, stall_threshold, repulsion_sigma, repulsion_weight, max_shadows_considered, tail_buffer_cap). | **destructive** |
| `fractal_explain_result` | Why the last search through a session looked the way it did: D_q, whether diversify is on, and overhead. | read-only |
| `fractal_feedback_report` | Tell a session how a result was received so later searches steer away from rejected rows. | **destructive** |
| `fractal_isolate_background` | Mark one result as background noise for a session so it is excluded from future repulsion. | **destructive** |
| `fractal_session_close` | Drop a diversify session and free its memory. | **destructive** |
| `fractal_session_list` | List the live diversify sessions. | read-only |

### Cognition (needs the fractalsqld daemon)

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_reason` | Send a question (and optional JSON context) to the configured reasoning model through the fractalsqld daemon. | read-only, model |
| `fractal_embed` | Embed text with the configured embedding model through the fractalsqld daemon; returns the vector. | read-only, model |

### Agency (the agents)

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_agent_anomaly_triage` | Drift on one entity's metric series, then a model-written triage summary. | **destructive**, model |
| `fractal_agent_allocate` | Cardinality-constrained portfolio optimisation, then a written rationale. | **destructive**, model |
| `fractal_agent_route_task` | Route a task to the nearest capability under a token budget, with a written justification. | **destructive**, model |
| `fractal_agent_outlier_intercept` | Intercept a proposed state that is too close to a known-bad one. | **destructive**, model |
| `fractal_agent_recall_hybrid` | Pure retrieval, no model: the K nearest memories to a query, optionally restricted to a cohort. | **destructive** |
| `fractal_agent_recommend_diverse` | Pure retrieval, no model: K recommendations that are relevant and mutually diverse. | **destructive** |
| `fractal_agent_data_analyst` | Natural-language question over a valkey-search index: the model writes a query, the module validates and runs it under YOUR ACL, and the model analyses the rows. | **destructive**, model |
| `fractal_agent_patient_deterioration_triage` | Cohort search plus the drift from baseline to current, then a written triage. | **destructive**, model |
| `fractal_agent_feedback_audit` | Self-contained diversify health check: warm a session with the first rows of a corpus, search, and report the diversity quotient. | **destructive** |
| `fractal_agent_schedule_workload` | Place a workload on the nearest node by capability vector, with a written justification and confidence. | **destructive**, model |
| `fractal_agent_rebalance_sibling` | Optimise a portfolio, find the most similar past allocation by weight change, and explain the rebalance. | **destructive**, model |
| `fractal_agent_detour_classify` | Classify a vehicle's detour: nearest fleet peer by route change and the box-counting complexity of its GPS trace, with a written verdict. | **destructive**, model |
| `fractal_agent_track_anomaly` | Track deviation check: nearest peer plus the DFA exponent of the heading series, with a written assessment. | **destructive**, model |
| `fractal_agent_network_coverage_alert` | Sensor-grid coverage: morphological dimension and lacunarity of the point cloud plus drift in the telemetry, then an alert. | **destructive**, model |
| `fractal_agent_regime_triage` | General-purpose regime check on any series: DFA exponent and drift, then a written triage. | **destructive**, model |
| `fractal_agent_diverse_portfolios` | Enterprise: several mutually different near-optimal portfolios (sharpe) or the return-versus-risk front (pareto), with a written comparison. | **destructive**, model |
| `fractal_agent_plan_explore` | Embed a seed state and branch it into the nearest strategies. | **destructive**, model |
| `fractal_agent_trajectory_predict` | Project the next state from a time-ordered state log: the drift since a baseline row, applied to the current state. | **destructive** |
| `fractal_agent_detect_loop` | Detect an agent stuck in a loop: fingerprint each logged state and find a cycle, tolerating a few differing bits. | **destructive** |
| `fractal_search_agent` | Embed a question, search the corpus, and answer from the hits. | **destructive**, model |
| `fractal_rag_agent` | Retrieval-augmented answer: search_agent with a fixed search. | **destructive**, model |

### Analytics (series, geometry, optimisation, vector math)

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_dimension_dfa` | Detrended fluctuation analysis exponent of a series: about 0.5 for noise, above 1 for persistent trends. | read-only |
| `fractal_dimension_drift` | Regime drift: the DFA exponent of the recent window against the baseline before it. | read-only |
| `fractal_dimension_boxcount` | Box-counting fractal dimension of a point cloud. | read-only |
| `fractal_periodogram` | Dominant frequencies of a series: the strongest spectral peaks. | read-only |
| `fractal_change_point_detect` | Indices where the series' behaviour changes. | read-only |
| `fractal_state_fingerprint` | A compact bit fingerprint of a vector (random-hyperplane hash); returns hex bytes. | read-only |
| `fractal_spectrogram_entropy` | Spectral entropy per sliding frame: low for tonal signals, near 1 for noise. | read-only |
| `fractal_cycle_detect` | Find a cycle in a stream of fingerprints (Brent's algorithm), tolerating a few differing bits. | read-only |
| `fractal_tda_persistence_diagram` | Persistent homology of a point cloud: H0 and H1 barcodes. | read-only |
| `fractal_morphological_complexity` | Fractal dimension and lacunarity of a point cloud. | read-only |
| `fractal_cortical_folding` | Gyrification index of a mesh: surface area against its convex hull. | read-only |
| `fractal_nerve_plexus_metric` | Fibre-length density and branch density of a nerve skeleton. | read-only |
| `fractal_vascular_network` | Tortuosity, branch density and fractal dimension of a vessel tree. | read-only |
| `fractal_optimize_portfolio` | Cardinality-constrained Sharpe maximisation by Stochastic Fractal Search. | read-only |
| `fractal_vector_lp_distance` | Minkowski L_p distance. | read-only |
| `fractal_vector_hamming_distance` | Differing bits between two equal-length binary strings (hex). | read-only |
| `fractal_vector_quantize_int8` | Scalar int8 quantisation: returns scale and codes (4x smaller). | read-only |
| `fractal_vector_quantize_binary` | Sign-bit quantisation: one bit per dimension, as hex bytes. | read-only |

### Enterprise (ledger and audit; needs the daemon and the enterprise tier)

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_ledger_verify` | Walk a ledger chain and report whether it is intact (ok, rows_verified, or the first failure). | read-only |
| `fractal_ledger_truth_count` | Entries in the truth ledger. | read-only |
| `fractal_ledger_shadow_count` | Entries in the shadow ledger. | read-only |
| `fractal_ledger_flush` | Persist the ledger to its file. | **destructive** |
| `fractal_ledger_load` | Load the persisted ledger into memory. | **destructive** |
| `fractal_ledger_compact` | Compact the ledger. | **destructive** |
| `fractal_ledger_reset_soft` | Clear the in-memory shadow state, keeping the truth ledger. | **destructive** |
| `fractal_ledger_reset_hard` | Erase the ledger entirely. | **destructive** |
| `fractal_audit_log` | Append a record to the tamper-evident decision-audit chain (kind 2). | **destructive** |
| `fractal_audit_unpack` | Decode a persisted audit blob (hex) into its JSON. | read-only |
| `fractal_optimize_portfolio_multimodal` | Enterprise: several mutually different near-optimal portfolios. | read-only |
| `fractal_optimize_portfolio_multimodal_pareto` | Enterprise: the non-dominated return-versus-risk front. | read-only |

### Keyspace helpers

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_store_vectors` | Store rows as a corpus: packs the rows as float32 into one key so fractal_search and the agents can use it. | **destructive** |
| `fractal_store_series` | Store a numeric series as packed float32 under a key, for the series tools and agents. | **destructive** |
| `fractal_store_hash` | Write a hash: the IDS / CONTENT / FILTER companions the agents read (field = row index). | **destructive** |
| `fractal_delete_key` | Delete a key (a corpus, series or hash this server wrote). | **destructive** |
| `fractal_key_info` | Describe a key: its type and, for a corpus or series, how many float32 values it holds. | read-only |

### Admin

| Tool | What it does | Flags |
| --- | --- | --- |
| `fractal_version` | The module's version and edition, the Valkey server version and this MCP server's version. | read-only |


The sixteen recipes and five building blocks are also available as one catalog resource, `fractalsql://agents`.

## Resources and prompts

| Resource | Contents |
| --- | --- |
| `fractalsql://agents` | Which agent to use for which problem, and which need a model |
| `fractalsql://config` | Module version and edition, Valkey version, the endpoint and ACL user, whether a daemon is attached and whether the enterprise tier is behind it. Never a secret |
| `fractalsql://ledger/{kind}` | The ledger audit report (needs the daemon) |
| `fractalsql://key/{key}` | What a key holds, and how many float32 values |

Ten prompts cover the common flows: `incident_triage`, `portfolio_rebalance`, `patient_triage`, `fleet_detour_check`, `track_anomaly_check`, `sensor_grid_health`,
`churn_recovery`, `ask_my_data`, `diversity_audit` and `ciso_ledger_check`. Each names the tools to call and carries the least-privilege reminder where it matters.

## Operating it

### Settings

Every setting the server reads from the environment, with the default it uses when the variable is unset or empty. The ones an operator turns in a
hardened deployment are collected in the [MCP README](../mcp/README.md).

**Transport and credentials**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_HTTP_PORT` | `8765` | The port the HTTP transport listens on (also `--http PORT`) |
| `FSQL_MCP_BIND` | `127.0.0.1` | The address the HTTP transport binds: an IPv4 dotted quad, `localhost`, `*` for every interface, or an empty value for the loopback. Also `--bind` / `--host` |
| `FSQL_MCP_HTTP_PATH` | `/mcp` | The path the JSON-RPC endpoint is served on |
| `FSQL_MCP_HTTP_TLS` | `false` | Serve TLS on the HTTP transport. Needs `FSQL_MCP_TLS_CERT` and `FSQL_MCP_TLS_KEY`; a certificate that will not load, or a key that does not match it, stops the server rather than falling back to plaintext |
| `FSQL_MCP_TLS_CERT` / `FSQL_MCP_TLS_KEY` | none | The server's certificate and its private key (PEM) |
| `FSQL_MCP_AUTH_TOKEN` | none | The deployment bearer token. The HTTP transport refuses to start without a credential |
| `FSQL_MCP_AUTH_TOKEN_FILE` | none | A file holding the deployment token (its first line). Re-read when it changes on disk |
| `FSQL_MCP_TOKEN_POLICY_FILE` | none | A JSON file of per-client credentials: SHA-256 digests, tool allowlists and expiry dates |
| `FSQL_MCP_ALLOWED_ORIGINS` | none | Comma-separated browser origins allowed besides the loopback ones. An origin outside the list is refused with `403` before the token is checked |
| `FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK` | off | Accept a plaintext HTTP bind on a non-loopback address. Off by default: the server refuses to start on one |
| `FSQL_MCP_MAX_CONNECTIONS` | `256` | How many HTTP connections are served at once. On POSIX the result never exceeds half the process's descriptor limit |
| `FSQL_MCP_PROTOCOL_ERA` | `dual` | `dual` serves both protocol eras; `modern` serves the modern era only and refuses the legacy handshake |

**Session and state**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_SESSION_TTL_SECS` | `3600` | How long an idle context handle survives |
| `FSQL_MCP_MAX_SESSIONS` | `64` | How many context handles exist at once |
| `FSQL_MCP_MAX_ROWS_CEILING` | `1000` | The hard ceiling on the rows a tool may return. A tool's own default is 100 |
| `FSQL_MCP_KEY_PREFIX` | none | The prefix the keyspace tools write and delete under. Empty means any key in the keyspace |
| `FSQL_MCP_REQUIRE_KEY_PREFIX` | `false` | Refuse to start unless `FSQL_MCP_KEY_PREFIX` is set |
| `FSQL_MCP_KEY_OWNERSHIP` | `true` | Record which MCP client wrote each key and refuse another client's overwrite or delete. `off` for a deployment that deliberately shares keys between sessions |

**Guardrails**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_READ_ONLY` | `false` | Refuse every tool annotated destructive before it reaches Valkey, and latch the module's destructive ledger commands for the connection |
| `FSQL_MCP_REQUIRE_LEDGER_LATCH` | `false` | With read-only mode on, refuse the Valkey connection when the ledger latch cannot be applied instead of warning |
| `FSQL_MCP_ALLOW_AUTO_EXECUTE` | off | Let a model ask for `auto_execute` on the tools that take it. Off by default: the parameter is controlled by the same actor the control protects against, so the operator decides |
| `FSQL_MCP_CONFIG_WRITES` | `safe` | Which config keys a config-writing tool may write (`off`, `safe`, `all`). This engine exposes no config-writing tool, so the switch has no call site today and the safe set is empty |
| `FSQL_MCP_TOOL_WHITELIST` / `--tool a,b` | all | Serve only these tools |
| `FSQL_MCP_ENABLED_CATEGORIES` / `--category x,y` | all | Serve only these categories (`discovery`, `cognition`, `agency`, `analytics`, `enterprise`, `keyspace`, `admin`) |

**Capacity and lifecycle**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_MAX_CONCURRENT_CHEAP` | `50` | How many cheap calls run at once |
| `FSQL_MCP_MAX_CONCURRENT_EXPENSIVE` | `5` | How many model-backed calls run at once |
| `FSQL_MCP_MAX_CONCURRENT_EXPENSIVE_PER_CLIENT` | `2` | How many model-backed calls one client may have in flight |
| `FSQL_MCP_MAX_QUEUE_WAIT_MS` | `900000` | How long a call waits for a slot before `server busy`. The three concurrency caps are each a whole number of at least 1; anything else, including 0, makes the server refuse to start rather than hang every call |
| `FSQL_MCP_SHUTDOWN_DRAIN_MS` | `5000` | How long shutdown waits for in-flight calls |
| `FSQL_MCP_LOG_LEVEL` | `normal` | `verbose` adds debug lines. One JSON line per tool call on stderr, arguments redacted and capped |

**Locks**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_SCHEMA_LOCK` | `mcp/.mcp-schema-lock` | The schema lock's path (also `--schema-lock-path`) |
| `FSQL_MCP_SAFETY_LOCK` | `mcp/.mcp-safety-annotations-lock` | The safety-annotation lock's path |
| `FSQL_MCP_MANIFEST` | `mcp/manifest.json` | Where `--write-manifest` writes the manifest |
| `FSQL_MCP_STRICT_SCHEMA_HASH` | `true` | Refuse to start if the catalog differs from the schema lock; `false` warns instead |
| `FSQL_MCP_ALLOW_MISSING_LOCKS` | off | Run without the drift check when a lock file is missing (development only) |
| `FSQL_MCP_REQUIRE_LOCKS` | on | A missing lock file stops startup. `false` is the same as `FSQL_MCP_ALLOW_MISSING_LOCKS=true` |

**The Valkey connection**

| Setting | Default | Effect |
| --- | --- | --- |
| `FSQL_MCP_VALKEY_CONNECT_TIMEOUT_MS` | `10000` | TCP connect |
| `FSQL_MCP_VALKEY_TIMEOUT_MS` | `600000` | One reply. A model-backed agent can take minutes; size it for yours |
| `FSQL_MCP_VALKEY_MAX_REPLY_MB` | `64` | Largest reply accepted. A server claiming more is refused and the connection dropped |

**The daemon and the engine behind it**

The server above is one process. The model-backed tools and the audit trail belong to two others: the `fractalsqld` daemon, and the module and engine it
drives. Those read their own environment, so the settings below are set on the daemon rather than on the MCP server.

| Setting | Default | Effect |
| --- | --- | --- |
| `FRACTALSQL_REASONING_PLUGIN` | none | The reasoning plugin's shared library. The `reasoning_plugin` key in the config file wins over it ([Reasoning Setup](reasoning-setup.md)) |
| `FSQL_REASONING_HTTP_TOKEN` | none | The bearer token or API key sent to the provider. Prefer the plugin's token file |
| `FSQL_REASONING_HTTP_THINK` | none | The reasoning effort: `none`, `low`, `medium` or `high` |
| `FSQL_REASONING_HTTP_THINK_PROVIDER` | none | Which provider's thinking shape to use: `openai`, `ollama`, `anthropic` or `vllm` |
| `FSQL_REASONING_HTTP_NATIVE_URL` | none | The native endpoint for `ollama` and `anthropic`, which have no OpenAI-compatible thinking control |
| `FSQL_REASONING_HTTP_NUM_CTX` | none | The context size for `ollama`, 1 to 10000000 |
| `FSQL_REASONING_HTTP_RESPONSE_MODE` | none | How the plugin reads the model's reply: `text` (the default), `code` or `json`. Read when the daemon starts |
| `FSQL_REASONING_HTTP_SYSTEM_TAG` | none | One sentence added to the system message, naming the target system |
| `FRACTALSQL_HTTP_URL` | none | The chat endpoint; the older spelling of `FSQL_REASONING_HTTP_URL` |
| `FRACTALSQL_HTTP_MODEL` | none | The chat model; the older spelling of `FSQL_REASONING_HTTP_MODEL` |
| `FRACTALSQL_HTTP_ALLOW_PLAINTEXT` | off | Permits a plain-HTTP endpoint; the older spelling of `FSQL_REASONING_HTTP_ALLOW_PLAINTEXT`. Any non-empty value except `0` allows it |
| `FRACTALSQL_HTTP_EMBED_URL` | none | The embeddings endpoint. Deliberately has no `FSQL_REASONING_HTTP_*` twin: a preset embedding URL under that namespace leaked into the chat path |
| `FRACTALSQL_HTTP_EMBED_MODEL` | none | The embeddings model, for the same reason |
| `FSQL_AUDIT_REDACT` | `none` | How much of the audit text the ledger stores: `none` keeps it as it is, `hash` stores `sha256:<hex> len=<bytes>` in its place, `truncate` keeps the first 64 characters. It applies to the persisted copy only; the agent still answers with the real text ([Enterprise Tier](enterprise.md#redacting-the-stored-text)) |

Two more settings decide whether the native reasoning plugin may be loaded at
all. The plugin is a shared object mapped into the daemon's own process, so
loading one is arbitrary code execution, and the daemon will not do it
unchecked: the plugin must carry a detached Ed25519 signature from the
FractalSQLabs reasoning-plugin signing key (a `<plugin>.sig` file next to the plugin of
exactly 64 bytes).

**Settings that appear in no other page of this documentation**

| Setting | Default | Effect |
| --- | --- | --- |
| `FRACTALSQL_CONFIG` | `/etc/fractalsql/fractalsqld.conf` | The daemon's configuration file. `-c <path>` overrides it |
| `FRACTALSQL_ENTERPRISE_LIB` | none | The enterprise shared library the daemon loads. The `enterprise_lib` config key wins over it, and it cannot be changed while a library is already loaded |
| `FRACTALSQL_ENTERPRISE_LEDGER_PATH` | `fractalsql_ledger.dat`, relative to the daemon's working directory | Where the ledger file is written. The `enterprise_ledger_path` config key wins over it |
| `FRACTALSQL_ENTERPRISE_LEDGER_KEY` | none | The HMAC key that authenticates ledger records and the head file's own MAC. The `enterprise_ledger_key` config key wins over it |
| `FRACTALSQL_ENTERPRISE_REQUIRE_SIGNATURE` | on | The operator's control over the enterprise signature requirement. It is on by default: an enterprise library with no detached signature file is refused, and an invalid signature is always refused. Set it to `0` to load an unsigned development build anyway. The `enterprise_require_signature` config key wins over it |
| `FSQL_LEDGER_ANCHOR_LOG` | `off` | `stderr` or `syslog` publishes one JSON line per ledger append — `{"event":"ledger_head","kind":K,"rows":N,"hash":"<hex>"}` — so the chain's head can be anchored somewhere its administrator cannot rewrite it |
| `FSQL_REASONING_REQUIRE_SIGNATURE` | `1` | Set to `0` to load a plugin that has no `.sig` (one warning goes to stderr), for a plugin you built yourself. A `.sig` that is present but wrong is always refused |
| `FSQL_REASONING_HTTP_URL` | none | The chat endpoint. Wins over `FRACTALSQL_HTTP_URL` when both are set |
| `FSQL_REASONING_HTTP_MODEL` | none | The chat model. Wins over `FRACTALSQL_HTTP_MODEL` |
| `FSQL_REASONING_HTTP_ALLOW_PLAINTEXT` | off | Permits a plain-HTTP endpoint. Any non-empty value except `0` allows it. Wins over `FRACTALSQL_HTTP_ALLOW_PLAINTEXT` |
| `FSQL_REASONING_HTTP_MODE` | none | Set by the daemon rather than by the operator: it reads `embedding` while the embeddings tier is loaded, so the plugin knows which kind of call it is serving |
| `FRACTALSQL_HTTP_TOKEN` | none | The older spelling of `FSQL_REASONING_HTTP_TOKEN`; the newer name wins |
| `FRACTALSQL_HTTP_THINK` | none | The older spelling of `FSQL_REASONING_HTTP_THINK`; the newer name wins |
| `FRACTALSQL_HTTP_THINK_PROVIDER` | none | The older spelling of `FSQL_REASONING_HTTP_THINK_PROVIDER`; the newer name wins |
| `FRACTALSQL_HTTP_NATIVE_URL` | none | The older spelling of `FSQL_REASONING_HTTP_NATIVE_URL`; the newer name wins |
| `FRACTALSQL_HTTP_NUM_CTX` | none | The older spelling of `FSQL_REASONING_HTTP_NUM_CTX`; the newer name wins |

A provider setting is read from the daemon's environment only when the config file does not carry the matching key, and the config file wins. Where both
spellings of a setting are present in the environment, the `FSQL_REASONING_HTTP_*` name is the one the reasoning tier reads.

The server's own version and protocol name are compiled in rather than read from the environment, and are reported by the `fractal_version` tool and
the `fractalsql://config` resource.

`--list-tools`, `--write-schema-lock`, `--write-safety-annotations-lock` and `--write-manifest` run without touching Valkey. The safety-annotation lock is always enforced: a change to any
tool's destructive, read-only or idempotent hint stops the server (exit 3) until the lock is regenerated on purpose.

**Cancellation.** `$/cancel` stops the *wait*: the server drops its connection to Valkey (`CLIENT KILL ID`), the call returns cancelled, and the next call reconnects. The work a module
command already started on Valkey's side cannot be interrupted and finishes; its reply is discarded.

**Platforms.** Linux and macOS, like the module. On macOS the server uses Homebrew's OpenSSL (`brew install openssl@3`); the module itself does not.

## Testing

`./build_test.sh` includes an MCP suite that runs against a real Valkey with the module loaded. It builds the server and runs the unit probes (RESP parsing of well-formed and hostile bytes, error
classification, the connection layer against a fake server that stalls, hangs up, lies about lengths, floods and refuses passwords, argument coercion, the concurrency caps, secret
redaction); runs a full MCP session (every tool once, the numbers the tools must produce, the error taxonomy, resources, prompts, the key-prefix and read-only guards, `$/cancel`,
shutdown) followed by a framing/robustness sweep and a sweep that mutates one argument of every tool; drives the HTTP transport over TLS; and starts the server in 139 different
ways, killing its connection underneath it. Under `--asan`, `--ubsan` and `--tsan` the server is built with that sanitizer; the server and its drivers have also been run clean under ASan and UBSan together.

## Related

- [Reasoning Setup](reasoning-setup.md) and the [FractalSQL Daemon](fractalsql-daemon.md): what the model-backed tools need.
- [Discovery](api-discovery.md), [Analytics](api-analytics.md), [Agency](api-agency.md): the commands behind the tools.
