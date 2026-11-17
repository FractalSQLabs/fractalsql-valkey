#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""The HTTP transport's handling of lookalike origins and repeated headers (SECURITY_AUDIT.md 3.17, 3.18).

usage: mcp_audit_http.py [MCP_BINARY]   (default: $MCP_SERVER_EXE, as the other MCP drivers)
No engine is needed: the checks are answered before any call reaches it.
  - an Origin that only starts with "127." (127.evil.example, 127.0.0.1.nip.io) is not loopback: 403; real loopback origins are not refused;
  - a header the server acts on that arrives twice (Host, Authorization, Origin, Content-Type, Mcp-Session-Id) is refused with 400;
  - /readyz answers the same way on every one of a burst of requests (the probe is cached, not run per request).
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.environ["MCP_SERVER_EXE"])
TOKEN = "t" * 32
failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else "  " + str(detail)[:200]))
    if not cond:
        failures.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def raw(port, head, body=b""):
    s = socket.create_connection(("127.0.0.1", port), 10)
    s.settimeout(10)
    s.sendall(head.encode() + body)
    data = b""
    try:
        while True:
            c = s.recv(65536)
            if not c:
                break
            data += c
            if b"\r\n\r\n" in data and b"content-length" in data.lower().split(b"\r\n\r\n")[0]:
                h, _, rest = data.partition(b"\r\n\r\n")
                n = int(h.lower().split(b"content-length:")[1].split(b"\r\n")[0])
                if len(rest) >= n:
                    break
    except OSError:
        pass
    s.close()
    try:
        return int(data.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return None


def post(port, extra, body=b'{"jsonrpc":"2.0","id":1,"method":"tools/list"}'):
    head = ("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n"
            % (TOKEN, extra, len(body)))
    return raw(port, head, body)


env = {k: v for k, v in os.environ.items() if not k.startswith("FSQL_MCP_")}
env["FSQL_MCP_AUTH_TOKEN"] = TOKEN
env.setdefault("SQLITE_DB_PATH", ":memory:")   # the SQLite server needs one; the others ignore it
port = free_port()
scratch = tempfile.mkdtemp(prefix="mcp-audit-http.")
proc = subprocess.Popen([binary, "--http", str(port)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                        stderr=open(os.path.join(scratch, "log"), "wb"), env=env, cwd=scratch)
try:
    up = False
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), 0.3).close()
            up = True
            break
        except OSError:
            time.sleep(0.05)
    check("the server starts", up)
    if up:
        for origin in ("http://127.evil.example", "http://127.0.0.1.evil.example", "http://127.0.0.1.nip.io:80",
                       "http://localhost.evil.example", "http://128.0.0.1", "http://1270.0.0.1"):
            check("Origin %s is refused (403)" % origin, post(port, "Origin: %s\r\n" % origin) == 403)
        for origin in ("http://127.0.0.1:3000", "http://127.1.2.3", "http://localhost:3000", "http://[::1]:5"):
            check("Origin %s is not refused as foreign" % origin, post(port, "Origin: %s\r\n" % origin) not in (403, None))
        for name in ("Host", "Authorization", "Origin", "Content-Type", "Mcp-Session-Id"):
            # 400 (ambiguous request); a server that judges the credential or the origin first answers 401 or 403: refused either way
            check("a repeated %s is refused (400, 401 or 403)" % name, post(port, "%s: a\r\n%s: b\r\n" % (name, name)) in (400, 401, 403))
        codes = {raw(port, "GET /readyz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n") for _ in range(20)}
        check("/readyz answers one way over a burst (%s)" % sorted(map(str, codes)), len(codes) == 1 and None not in codes)
        check("the server is still running", proc.poll() is None)
finally:
    proc.terminate()
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        proc.kill()
print(("ALL GREEN; " if not failures else "") + "%d failure(s)" % len(failures))
sys.exit(1 if failures else 0)
