<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Cognition API Reference

`FRACTAL.REASON` and `FRACTAL.EMBED` call out to a language model, so they run in the **`fractalsqld` daemon**, not in the Valkey process. A provider that stalls, a
plugin that crashes, or a model SDK that leaks can never take your datastore down: Valkey keeps serving every other command. See the
[FractalSQL Daemon](fractalsql-daemon.md) for running it and [Reasoning Setup](reasoning-setup.md) for provider configuration.

Both are optional. Without the `daemon-socket` and `daemon-key-file` load options they answer `the fractalsqld daemon is not configured`, and everything else is unaffected.

## Configuration: `fractalsqld.conf` preferred, environment fallback

Every reasoning and embedding setting belongs to the daemon, never to a Valkey command, so a credential cannot appear in `MONITOR` or the slowlog.

| `fractalsqld.conf` key (preferred) | Environment fallback | Purpose |
| --- | --- | --- |
| `reasoning_plugin` | `FRACTALSQL_REASONING_PLUGIN` | absolute path to the reasoning plugin (`fractalsql-reasoning-http.so`) |
| `reasoning_url` | `FRACTALSQL_HTTP_URL` | chat-completions endpoint, for `FRACTAL.REASON` |
| `reasoning_token` | `FSQL_REASONING_HTTP_TOKEN` | bearer or API-key token; plaintext in the config, which is therefore permission-gated (`chmod 600`) |
| `reasoning_model` | `FRACTALSQL_HTTP_MODEL` | chat model name |
| `embed_url` | `FRACTALSQL_HTTP_EMBED_URL` | embeddings endpoint, for `FRACTAL.EMBED`. No fallback to the chat URL: it is a different shape |
| `embed_model` | `FRACTALSQL_HTTP_EMBED_MODEL` | embedding model name |
| `reasoning_allow_plaintext` | `FRACTALSQL_HTTP_ALLOW_PLAINTEXT` | any non-empty value except `0` allows a non-TLS URL |
| `think`, `think_provider`, `think_native_url`, `think_num_ctx` | `FSQL_REASONING_HTTP_THINK*` | reasoning effort for hybrid-thinking models |

A bad value in the file refuses startup and reload with the key named in the daemon log. Provider-by-provider setup is in [reasoning-setup.md](reasoning-setup.md).

---

## `FRACTAL.REASON`
**LLM Dispatch**

Sends a natural-language query and an optional context payload to the configured reasoning provider and returns the model's reply.

### Signature

```
FRACTAL.REASON query [context_json]
```

- `query`: the text to send. A prompt over 4 MiB is refused cleanly.
- `context_json`: optional JSON passed to the model as the data to reason over, defaulting to `{}`. Put your data here, not credentials.

Returns the reply as a string.

```
127.0.0.1:6379> FRACTAL.REASON "reply with a one-word confirmation"
127.0.0.1:6379> FRACTAL.REASON "summarise this" "{\"rows\": [1, 2, 3]}"
```

### Reasoning Effort

A hybrid-thinking model's internal trace is throttled with `think` and `think_provider` in `fractalsqld.conf` (reloadable live). It applies to `FRACTAL.REASON` only;
embeddings ignore it. See [reasoning-setup.md](reasoning-setup.md#reasoning-effort).

---

## `FRACTAL.EMBED`
**Semantic Vector Generation**

### Signature

```
FRACTAL.EMBED text
```

Returns the text's embedding as a **packed float32 blob**, the same format the corpus keys use, so it drops straight into the vector commands:

```
embedding = FRACTAL.EMBED "reset my password"
FRACTAL.SEARCH corpus <embedding> 3           # use it as the query
FRACTAL.VECTOR.TOTEXT <embedding>             # "[0.1,0.2,0.3]"
```

It needs an embeddings endpoint (`embed_url` and `embed_model`). The result's length must match the corpus key you search; a different embedding model is a different key.

---

## Errors

| Error | Meaning |
| --- | --- |
| `the fractalsqld daemon is not configured ...` | the module was loaded without the daemon options |
| `fractalsqld is not reachable at PATH (...)` | the daemon is down; reasoning returns when it is back, with no Valkey restart |
| `fractalsqld did not answer (...)` / `reply failed authentication` | a timeout, or the module holds the wrong key file |
| `fractal_reason failed in the daemon (core_rc=N): is a reasoning provider configured ...` | the daemon answered but the provider failed; the daemon log has the detail |

## Retries

The daemon classifies each function by cost. `REASON` and `EMBED` are **expensive**: a
failure surfaces as an error and is never retried automatically, because a retry would
repeat a paid provider call. The pure read-only calls retry up to three times across a
dropped connection, and stateful calls (the ledger) resend once carrying the same
idempotency token so the daemon can replay rather than repeat.

## Concurrency

Each call occupies one of the module's worker threads for as long as the provider takes.
Size the `threads` load option for the reasoning concurrency you expect; eight clients
reasoning at once were verified with the default of four.
