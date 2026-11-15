<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# fractalsql-valkey-mcp

A Model Context Protocol server for FractalSQL on Valkey: 69 typed tools over the module's commands, stdio or HTTP/TLS. A standalone C++17 client of Valkey (it speaks RESP itself; no client library, no link to the module).

    make -C mcp                                  # needs a C++17 compiler and OpenSSL headers
    VALKEY_PORT=6379 mcp/fractalsql-valkey-mcp --list-tools

Documentation: [docs/mcp-plugin.md](../docs/mcp-plugin.md). Tests: the MCP suite in `./build_test.sh`, or `mcp/tests/` directly.

Layout: `src/` the generic MCP core (JSON-RPC, transports, concurrency, registry, locks), `src/db.*` the RESP client, `src/adapters/commands.cpp` the whole tool catalog as one table,
`tests/` the probes and the drivers, `.mcp-schema-lock` and `.mcp-safety-annotations-lock` the committed catalog fingerprints, `manifest.json` the tool manifest.

## Authentication

The HTTP transport requires a bearer token; the stdio transport has none, because it inherits the client's own process boundary. A credential comes from
one of three places: `FSQL_MCP_AUTH_TOKEN` (one secret for the whole deployment), `FSQL_MCP_AUTH_TOKEN_FILE` (the same, kept in a file whose first line is
the token, re-read whenever it changes so a secrets manager can rotate it in place), or `FSQL_MCP_TOKEN_POLICY_FILE` (per-client credentials).

The policy file is JSON:

```json
{ "tokens": [
  { "name": "analyst", "sha256": "<hex sha256 of that client's token>",
    "tools": ["fractal_search"], "expires": "2027-01-31T00:00:00Z" }
] }
```

Each client is revoked on its own, gets its own tool allowlist (`"*"` for all of them), and can carry an expiry date; the file holds the SHA-256 of each
token, never the token itself. A client that presents its own token *is* that client: its name labels its calls in the log and its allowlist bounds what
it can call.

A client presents the token in the `Authorization` header and nowhere else — `Authorization: Bearer <token>`. A token in the URL's query string is not a
credential: the query string takes no part in the routing and is never read for a token, because it ends up in proxy logs and browser history. The
comparison is constant-time and the token is never written to a log line. A missing or wrong token is answered with `401` and
`WWW-Authenticate: Bearer realm="fractalsql-mcp"`, plus `error="invalid_token"` when a token was presented and did not match. Repeated failures are
slowed down, 100 ms per consecutive failure up to two seconds, so guessing is not free.

`FSQL_MCP_ALLOWED_ORIGINS` lists the browser origins allowed besides the loopback ones. An origin outside the list is refused with `403` *before* the
token is checked, so an unapproved origin never reaches the credential check. A chunked request body is refused with `411` — send a `Content-Length`.

TLS and the token are independent controls and neither substitutes for the other: the token over plaintext is a secret on the wire, and TLS without a
token is an open door. `FSQL_MCP_HTTP_TLS=true` with `FSQL_MCP_TLS_CERT` and `FSQL_MCP_TLS_KEY` serves TLS only (1.2 or later) and stops the server
rather than falling back to plaintext on a bad certificate. A plaintext bind on a non-loopback address is refused unless
`FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK` is set. `SIGHUP` reloads the token and the policy file without a restart.

## Protocol versions

The server speaks two eras at once, and the era a call arrived in is on its log line.

The **legacy** era is the handshake-based one: `initialize` first, an `Mcp-Session-Id` for the calls that follow, and the revision carried in the
handshake. The revisions served are `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25` and `2026-07-28`; the reply echoes the revision the client
asked for when it is one of those, and `2025-06-18` when it is not.

The **modern** era is the stateless one: every request carries its own metadata and there is no protocol session at all. A modern request must carry
`MCP-Protocol-Version`, `Mcp-Method`, and, for the methods that address something by name (`tools/call`, `prompts/get`, `resources/read`), `Mcp-Name`;
the headers must agree with the body, which carries its revision as `_meta["io.modelcontextprotocol/protocolVersion"]`. The revisions the modern era
covers are `2025-11-25` and `2026-07-28`.

