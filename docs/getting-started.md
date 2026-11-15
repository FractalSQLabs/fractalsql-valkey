<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Getting Started: From Zero to Your First Agent

This guide takes you from a fresh checkout to a running FractalSQL module in about five minutes: no compiler needed for
the first path, no model download required to start. By the end you will have:

- a `valkey-server` with the `fractalsql` module loaded,
- a search and a diverse (Scout) search that run with **no model** connected,
- an **agent** making a decision from your own data,
- optionally, a live reasoning call against a real LLM through `fractalsqld`.

> **The 5-minute path:** [1. Running in 60 seconds](#1-running-in-60-seconds-docker)
> → [2. Your first search](#2-your-first-search-no-model-needed)
> → [3. Turn on reasoning](#3-turn-on-reasoning) → [4. Your first agent](#4-your-first-agent) → [where next](#where-next).

---

## 1. Running in 60 seconds (Docker)

```bash
./build.sh amd64                      # builds dist/amd64/fractalsql.so in a container (glibc 2.34)
docker compose up -d                  # the official valkey image with the module mounted
docker compose exec valkey valkey-cli FRACTAL.VERSION
```

`docker-compose.yml` mounts `dist/amd64/fractalsql.so` into the official
`valkey/valkey` image and starts it with `--loadmodule`. See the
[Docker Demo](docker-demo.md).

## 2. Your first search (no model needed)

Vectors travel as **packed little-endian float32 BLOBs**, in the corpus key and
in the query. The kits and the examples below use Python because it packs them
naturally; `FRACTAL.VECTOR.FROMTEXT` converts `"1,2,3"` or `"[1,2,3]"` to a BLOB
if you want to stay in `valkey-cli`.

```python
import socket, struct
def call(s, *args):                       # a minimal RESP client; any Valkey client works
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a.encode() if isinstance(a, str) else a
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    s.sendall(out); return s.makefile("rb").readline()

pack = lambda v: struct.pack("<%df" % len(v), *v)
# A corpus is rows of `dim` floats back to back, stored under any key:
#   SET corpus <rows>         then:   FRACTAL.SEARCH corpus <query> 3
```

```
127.0.0.1:6379> FRACTAL.SEARCH corpus <query-blob> 3
"{\"dim\":4,\"n_corpus\":8,\"best_fit\":0,\"best_point\":[...],\"top_k\":[{\"idx\":0,\"dist\":2.2e-16},...]}"
```

- **`FRACTAL.SEARCH`** (Sniper) returns the nearest rows, ranked by **cosine distance**.
- **`FRACTAL.EXPLORE`** (Scout) disperses a population across distinct regions near the
  query: coverage, not nearest-match. A bigger population reaches further afield.

Run a complete worked example, with no setup beyond a running server:

```bash
python3 demo/demo-vertical-recommendation-search.py --port 6379
```

## 3. Turn on reasoning

Reasoning and embeddings run in a separate process, `fractalsqld`, so a slow or
crashing model provider can never take your datastore down. See the
[FractalSQL Daemon](fractalsql-daemon.md) and [Reasoning Setup](reasoning-setup.md):

```bash
valkey-server --loadmodule /usr/lib/valkey/modules/fractalsql.so \
    daemon-socket /run/fractalsql/fractalsqld.sock daemon-key-file /etc/fractalsql/hmac.key
valkey-cli FRACTAL.REASON "In one sentence, what is a vector?"
```

Without those two options everything else works and the reasoning commands answer
with a clear "not configured" error.

## 4. Your first agent

An agent is a command that reads your keys, computes a result with the module's primitives, and (when a reasoner is
configured) writes a short read of it. Store a metric series under a key and ask for a triage:

```python
import random, struct
pack = lambda v: struct.pack("<%df" % len(v), *v)
noise = [random.gauss(0, 1) for _ in range(96)]
ramp = [x + (max(0, i - 48) ** 2) / 40.0 for i, x in enumerate(noise)]       # a series that changes regime halfway through
call(s, "SET", "host:17:latency", pack(ramp))
call(s, "FRACTAL.AGENT.ANOMALY_TRIAGE", "host:17:latency", "32")
```

```
> FRACTAL.AGENT.ANOMALY_TRIAGE host:17:latency 32
{"threat_score":1.2653238663,"anomaly_type":"vector_drift","triage_summary":null,"reasoning_step_failed":"the fractalsqld daemon is not configured"}
```

`threat_score` is a real number you can act on: it is the drift `FRACTAL.DIM.DRIFT` reports for the same series. With no reasoner configured
the agent still returns it, and says why `triage_summary` is empty. Configure one ([step 3](#3-turn-on-reasoning)) and the same call also
returns a one-sentence triage. Which agent should you use for your problem? See the
**[decision table in api-agency.md](api-agency.md#which-agent-should-i-use)**, then run a complete worked example:

```bash
python3 demo/demo-vertical-agentic-ops-devops.py --port 6379
```

## 5. Install without Docker

### Option A: one command

```bash
# Linux or macOS
curl -fsSL https://github.com/FractalSQLabs/fractalsql-valkey/releases/latest/download/easy_install.sh | bash

# or, from a clone:
./scripts/easy_install.sh
```

It installs the package for your platform (`apt` or `dnf` on Linux; the unsigned macOS archive is unpacked and its Gatekeeper quarantine flag
cleared), writes a `loadmodule` snippet, and tells you how to use it. It never restarts Valkey. Useful flags:

| Flag | Meaning |
| --- | --- |
| `--conf /etc/valkey/valkey.conf` | also add `include <snippet>` to that server config |
| `--threads N` | worker threads for the module |
| `--daemon-socket PATH --daemon-key-file PATH` | point the module at a running `fractalsqld` (reasoning, embeddings, the ledger) |
| `--from-file PATH` | install a package you already downloaded (air-gapped machines) |
| `--version X.Y.Z` | a specific release |
| `--dry-run` | print what would happen, change nothing |
| `--yes` | answer every prompt yes (CI and other non-tty runs) |
| `--uninstall` | remove what it installed |

### Option B: manual or air-gapped

| Platform | How |
| --- | --- |
| Debian / Ubuntu | `sudo apt install ./fractalsql-valkey-amd64.deb`, then `loadmodule /usr/lib/valkey/modules/fractalsql.so` (or `include /etc/valkey/modules-available/fractalsql.conf`) |
| RHEL 9+ / Fedora | `sudo dnf install ./fractalsql-valkey-amd64.rpm` |
| Alpine | `sudo apk add --allow-untrusted ./fractalsql-valkey-musl-amd64.apk` |
| macOS | unzip `fractalsql-valkey-osx_arm64.zip` (or `osx_amd64`), run `xattr -dr com.apple.quarantine fractalsql.dylib`, then `MODULE LOAD /path/to/fractalsql.dylib` (the module needs nothing else; the optional MCP server in the zip's `mcp/` folder uses Homebrew's OpenSSL: `brew install openssl@3`) |
| Live (no restart) | `valkey-cli MODULE LOAD /path/to/fractalsql.so` (not persistent) |

Then check it:

```bash
valkey-cli MODULE LIST
valkey-cli FRACTAL.VERSION
valkey-cli FRACTAL.EDITION          # Community
```

The floor is **glibc 2.34** (Ubuntu 22.04+, Debian 12+, RHEL 9+). Platforms are
Linux and macOS, amd64 and arm64.

Load-time options (all optional): `threads N` (worker threads, default 4),
`max-inflight-mb M` (cap on decoded input held by running jobs, default 1024),
`daemon-socket PATH`, `daemon-key-file PATH`, `daemon-timeout-s N`,
`max-sessions N` (live diversify sessions, default 64), `session-ttl-sec N` (idle sessions are dropped after this, default 3600, 0 = never),
`agent-partial-reasoning-fail yes|no`.

## Where next

- **Apply it to your industry** → [Starter Kits](starter-kits.md): forty-four runnable, self-checking scripts.
- **Every command and its arguments** → [Discovery](api-discovery.md), [Analytics](api-analytics.md), [Cognition](api-cognition.md).
- **Run an agent** → [Agency](api-agency.md): twenty-one commands that decide from your keys.
- **Compose your own** → [Composition Guide](composition-guide.md).
- **Prove what an agent decided** → [Enterprise Tier](enterprise.md).
