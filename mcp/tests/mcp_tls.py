#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs

"""The MCP server's TLS connection to Valkey (VALKEY_TLS and the VALKEY_TLS_* settings).

A TLS-terminating proxy built on the stdlib `ssl` module sits in front of the plain Valkey the gate started, with a
throwaway certificate for 127.0.0.1 / localhost, so no TLS-enabled Valkey build is needed. For each case the server
is started with the settings under test and either reaches Valkey (a tools/call answers) or refuses at startup with a
clear message; it must never hang and never fall back to plaintext.

  success   a trusted CA; VALKEY_TLS_INSECURE; a client certificate against a proxy that requires one
  refusal   an untrusted certificate; the wrong server name; no client certificate against mTLS; TLS spoken to a
            plain port; a plain client against the TLS port
  config    VALKEY_TLS values and combinations that must exit 2

  MCP_SERVER_EXE   the server binary (required)      VALKEY_HOST/VALKEY_PORT   the plain Valkey behind the proxy
Zero dependencies beyond the stdlib and the openssl command line tool."""

from __future__ import annotations

import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mcp_robustness as rb  # noqa: E402

_FAILURES = 0


def check(label, cond, note=""):
    global _FAILURES
    print(("PASS " if cond else "FAIL ") + label + ((" -- " + note) if (note and not cond) else ""), flush=True)
    if not cond:
        _FAILURES += 1
    return bool(cond)


def _gen_cert(d, name, san):
    key, crt = os.path.join(d, name + ".key"), os.path.join(d, name + ".crt")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost",
                    "-addext", "subjectAltName=" + san, "-keyout", key, "-out", crt],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return key, crt


class Proxy:
    """TLS in front of a plain TCP backend. require_client: the CA file clients must be signed by."""

    def __init__(self, key, crt, backend, require_client=None):
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(crt, key)
        if require_client:
            self.ctx.verify_mode = ssl.CERT_REQUIRED
            self.ctx.load_verify_locations(require_client)
        self.backend = backend
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(16)
        self.port = self.srv.getsockname()[1]
        self.stop = False
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, raw):
        try:
            c = self.ctx.wrap_socket(raw, server_side=True)
            b = socket.create_connection(self.backend, timeout=10)
        except Exception:  # noqa: BLE001
            try:
                raw.close()
            except OSError:
                pass
            return

        def pump(src, dst):
            try:
                while True:
                    d = src.recv(65536)
                    if not d:
                        break
                    dst.sendall(d)
            except Exception:  # noqa: BLE001
                pass
            for s in (src, dst):
                try:
                    s.close()
                except Exception:  # noqa: BLE001
                    pass
        threading.Thread(target=pump, args=(c, b), daemon=True).start()
        threading.Thread(target=pump, args=(b, c), daemon=True).start()

    def close(self):
        self.stop = True
        self.srv.close()


def attempt(exe, env):
    """Starts the server with `env`; returns (reached, text): reached means a version call answered."""
    errf = tempfile.TemporaryFile("w+")
    s = rb._Session(exe, os.getcwd(), env, stderr=errf)
    reached, text = False, ""
    try:
        try:
            rb._init(s)
            r = s.call("tools/call", {"name": "fractal_version", "arguments": {}}, timeout=30)
            reached = "result" in r and not r["result"].get("isError")
            text = str(r)[:300]
        except Exception as e:  # noqa: BLE001
            text = "no answer: %r" % (e,)
    finally:
        rc = s.close()
    errf.seek(0)
    return reached, rc, text + " | " + errf.read()[-400:]


