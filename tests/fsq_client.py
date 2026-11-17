#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""A small FSQ protocol v2 client, used to test any fractalsqld."""
import hashlib
import hmac
import socket
import struct

OP_PING, OP_VERSION, OP_INIT, OP_CALL, OP_DEINIT, OP_CANCEL, OP_RELOAD = 1, 2, 0x10, 0x11, 0x12, 0x13, 0x14
OK, ARGS, LIMIT, CORE, INTERNAL, NOTSUP, BUSY = 0, 1, 2, 3, 6, 7, 8
T_STRING, T_INT, T_REAL = 1, 2, 3


def load_key(path):
    return bytes.fromhex("".join(open(path).read().split()))


class Reply:
    def __init__(self, status, core_rc, body):
        self.status, self.core_rc, self.body = status, core_rc, body

    def text(self):
        return self.body.decode("utf-8", "replace")


class Client:
    def __init__(self, sock_path, key, timeout=30):
        self.key = key
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(sock_path)
        self.rid = 0

    def close(self):
        self.sock.close()

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    def request(self, opcode, payload=b"", context=0, bad_tag=False):
        self.rid += 1
        hdr = b"FSQ1" + struct.pack("<HHIIQII", 2, opcode, 0, self.rid, context, 32, len(payload))
        tag = hmac.new(self.key, hdr + payload, hashlib.sha256).digest()
        if bad_tag:
            tag = bytes(32)
        self.sock.sendall(hdr + tag + payload)
        rh = self._recv(32)
        self._recv(32)
        plen = struct.unpack_from("<I", rh, 28)[0]
        body = self._recv(plen)
        status, rc = struct.unpack_from("<Ii", body, 0)
        return Reply(status, rc, body[8:])

    @staticmethod
    def arg(v):
        if v is None:
            return struct.pack("<BBI", T_STRING, 1, 0)
        if isinstance(v, int):
            return struct.pack("<BBI", T_INT, 0, 8) + struct.pack("<q", v)
        if isinstance(v, float):
            return struct.pack("<BBI", T_REAL, 0, 8) + struct.pack("<d", v)
        b = v.encode() if isinstance(v, str) else v
        return struct.pack("<BBI", T_STRING, 0, len(b)) + b

    def init(self, name, *args):
        n = name.encode()
        r = self.request(OP_INIT, bytes([len(n)]) + n + struct.pack("<I", len(args)) + b"".join(self.arg(a) for a in args))
        if r.status != OK:
            return None, r
        handle = struct.unpack_from("<Q", r.body, 0)[0]
        return handle, r

    def call_handle(self, name, handle, *args, token=0):
        n = name.encode()
        return self.request(OP_CALL, bytes([len(n)]) + n + struct.pack("<QI", handle, len(args))
                            + b"".join(self.arg(a) for a in args), context=token)

    def deinit(self, handle):
        return self.request(OP_DEINIT, struct.pack("<Q", handle))

    def cancel(self, handle):
        return self.request(OP_CANCEL, struct.pack("<Q", handle))

    def call(self, name, *args, token=0):
        """init, call, deinit. Returns (status, value-or-text)."""
        handle, r = self.init(name, *args)
        if handle is None:
            return r.status, r.text()
        try:
            c = self.call_handle(name, handle, *args, token=token)
        finally:
            self.deinit(handle)
        if c.status != OK:
            return c.status, c.text()
        t, is_null, ln = struct.unpack_from("<BBI", c.body, 0)
        raw = c.body[6:6 + ln]
        if is_null:
            return OK, None
        if t == T_INT:
            return OK, struct.unpack("<q", raw)[0]
        if t == T_REAL:
            return OK, struct.unpack("<d", raw)[0]
        return OK, raw.decode("utf-8", "replace")
