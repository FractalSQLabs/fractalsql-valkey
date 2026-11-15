# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# tests/fake_daemon.py: a scripted stand-in for fractalsqld, for the module's daemon client (src/fv_daemon.c).
#
# It speaks the FSQ wire protocol (service/protocol/SPEC.md): frames with an HMAC-SHA256 tag, UDF_INIT / UDF_CALL / UDF_DEINIT. The real
# daemon always answers correctly, so the client's handling of a daemon that does not -- a reply cut short, a bad tag, a wrong request id,
# an oversized length, a status the client has to map, a result body that is too short or lies about its length, a daemon that never
# answers -- can only be driven by a double. `mode` selects what the next replies do; `results` gives each function its answer in the
# modes that answer. Python standard library only.

import hashlib
import hmac
import os
import socket
import struct
import threading
import time

MAGIC = b"FSQ1"
HDR = struct.Struct("<4sHHIIQII")          # magic version opcode flags request_id context auth_len payload_len
OP_INIT, OP_CALL, OP_DEINIT = 0x0010, 0x0011, 0x0012
T_STRING, T_INT, T_REAL = 1, 2, 3

DEFAULT_RESULTS = {
    "fractal_embed": ("s", "[0.1,0.2,0.3]"),
    "fractal_reason": ("s", "A reasoned\tanswer\rwith controls\n\"quoted\" \\ \x01 \u00e9"),
    "fractal_generate": ("s", "```\nFT.SEARCH idx '*'\n```"),
    "fractal_text_to_sql": ("s", "SELECT 1"),
    "fractal_ledger_verify": ("s", '{"ok":true,"rows_verified":3}'),
    "fractal_ledger_truth_count": ("i", 3),
    "fractal_ledger_shadow_count": ("i", 1),
    "fractal_optimize_portfolio_multimodal": ("s", '{"n_found":2,"candidates":[{"sharpe":1.2,"weights":[0.5,0.5]},{"sharpe":1.1,"weights":[0.6,0.4]}]}'),
    "fractal_optimize_portfolio_multimodal_pareto": ("s", '{"n_found":2,"candidates":[{"return":0.1,"risk":0.2,"weights":[0.5,0.5]},{"return":0.2,"risk":0.3,"weights":[0.6,0.4]}]}'),
}


def tlv(kind, value):
    if kind == "n":                                 # a NULL result
        return bytes([T_STRING, 1]) + struct.pack("<I", 0)
    if kind == "i":
        return bytes([T_INT, 0]) + struct.pack("<I", 8) + struct.pack("<q", value)
    if kind == "r":
        return bytes([T_REAL, 0]) + struct.pack("<I", 8) + struct.pack("<d", value)
    b = value.encode() if isinstance(value, str) else value
    return bytes([T_STRING, 0]) + struct.pack("<I", len(b)) + b