| Situation | Answer |
|---|---|
| A required header is missing, or a header disagrees with the body | `-32020` (HeaderMismatch), HTTP `400` |
| The revision is not one this server serves | `-32022` (UnsupportedProtocolVersionError), HTTP `400` |
| A method this server does not have | `-32601`, HTTP `404` |
| A `GET` or a `DELETE` on the modern era | HTTP `405` |

`FSQL_MCP_PROTOCOL_ERA` chooses what is served: `dual` (the default) serves both, `modern` serves the modern era only and refuses the legacy handshake
with an HTTP `400` whose message names the revisions it speaks. The tools, the arguments and the results are the same in both eras; only the framing
differs. The modern era adds `server/discover`, which returns the revisions served, the server's name and version, and its capabilities.

**Conformance.** The server conforms to the MCP revisions it names above: `2026-07-28` in the modern era, and `2025-06-18` in the legacy era (the
revisions `2024-11-05`, `2025-03-26` and `2025-11-25` are served in the legacy era as well). A conformance suite in `mcp/tests/` drives a built server over
its real transports and checks the behaviour against those specifications — both eras, the authentication and framing rules, the shape of the tool
catalog, and the session-state handles.

## Session state

A caller that acts on state belonging to a caller rather than to a connection passes a `context_id` — the handle the server mints and returns with
the result. The handle is bound to the credential that created it, so a client can neither use nor probe for a handle it does not own.

Valkey's Diversify state lives in a named session, and the session tools take that name in a `session` argument. The argument is optional: leave it
out and the call acts on the caller's own session, whose name is `ctx-` followed by the caller's context id, so two callers never share state even
when neither names a session. Name a session explicitly and the call acts on that one instead.

## Settings

