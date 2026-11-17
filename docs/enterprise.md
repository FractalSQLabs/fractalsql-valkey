<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Enterprise Tier

Everything else in this documentation is the Community edition and is fully functional
on its own. For regulated environments that need to **prove, not just claim**, what an
autonomous process decided and why, an optional drop-in library adds a tamper-evident,
hash-chained decision ledger. It runs in the [daemon](fractalsql-daemon.md), never in
the Valkey process: the licensed library and the ledger's HMAC key stay out of your
datastore.

## Activating it

In `fractalsqld.conf` (or the matching `FRACTALSQL_ENTERPRISE_*` environment variables):

```ini
enterprise_lib = /opt/fractalsql/libfractalsql-enterprise-sovereign-c.so
enterprise_ledger_path = /var/lib/fractalsql/ledger.dat
enterprise_ledger_key = <hex>          # optional; unset means structural-only validation
enterprise_require_signature = on      # the default: refuse a library with no detached .sig
```

No rebuild and no module reload. On a Community daemon every command below answers
`the enterprise tier is not loaded in fractalsqld`, which is the intended dormant path,
except `FRACTAL.LEDGER.LOCK` (a session latch that works without the library) and
`FRACTAL.LEDGER.VERIFY`, which is read-only and works without it: with no ledger it
reports `{"ok":true,"rows_verified":0}`.

## Commands

| Command | Purpose |
| --- | --- |
| `FRACTAL.LEDGER.FLUSH` | encode the truth and shadow ledgers into a blob and append it to the chain |
| `FRACTAL.LEDGER.LOAD` | hydrate the in-memory ledgers from the chain's latest entry (verifies only the tip) |
| `FRACTAL.LEDGER.VERIFY [kind]` | walk the **entire** chain and return `{"ok":true,"rows_verified":N}` or `{"ok":false,...}` |
| `FRACTAL.LEDGER.COMPACT` · `RESET.SOFT` · `RESET.HARD` | maintenance |
| `FRACTAL.LEDGER.TRUTH.COUNT` · `SHADOW.COUNT` | entry counts |
| `FRACTAL.LEDGER.LOCK` | a one-way latch for this client: afterwards flush, compact and the resets are refused (load still works) |
| `FRACTAL.AUDIT.LOG entry_type payload_json` | append a record to the general decision-audit chain (kind 2); verify it with `LEDGER.VERIFY 2` |
| `FRACTAL.AUDIT.UNPACK blob` | decode a persisted blob into its audit JSON |

The session identity behind the lock and the per-client ledger context is the Valkey
client id.

## What the chain can and cannot prove