class FakeDaemon:
    def __init__(self, sock_path, key_hex):
        self.path = sock_path
        self.key = bytes.fromhex(key_hex)
        self.mode = "ok"
        self.results = dict(DEFAULT_RESULTS)
        self.frames = 0
        self.stop = False
        try:
            os.unlink(sock_path)
        except OSError:
            pass
        self.srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.srv.bind(sock_path)
        self.srv.listen(64)
        self.srv.settimeout(0.2)
        self.th = threading.Thread(target=self._accept, daemon=True)
        self.th.start()

    def close(self):
        self.stop = True
        self.th.join(timeout=2)
        self.srv.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass

    def _accept(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    @staticmethod
    def _read(c, n):
        out = b""
        while len(out) < n:
            chunk = c.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def _tag(self, hdr, payload):
        return hmac.new(self.key, hdr + payload, hashlib.sha256).digest()

    def _serve(self, c):
        c.settimeout(30)
        handles = {}
        try:
            while not self.stop:
                hdr = self._read(c, 32)
                magic, ver, op, flags, rid, ctx, alen, plen = HDR.unpack(hdr)
                self._read(c, 32)
                payload = self._read(c, plen) if plen else b""
                self.frames += 1
                mode = self.mode
                if mode == "stall":
                    time.sleep(8)
                    return
                if mode == "close_before":
                    return
                status, body = 0, b""
                if op == OP_INIT:
                    nl = payload[0]
                    name = payload[1:1 + nl].decode()
                    o = 1 + nl
                    nargs = struct.unpack_from("<I", payload, o)[0]
                    o += 4
                    types = []
                    for _ in range(nargs):
                        t, _n, ln = struct.unpack_from("<BBI", payload, o)
                        types.append(t)
                        o += 6 + ln
                    h = len(handles) + 1
                    handles[h] = name
                    body = struct.pack("<QIBI", h, 1 << 20, 1, nargs) + bytes(types)
                    if mode == "init_short":
                        body = body[:10]
                    elif mode == "init_status":
                        status, body = 1, b"init refused: bad arguments"
                elif op == OP_CALL:
                    nl = payload[0]
                    name = payload[1:1 + nl].decode()
                    kind, value = self.results.get(name, ("s", "ok"))
                    if mode == "null_result":
                        body = tlv("n", None)
                    elif mode == "type_int":
                        body = tlv("i", 7)
                    elif mode == "type_real":
                        body = tlv("r", 0.5)
                    elif mode == "call_short":
                        body = b"\x01\x00\x00"
                    elif mode == "call_badlen":
                        body = bytes([T_STRING, 0]) + struct.pack("<I", 9999) + b"abc"
                    elif mode == "empty_string":
                        body = tlv("s", "")
                    elif mode == "garbage_embed":
                        body = tlv("s", "not a vector")
                    elif mode == "nan_embed":
                        body = tlv("s", "[0.1,nan,0.3]")
                    elif mode == "big_embed":
                        body = tlv("s", "[" + ",".join("0.5" for _ in range(20000)) + "]")
                    elif mode == "bad_json":
                        body = tlv("s", "{not json")
                    elif mode == "no_candidates":
                        body = tlv("s", '{"n_found":0,"candidates":[]}')
                    elif mode == "odd_candidates":
                        body = tlv("s", '{"n_found":3,"candidates":[{"sharpe":1},{"weights":[1]} , {"x":1}')
                    elif mode.startswith("status:"):
                        status = int(mode.split(":")[1])
                        body = b"" if status != 0 else tlv(kind, value)
                    else:
                        body = tlv(kind, value)
                elif op == OP_DEINIT:
                    body = b""
                rflags = 1 | (2 if status else 0)
                rop, rrid, rmagic, rver, ralen = op, rid, MAGIC, 2, 32
                rpayload = struct.pack("<Ii", status, 0) + body
                if mode == "bad_magic": rmagic = b"XXXX"
                elif mode == "bad_version": rver = 9
                elif mode == "no_response_flag": rflags = 0
                elif mode == "bad_authlen": ralen = 16
                elif mode == "wrong_rid": rrid = rid + 1000
                elif mode == "wrong_opcode": rop = 0x0001
                elif mode == "tiny_payload": rpayload = b"\x00\x00\x00\x00"
                rplen = len(rpayload)
                if mode == "huge_payload": rplen = 33 * 1024 * 1024
                rhdr = HDR.pack(rmagic, rver, rop, rflags, rrid, 0, ralen, rplen)
                tag = self._tag(rhdr, rpayload)
                if mode == "bad_tag":
                    tag = bytes(32)
                if mode == "partial_header":
                    c.sendall(rhdr[:10]); return
                if mode == "partial_tag":
                    c.sendall(rhdr + tag[:5]); return
                if mode == "huge_payload":
                    c.sendall(rhdr + tag); return
                if mode == "partial_payload":
                    c.sendall(rhdr + tag + rpayload[:max(1, len(rpayload) // 2)]); return
                if mode == "call_close" and op == OP_CALL:
                    return
                c.sendall(rhdr + tag + rpayload)
        except (EOFError, OSError):
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass
