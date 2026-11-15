#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
"""List the files and the dependencies of an Alpine .apk built by fpm.

An .apk is a few gzip members back to back (control data, then the payload), each a tar stream. The tar
headers fpm writes carry a checksum that GNU tar and Python's tarfile reject, so the headers are walked
directly: a name, a size, the data padded to 512 bytes, and PAX records for long names.

  apk_list.py PACKAGE.apk            one path per line, then the `depend = ...` lines as `depend: name`
"""
import sys
import zlib


def members(data):
    while data:
        d = zlib.decompressobj(31)
        out = d.decompress(data)
        yield out
        data = d.unused_data


def entries(blob):
    pos, pax_path = 0, None
    while pos + 512 <= len(blob):
        hdr = blob[pos:pos + 512]
        if hdr == b"\0" * 512:
            break
        name = hdr[:100].split(b"\0", 1)[0].decode("utf-8", "replace")
        size = int(hdr[124:136].split(b"\0", 1)[0].strip() or b"0", 8)
        kind = hdr[156:157]
        body = blob[pos + 512:pos + 512 + size]
        pos += 512 + ((size + 511) // 512) * 512
        if kind in (b"x", b"g"):
            for rec in body.split(b"\n"):
                if b" path=" in rec:
                    pax_path = rec.split(b" path=", 1)[1].decode("utf-8", "replace")
            continue
        yield (pax_path or name), kind, body
        pax_path = None


def main(path):
    data = open(path, "rb").read()
    for blob in members(data):
        for name, kind, body in entries(blob):
            if kind == b"5":
                continue
            if name == ".PKGINFO":
                for line in body.decode("utf-8", "replace").splitlines():
                    if line.startswith("depend = "):
                        print("depend:", line[len("depend = "):])
                continue
            if name.startswith(".") and not name.startswith("./"):
                continue          # control files (.post-install, .pre-deinstall, ...)
            print(name.lstrip("./"))


if __name__ == "__main__":
    main(sys.argv[1])