It proves that the records are in order, that none was changed or removed in the middle,
and, with a key, that they were written by someone holding the key. It does not prove
that a decision was *correct*. The loss of the newest records, when the file is truncated
at a record boundary, is invisible to the chain itself -- there is nothing after the last
record to disagree with it -- but it is not invisible to the **head file** next to the
ledger, which counts the rows each chain ever had (see [Data at rest](#data-at-rest)). Back
the ledger file up like any audit log, and anchor the latest record's hash somewhere
external if truncation matters to you.

In practice: a flushed ledger verifies as a one-row chain; a two-record decision chain verifies as two rows; the ledger
reloads across a daemon restart; and flipping one byte makes `VERIFY` report `ok:false` and `LOAD` refuse the tampered record.

## Data at rest

The decision ledger is a set of files, and they hold what the agents were asked and what
they answered. This is where that text lives, and what protects it.

| What | Where | Notes |
| --- | --- | --- |
| The ledger itself | `enterprise_ledger_path` (default `fractalsql_ledger.dat`) | the authoritative hash chain, append-only between maintenance operations |
| The CSV mirror | `<ledger>.csv` | a queryable copy of the same records; a read surface, not a second source of truth |
| The head file | `<ledger>.head` | how many rows each chain ever had, so a truncated tail is visible; carries an HMAC of its own when a ledger key is set |
| The daemon's own settings | `fractalsqld.conf`, the HMAC key file | the key file holds the key that authenticates the daemon's replies to the module |
| Your data | the Valkey keyspace | the corpus, the vectors and the sessions |

The ledger file, the CSV mirror and the head file are created mode `0600` -- owner read and
write, nothing for the group or the world -- and an existing file with wider permissions is
tightened to the same on the next write.

**Confidentiality at rest is the volume's job, not the ledger's.** Valkey has no record-level
encryption: there is no setting that encrypts a key, a value or a ledger record inside the
database, and this project does not encrypt the records inside the ledger either. The
ledger's HMAC key buys integrity, not secrecy -- it proves a record was written by someone
holding the key and that it has not been changed since; it does not hide what the record
says. So:

- Encrypt the volume (or the disk, or the backups) and the ledger file, the CSV mirror and the
  head file are covered by that encryption.
- With the volume unencrypted, anyone who can read those files can read the audit text: the
  questions, the generated SQL, the analyses and the result rows are stored verbatim unless
  you ask for redaction.

### Redacting the stored text

`FSQL_AUDIT_REDACT` decides what the chain keeps of the audit text:

| Value | The chain stores |
| --- | --- |
| `none` (the default) | the text as it is |
| `hash` | `sha256:<hex> len=<bytes>` -- a fingerprint you can compare against, not the text |
| `truncate` | the first 64 characters |

It applies to the persisted copy only: the agent still answers with the real question, the
real SQL and the real analysis, and the values handed back to the caller are never redacted.
The redaction works by field name, so the fields that carry free text (the question, the
generated SQL, the analysis, the reason, the result rows) are the ones that are replaced, while
the fields that carry no text (identifiers, counts, statuses, flags) are left exactly as they
were. The stored record stays valid JSON either way.

With `hash` the ledger still shows that the same question was asked twice and that the answer
did not change, without showing what the question was. That is the trade: less detail in the
trail, in exchange for a trail you can keep where the text may not.

### The head file

The head file is a small text file next to the ledger, with one record per chain kind:

```
kind=1 rows=12 mac=<64 hex digits>
kind=2 rows=3
```

The row count is the number of rows that kind was ever appended to, so `FRACTAL.LEDGER.VERIFY`
can tell that the chain now holds fewer rows than were written -- which is what a truncated
tail looks like from the outside. The MAC is an HMAC-SHA256 over the record's own content,
keyed with the **same key** as the ledger's records (`enterprise_ledger_key`), so it shares
that key's handling: whoever holds the key can rewrite the head file as plausibly as they can
rewrite a ledger record. With no key configured the head records carry no MAC at all, and a
head file written that way is recognised as predating the key when a key is later set.

The head file is rewritten atomically on every append, and it is derived data: it can be
deleted and rebuilt from the chain, at the cost of losing the tail-detection it provides.

## Enterprise optimizers

The enterprise library also provides multimodal portfolio optimization: instead of one answer, several structurally different ones.

| Command | Purpose |
| --- | --- |
| `FRACTAL.OPTIMIZE.MULTIMODAL mu cov k n_restarts overlap_threshold quality_frac [seed]` | diverse portfolios, best Sharpe first |
| `FRACTAL.OPTIMIZE.MULTIMODAL.PARETO mu cov k n_restarts [max_front [seed]]` | the non-dominated return-versus-risk front |
| `FRACTAL.AGENT.DIVERSE_PORTFOLIOS mu cov k [options]` | the same, with one reasoned comparison of the candidates |

Arguments, ranges and replies are in [api-analytics.md](api-analytics.md#fractaloptimizemultimodal-and-fractaloptimizemultimodalpareto-enterprise) and
[api-agency.md](api-agency.md). On a Community daemon each answers `enterprise tier not loaded`. **Every call is also written to the decision ledger as an
audit record**, so a run of optimizations is part of the chain you can verify (`FRACTAL.LEDGER.VERIFY 2`).

### Configuration and live reload

The enterprise keys (`enterprise_lib`, `enterprise_ledger_path`, `enterprise_ledger_key`, `enterprise_require_signature`) live in `fractalsqld.conf` and reload
live with `fsqlctl reload`, with two exceptions: `enterprise_lib` cannot be changed while a library is already loaded (restart the daemon), and a change of
`enterprise_ledger_path` is yours to reconcile, because the ledger file opens per operation and the new path starts its own chain. See the
[FractalSQL Daemon](fractalsql-daemon.md#provider-keys).

### External anchoring (closing the truncation gap)

A hash chain cannot show that its **newest** records were cut off the end of the file. Close that gap by publishing the ledger's state somewhere its
administrator cannot rewrite. The ledger file is append-only between maintenance operations, so a size and a hash are enough:

```bash
f=/var/lib/fractalsql/ledger.dat
size=$(stat -c %s "$f"); hash=$(sha256sum "$f" | cut -d' ' -f1)
echo "anchor size=$size sha256=$hash at=$(date -u +%FT%TZ)" | logger -t fractalsql-anchor      # or an object-lock bucket, or a compliance mailbox
```

Run it on a schedule (cron or a systemd timer). To verify an anchor later, hash the same number of leading bytes and compare:

```bash
head -c "$size" "$f" | sha256sum        # must equal the anchored hash
```

Then run `FRACTAL.LEDGER.VERIFY` to check the chain itself. A mismatch means the file was altered, or rewound past the anchor, after it was taken. Where
the record is published is what satisfies the guarantee: plain redirection to a file the same administrator controls does not. `FRACTAL.LEDGER.COMPACT` and
`RESET.*` rewrite the file, so take a fresh anchor after one.

### Detached signature verification

Loading a library only proves it has the right function names; a tampered file with the same names would load. The daemon therefore
requires a valid detached Ed25519 signature (a `<path>.sig` file of 64 raw bytes) against a fixed FractalSQLabs public key before the library is loaded. This
is on by default: a library with no `.sig` is refused. Set `enterprise_require_signature = off` only for a development build you trust, and the missing `.sig`
is then logged and the library still loads. An **invalid** signature (present but wrong) is always refused, and a library that cannot be read for verification
is refused while the setting is on. New releases only need a fresh signature from the same key.

---

For enterprise editions, licensing and support, contact FractalSQLabs.

## Destructive ledger operations are off by default

`fractal_ledger_compact`, `fractal_ledger_reset_soft` and `fractal_ledger_reset_hard` destroy ledger records, so they refuse unless the
operator enables them by setting this in the environment of the `fractalsqld` process:

```sh
FRACTALSQL_LEDGER_ALLOW_DESTRUCTIVE=1
```

Without it, a server nobody configured cannot have its audit chain destroyed by a command from any account that may call the function.
`fractal_ledger_flush` (sealing the ledgers into the chain) is routine and is not affected, and the one-way `fractal_lock_destructive`
latch still wins when it is set. Each refusal is written to the server's log. Where the database has its own privileges, keep restricting
who may execute these functions as well.

This default changed: earlier builds allowed these operations unless the session was latched.