| Variable | Default | Effect |
|---|---|---|
| `FSQL_MCP_HTTP_PORT` | `8765` | The port the HTTP transport listens on (also `--http PORT`). |
| `FSQL_MCP_BIND` | `127.0.0.1` | The address the HTTP transport binds: an IPv4 dotted quad, `localhost`, `*` for every interface, or an empty value for the loopback. Also `--bind` / `--host`. |
| `FSQL_MCP_HTTP_PATH` | `/mcp` | The path the JSON-RPC endpoint is served on. |
| `FSQL_MCP_HTTP_TLS` | `false` | Serve TLS on the HTTP transport. Needs `FSQL_MCP_TLS_CERT` and `FSQL_MCP_TLS_KEY`. |
| `FSQL_MCP_TLS_CERT` / `FSQL_MCP_TLS_KEY` | none | The server's certificate and its private key (PEM). |
| `FSQL_MCP_AUTH_TOKEN` | none | The deployment bearer token. The HTTP transport refuses to start without a credential. |
| `FSQL_MCP_AUTH_TOKEN_FILE` | none | A file holding the deployment token (its first line). Re-read when it changes on disk. |
| `FSQL_MCP_TOKEN_POLICY_FILE` | none | A JSON file of per-client credentials: SHA-256 digests, tool allowlists and expiry dates. |
| `FSQL_MCP_ALLOWED_ORIGINS` | none | Comma-separated browser origins allowed besides the loopback ones. |
| `FSQL_MCP_ALLOW_PLAINTEXT_NONLOOPBACK` | off | Accept a plaintext HTTP bind on a non-loopback address. |
| `FSQL_MCP_MAX_CONNECTIONS` | `256` | How many HTTP connections are served at once. |
| `FSQL_MCP_PROTOCOL_ERA` | `dual` | `dual` serves both protocol eras; `modern` serves the modern era only. |
| `FSQL_MCP_SESSION_TTL_SECS` | `3600` | How long an idle context handle survives. |
| `FSQL_MCP_MAX_SESSIONS` | `64` | How many context handles exist at once. Each one owns a Valkey connection. |
| `FSQL_MCP_MAX_ROWS_CEILING` | `1000` | The hard ceiling on the rows a tool may return. |
| `FSQL_MCP_KEY_PREFIX` | none | The prefix the keyspace tools write and delete under. Empty means any key in the keyspace. |
| `FSQL_MCP_REQUIRE_KEY_PREFIX` | `false` | Refuse to start unless `FSQL_MCP_KEY_PREFIX` is set. |
| `FSQL_MCP_KEY_OWNERSHIP` | `true` | Record which MCP client wrote each key and refuse another client's overwrite or delete. `off` for a deployment that deliberately shares keys between sessions. |
| `FSQL_MCP_READ_ONLY` | `false` | Refuse every tool annotated destructive before it reaches Valkey, and latch the module's destructive ledger commands for the connection. |
| `FSQL_MCP_REQUIRE_LEDGER_LATCH` | `false` | With read-only mode on, refuse the Valkey connection when the ledger latch cannot be applied instead of warning. |
| `FSQL_MCP_ALLOW_AUTO_EXECUTE` | off | Let a model ask for `auto_execute` on the tools that take it. Off by default: the parameter is controlled by the same actor the control protects against, so the operator decides. |
| `FSQL_MCP_CONFIG_WRITES` | `safe` | Which config keys a config-writing tool may write (`off`, `safe`, `all`). This engine exposes no config-writing tool, so the switch has no call site today. |
| `FSQL_MCP_TOOL_WHITELIST` / `--tool a,b` | all | Serve only these tools. |
| `FSQL_MCP_ENABLED_CATEGORIES` / `--category x,y` | all | Serve only these categories (`discovery`, `cognition`, `agency`, `analytics`, `enterprise`, `keyspace`, `admin`). |
| `FSQL_MCP_MAX_CONCURRENT_CHEAP` | `50` | How many cheap calls run at once. |
| `FSQL_MCP_MAX_CONCURRENT_EXPENSIVE` | `5` | How many model-backed calls run at once. |
| `FSQL_MCP_MAX_CONCURRENT_EXPENSIVE_PER_CLIENT` | `2` | How many model-backed calls one client may have in flight. |
| `FSQL_MCP_MAX_QUEUE_WAIT_MS` | `900000` | How long a call waits for a slot before `server busy`. The three concurrency caps are each a whole number of at least 1; anything else, including 0, makes the server refuse to start rather than hang every call. |
| `FSQL_MCP_SHUTDOWN_DRAIN_MS` | `5000` | How long shutdown waits for in-flight calls. |
| `FSQL_MCP_LOG_LEVEL` | `normal` | `verbose` adds debug lines. One JSON line per tool call on stderr, arguments redacted and capped. |
| `FSQL_MCP_SCHEMA_LOCK` | `mcp/.mcp-schema-lock` | The schema lock's path (also `--schema-lock-path`). |
| `FSQL_MCP_SAFETY_LOCK` | `mcp/.mcp-safety-annotations-lock` | The safety-annotation lock's path. |
| `FSQL_MCP_MANIFEST` | `mcp/manifest.json` | Where `--write-manifest` writes the manifest. |
| `FSQL_MCP_STRICT_SCHEMA_HASH` | `true` | Refuse to start if the catalog differs from the schema lock; `false` warns instead. |
| `FSQL_MCP_ALLOW_MISSING_LOCKS` | off | Run without the drift check when a lock file is missing (development only). Lock files are looked up in the working directory first, then next to the executable. |
| `FSQL_MCP_REQUIRE_LOCKS` | on | A missing lock file stops startup. `false` is the same as `FSQL_MCP_ALLOW_MISSING_LOCKS=true`. |
| `FSQL_MCP_VALKEY_CONNECT_TIMEOUT_MS` | `10000` | TCP connect. |
| `FSQL_MCP_VALKEY_TIMEOUT_MS` | `600000` | One reply. A model-backed agent can take minutes; size it for yours. |
| `FSQL_MCP_VALKEY_MAX_REPLY_MB` | `64` | Largest reply accepted. A server claiming more is refused and the connection dropped. |