def run():
    exe = os.environ["MCP_SERVER_EXE"]
    backend = (os.environ.get("VALKEY_HOST", "127.0.0.1"), int(os.environ.get("VALKEY_PORT", "6379")))
    if not shutil.which("openssl"):
        print("SKIP tls: no openssl command line tool")
        return
    d = tempfile.mkdtemp(prefix="fsqlmcp_tls_")
    skey, scrt = _gen_cert(d, "server", "IP:127.0.0.1,DNS:localhost")
    ckey, ccrt = _gen_cert(d, "client", "DNS:client")
    okey, ocrt = _gen_cert(d, "other", "IP:127.0.0.1,DNS:localhost")   # a certificate the client does not trust
    p = Proxy(skey, scrt, backend)
    m = Proxy(skey, scrt, backend, require_client=ccrt)
    base = {"VALKEY_HOST": "127.0.0.1", "VALKEY_PORT": str(p.port), "VALKEY_TLS": "true"}

    ok, rc, t = attempt(exe, {**base, "VALKEY_TLS_CA_FILE": scrt})
    check("tls: a server certificate signed by the configured CA is accepted and a call is answered", ok, t)
    ok, rc, t = attempt(exe, {**base, "VALKEY_TLS_CA_FILE": scrt, "VALKEY_TLS_SERVER_NAME": "localhost"})
    check("tls: VALKEY_TLS_SERVER_NAME matching the certificate's name is accepted", ok, t)
    ok, rc, t = attempt(exe, {**base, "VALKEY_TLS_INSECURE": "true"})
    check("tls: VALKEY_TLS_INSECURE connects without a CA", ok, t)
    ok, rc, t = attempt(exe, {**base})
    check("tls: an unknown certificate is refused by default (not in the system trust store)", not ok and "verify" in t.lower(), t)
    ok, rc, t = attempt(exe, {**base, "VALKEY_TLS_CA_FILE": ocrt})
    check("tls: a certificate that the configured CA did not sign is refused", not ok and "verify" in t.lower(), t)
    ok, rc, t = attempt(exe, {**base, "VALKEY_TLS_CA_FILE": scrt, "VALKEY_TLS_SERVER_NAME": "wrong.example"})
    check("tls: a server name the certificate does not carry is refused", not ok, t)
    mb = {**base, "VALKEY_PORT": str(m.port), "VALKEY_TLS_CA_FILE": scrt}
    ok, rc, t = attempt(exe, mb)
    check("tls: a proxy that requires a client certificate refuses a client without one", not ok, t)
    ok, rc, t = attempt(exe, {**mb, "VALKEY_TLS_CERT_FILE": ccrt, "VALKEY_TLS_KEY_FILE": ckey})
    check("tls: the client certificate and key are presented and accepted", ok, t)
    ok, rc, t = attempt(exe, {**base, "VALKEY_PORT": str(backend[1]), "VALKEY_HOST": backend[0], "VALKEY_TLS_CA_FILE": scrt})
    check("tls: TLS spoken to a plain port fails cleanly, not a hang", not ok and rc is not None, t)
    ok, rc, t = attempt(exe, {"VALKEY_HOST": "127.0.0.1", "VALKEY_PORT": str(p.port)})
    check("tls: a plain client against the TLS port fails cleanly and never falls back", not ok and rc is not None, t)

    for label, env in (("VALKEY_TLS=maybe", {"VALKEY_TLS": "maybe"}),
                       ("VALKEY_TLS_INSECURE=1", {"VALKEY_TLS": "true", "VALKEY_TLS_INSECURE": "1"}),
                       ("a TLS setting without VALKEY_TLS", {"VALKEY_TLS_CA_FILE": scrt}),
                       ("a client certificate without its key", {"VALKEY_TLS": "true", "VALKEY_TLS_CERT_FILE": ccrt}),
                       ("VALKEY_TLS with VALKEY_SOCKET", {"VALKEY_TLS": "true", "VALKEY_SOCKET": "/tmp/none.sock"})):
        r = subprocess.run([exe, "--version"], env={**os.environ, **env}, capture_output=True, text=True, timeout=30)
        r2 = subprocess.run([exe], env={**os.environ, **env}, input="", capture_output=True, text=True, timeout=30)
        check("tls: config error exits 2 -- " + label, r2.returncode == 2, "rc=%s %s" % (r2.returncode, r2.stderr[-200:]))
    p.close()
    m.close()
    shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    run()
    print("mcp-tls: %s; %d failure(s)" % ("ALL GREEN" if not _FAILURES else "FAILED", _FAILURES))
    sys.exit(1 if _FAILURES else 0)
