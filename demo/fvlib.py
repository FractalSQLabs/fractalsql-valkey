# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""demo/fvlib.py -- the small shared client for the FractalSQL starter kits.

Python standard library only. Talks RESP to a Valkey server that has
fractalsql.so loaded. Vectors travel as packed little-endian float32 BLOBs, the
same format the module's corpus keys use.

    from fvlib import Kit
    k = Kit("quant-finance")          # parses --host/--port, connects
    k.section("1. Optimize")
    k.expect("the book holds at most 8 assets", nonzero <= 8)
    k.done()                           # exits 1 if any expectation failed
"""
import argparse
import json
import math
import random
import socket
import struct
import sys


class FvError(Exception):
    pass


class Conn:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=120)
        self.f = self.sock.makefile("rb")

    def call(self, *args):
        out = [b"*%d\r\n" % len(args)]
        for a in args:
            if isinstance(a, str):
                a = a.encode()
            elif isinstance(a, (int, float)):
                a = repr(a).encode()
            out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))
        return self._read()

    def _read(self):
        line = self.f.readline()
        if not line:
            raise ConnectionError("the server closed the connection")
        line = line.rstrip(b"\r\n")
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            raise FvError(rest.decode("utf-8", "replace"))
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            return self.f.read(n + 2)[:-2]
        if t == b"*":
            return [self._read() for _ in range(int(rest))]
        raise ValueError("bad RESP type %r" % t)


def pack(v):
    """The packed float32 blob of a vector, or of a list of rows.

    A corpus is a list of rows (each row a list of floats), so a caller
    hands over pack([[...], [...]]); the rows are flattened here, the
    same shape flat() gives, so a flat vector and a list of rows are both
    accepted and neither can be packed by mistake.
    """
    if v and isinstance(v[0], (list, tuple)):
        v = flat(v)
    return struct.pack("<%df" % len(v), *v)


def unpack(b):
    return list(struct.unpack("<%df" % (len(b) // 4), b))


def pack_u32(v):
    return struct.pack("<%dI" % len(v), *v)


def flat(rows):
    return [x for r in rows for x in r]


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def cosd(a, b):
    """Cosine distance. FRACTAL.SEARCH ranks by THIS, not by Euclidean distance."""
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    return 1.0 - sum(x * y for x, y in zip(a, b)) / (na * nb)


def noise(rng, n, sd=1.0):
    """n independent Gaussian samples."""
    return [rng.gauss(0, sd) for _ in range(n)]


def ramp(rng, n, onset, rate=1 / 40.0, sd=1.0):
    """White noise that develops an accelerating ramp from sample `onset`: a change in the
    series' dynamics, which is what FRACTAL.DIM.DRIFT detects (a constant offset is not)."""
    return [rng.gauss(0, sd) + (((i - onset) ** 2) * rate if i > onset else 0.0) for i in range(n)]


def walk(rng, n, sd=1.0):
    """A random walk (cumulative sum of noise): strongly persistent, DFA exponent near 1.5."""
    out, acc = [], 0.0
    for _ in range(n):
        acc += rng.gauss(0, sd)
        out.append(acc)
    return out


def meander(rng, n, step=0.15):
    """A 2-D random-walk trace, flattened (x, y, x, y, ...): a path that doubles back."""
    x = y = 0.0
    out = []
    for _ in range(n):
        x += rng.gauss(0, step)
        y += rng.gauss(0, step)
        out += [x, y]
    return out


def line(rng, n, jitter=0.002):
    """A near-straight 2-D trace, flattened: box-counting dimension close to 1."""
    out = []
    for i in range(n):
        out += [i * 0.1, i * 0.05 + rng.gauss(0, jitter)]
    return out


def unit(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


class Kit:
    def __init__(self, name, argv=None):
        ap = argparse.ArgumentParser(description="FractalSQL starter kit: " + name)
        ap.add_argument("--host", default="127.0.0.1")
        ap.add_argument("--port", type=int, default=6379)
        ap.add_argument("--quiet", action="store_true")
        a = ap.parse_args(argv)
        self.name, self.quiet, self.failed, self.checks = name, a.quiet, 0, 0
        self.c = Conn(a.host, a.port)
        self.rng = random.Random(sum(map(ord, name)))      # deterministic per kit
        try:
            self.c.call("FRACTAL.VERSION")
        except FvError:
            sys.exit("the fractalsql module is not loaded in that server")
        self.say("FractalSQL kit: %s" % name)

    def say(self, msg=""):
        if not self.quiet:
            print(msg)

    def section(self, title):
        self.say("\n-- " + title)

    def show(self, label, value):
        self.say("   %-44s %s" % (label, value))

    def call(self, *args):
        return self.c.call(*args)

    def js(self, *args):
        return json.loads(self.c.call(*args))

    def optional_js(self, label, *args):
        """Run a command that needs an optional piece (an embedding or reasoning provider behind
        fractalsqld). Returns its JSON, or None -- after saying so -- when that piece is not
        configured. Any other error is real and propagates."""
        try:
            return json.loads(self.c.call(*args))
        except FvError as e:
            if "not configured" in str(e) or "provider" in str(e) or "enterprise tier not loaded" in str(e):
                self.show(label, "skipped: needs a provider or the enterprise tier behind fractalsqld")
                return None
            if "dimensions but DIM is" in str(e):
                self.show(label, "skipped: the embedding model's dimension differs from this kit's small demo corpus (%s)" % str(e)[4:])
                return None
            raise

    def load(self, key, rows):
        """SET a corpus key from a list of equal-length float rows."""
        self.c.call("SET", key, pack(flat(rows)))

    def expect(self, what, ok, detail=""):
        self.checks += 1
        if not ok:
            self.failed += 1
        self.say("   [%s] %s%s" % ("ok" if ok else "FAIL", what, ("  (%s)" % detail) if detail and not ok else ""))

    def done(self):
        self.say("\n%s: %d check(s), %d failed" % (self.name, self.checks, self.failed))
        sys.exit(1 if self.failed else 0)