The Valkey endpoint itself is configured with `VALKEY_HOST`, `VALKEY_PORT`, `VALKEY_SOCKET`, `VALKEY_USERNAME`, `VALKEY_PASSWORD`,
`VALKEY_PASSWORD_FILE`, `VALKEY_DB` and the `VALKEY_TLS*` set; the complete list is in [docs/mcp-plugin.md](../docs/mcp-plugin.md).

Every tool marked `destructiveHint: true` is reported with it, so a client that honours the annotation asks before calling one of them and read-only mode
refuses it. The rule is the annotation itself: every tool marked destructive is refused in read-only mode, and nothing else is. The set is larger than
the set that obviously writes: the agents are in it too, because a successful agent records its decision document in the
audit ledger. The annotations themselves are locked — `mcp/.mcp-safety-annotations-lock` holds the reviewed values, and a change to any of them stops the
server until the lock is regenerated on purpose. Each `tool_call` log line carries a short `client` label identifying who made the call: a digest of the
credential, or the session it belongs to.

## Audit data at rest

**What the audit trail stores.** Each agent decision is recorded as a decision document: the computed half of the answer and the model's written analysis
of it, as JSON. A call to the audit tool records the caller's own payload, and the module adds a server-assigned `_actor` block to it (the Valkey client id
and the ACL user); a call that came through this server also carries the `mcp_client` field naming the session or token that made it.

**Where it is stored.** Not in Valkey and not in this server. The decision ledger lives in the daemon, as a hash-chained file at the daemon's
`enterprise_ledger_path`, keyed by the daemon's `enterprise_ledger_key` when one is configured. Beside it the daemon writes a CSV mirror
(`<ledger>.csv`) and a head file (`<ledger>.head`) that records how many rows each chain kind ever had, so a truncated tail is visible from outside the
chain. All three are created mode `0600`. The chain proves that the records are in order and that none was changed or removed in the middle; with a key
it also proves who wrote them.

**Nothing is encrypted at the record level.** The chain key is a chain key, not a cipher: it proves authorship and integrity, and it does not encrypt the
record. The audit text — the question an agent was asked, the analysis it wrote, the rows it read — sits in the ledger file as it was written, and the same
is true of the keyspace the store tools write into.

**What protects it.** Confidentiality at rest comes from the storage layer, not from the ledger. The ledger file, the CSV mirror and the head file are
covered by volume encryption only — encrypt the disk, the volume or the backups they live on, and they are covered; there is no setting that encrypts a
record inside them, and this project does not encrypt records inside the ledger. The keyspace is covered by whatever your Valkey deployment offers:
upstream Valkey has no record-level encryption (its documented controls are TLS in transit, ACLs, network isolation and protected mode), while a managed
service may add its own — Amazon ElastiCache, for example, supports at-rest encryption on replication groups running Valkey 7.2 and later and enables it by
default for Valkey. Check the product's own documentation for the version you run before assuming it is on; where it is not, the keyspace is covered by the
encryption of the volume it lives on, like everything else. The MCP server's own bearer-token file holds a secret in the clear, so it belongs on the same
footing as the daemon's key file.

**Redaction.** `FSQL_AUDIT_REDACT` decides what the chain keeps of the audit text: `none` (the default) stores it as it is, `hash` stores
`sha256:<hex> len=<bytes>` in its place, and `truncate` keeps the first 64 characters. The redaction applies to the persisted copy only — the agent still
answers with the real question, the real SQL and the real analysis, and the values handed back to the caller are never redacted. For a deployment where
the trail has to be kept but the text may not be, `FSQL_AUDIT_REDACT=hash` is the setting to reach for: the ledger still shows that the same question was
asked twice and that the answer did not change, without recording what the question was.

**The residual risk.** Anyone who can read the ledger file, or the Valkey keyspace, can read the decisions. The protections that apply are the ones your
deployment provides: the Valkey ACL on the user this server connects as, the file permissions on the ledger, and the encryption of the volume they live on.
Treat anyone who can read them as able to read the decisions.

