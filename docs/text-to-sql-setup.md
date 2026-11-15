<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# Text-to-SQL

Text-to-SQL turns a natural-language question into a validated `SELECT` over your schema and runs it. It needs a SQL engine to read the
schema from, to check the generated statement against, and to execute it in. **Valkey has no SQL engine, so FractalSQL for Valkey does not
provide text-to-SQL** (`fractal_text_to_sql`, `fractal_sql_agent`, `fractal_schema_context`, or the `DATA_ANALYST` agent built on them). This
page keeps the layout of the SQL editions' page, and says for each part what applies on Valkey instead.

## Quick Start

You usually do not need SQL to answer a question over your data by meaning. Embed the question and search your corpus:

```
embedding = FRACTAL.EMBED "how many orders does acme have?"
FRACTAL.SEARCH orders:corpus <embedding> 5
```

See the `semantic-cache` and `agentic-recall-recommendation` kits. To turn a question into a **command** instead of SQL, ask the model through
`FRACTAL.REASON` with your command set as context, and validate the reply in your application against an allow-list **before** it reaches
Valkey. Never pass a model's output straight to the server.

### Scoping the Context

Each corpus key is its own scope, and `FRACTAL.REASON` takes the context you give it as JSON (up to 64 KiB) and nothing else: no schema is read
for you, so nothing leaves the server that you did not put in the call. To restrict what a retrieval sees, search a cohort key, or use the agents'
`FILTER hash value` option ([api-agency.md](api-agency.md#a-note-on-id-resolution)).

## The Safety Pipeline: How it Works

The SQL editions generate, allow-list, optionally review, and parse-check a statement before returning it, and feed each failure back for a
retry. On Valkey the same idea applies to any model-written command, and the checks are yours to make:

1. **Generate** with `FRACTAL.REASON`, naming the commands you will accept.
2. **Allow-list** the reply: exactly one command, from a short list of read-only commands, with arguments you can validate.
3. **Run it as a restricted user**, so the server itself enforces the limit (see below).
4. **Cap the result** you hand back to the model or the user.

### Execution on a restricted user

Valkey's own ACL is the strongest guardrail available. Create a user that can only read what the question needs, and run the generated command as
that user:

```
ACL SETUSER nlq on >a-long-secret ~report:* -@all +get +hget +hgetall +zrange +zcard
```

A command outside that list, or a key outside `report:*`, is refused by the server whatever the model wrote.

## Schema context

`fractal_schema_context` describes tables and columns for the model. Valkey keys have no schema; if you want to tell a model what is in a keyspace,
describe your key patterns and their types yourself (`SCAN`, `TYPE`), and decide deliberately what to include, because it is sent to the provider.

## Configuration & Security

There is nothing to configure: no generation limits, statement allow-list or review step exist on the Valkey side. The reasoning provider
itself is configured in `fractalsqld.conf` ([reasoning-setup.md](reasoning-setup.md)), and its credentials are never settable through a command.

### Secure it: Authorization vs. Correctness

An allow-list improves correctness; it is not authorization. The boundary is the ACL user above, with key patterns and a command list that
cannot do harm even if the model's output is wrong or malicious. Treat anything a model writes, and anything it reads from your data, as untrusted.

## Validation & Testing

Nothing to validate on the server. If you build command generation in your application, test it with hostile model replies (several commands,
a write, a script, an oversized argument) and confirm the restricted user refuses each.

## Known Limitations

- No SQL, no schema introspection, no generated-query execution.
- For question answering, `FRACTAL.AGENT.DATA_ANALYST` is the Valkey form: instead of SQL the model writes a valkey-search filter, which the module
  runs under your own ACL (see [Agency](api-agency.md#data-analyst-fractalagentdata_analyst)). For anything else, compose retrieval and
  `FRACTAL.REASON` yourself, as the [composition guide](composition-guide.md) shows.

If you need text-to-SQL with its allow-list, parse check and never-executes-unvalidated pipeline, use a
FractalSQL SQL edition.
