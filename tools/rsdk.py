#!/usr/bin/env python3
"""Read an RSDKv5 data pack (Sonic Mania's Data.rsdk).

Files are addressed by the MD5 of their lowercase path, so the container has no
name table. Recovering a file means knowing its name, which the decomp source
provides as string literals.

Container layout, from RSDKv5 Reader.cpp LoadDataPack:
    'RSDK' 'v' <version:u8> <fileCount:u16>
    per file: <md5:16> <offset:u32le> <size:u32le>   (size bit 31 = encrypted)
"""

import hashlib
import struct
import sys


class Pack:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        if self.data[:4] != b"RSDK":
            raise SystemExit(f"{path}: not an RSDK container")
        self.version = self.data[5]
        count = struct.unpack_from("<H", self.data, 6)[0]
        self.files = {}
        off = 8
        for _ in range(count):
            md5 = self.data[off:off + 16]
            offset, size = struct.unpack_from("<II", self.data, off + 16)
            self.files[md5] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))
            off += 24

    @staticmethod
    def key(name):
        # The table stores each 4-byte group of the digest big-endian, while
        # the engine compares them as native words, so the groups come out
        # byte-reversed relative to a plain MD5 digest.
        d = hashlib.md5(name.lower().encode()).digest()
        return b"".join(d[i:i + 4][::-1] for i in range(0, 16, 4))

    def lookup(self, name):
        return self.files.get(self.key(name))

    def read(self, name):
        entry = self.lookup(name)
        if entry is None:
            return None
        offset, size, encrypted = entry
        blob = self.data[offset:offset + size]
        return decrypt(blob, name, size) if encrypted else blob


# RSDKv5 file encryption, ported from Reader.cpp GenerateELoadKeys and
# DecryptBytes. Key A comes from the uppercased path, key B from the decimal
# file size, both with each 4-byte group of the digest reversed.
def _swizzle(digest):
    return b"".join(digest[i:i + 4][::-1] for i in range(0, 16, 4))


def decrypt(blob, name, size):
    keyA = _swizzle(hashlib.md5(name.upper().encode()).digest())
    keyB = _swizzle(hashlib.md5(str(size).encode()).digest())

    out = bytearray(blob)
    keyNo = (size // 4) & 0x7F
    posA, posB, swap = 0, 8, False

    for i in range(len(out)):
        c = out[i] ^ keyNo ^ keyB[posB]
        if swap:
            c = ((c << 4) + (c >> 4)) & 0xFF
        c ^= keyA[posA]
        out[i] = c

        posA += 1
        posB += 1

        if posA <= 15:
            if posB > 12:
                posB = 0
                swap = not swap
        elif posB <= 8:
            posA = 0
            swap = not swap
        else:
            keyNo = (keyNo + 2) & 0x7F
            if swap:
                swap = False
                posA = keyNo % 7
                posB = (keyNo % 12) + 2
            else:
                swap = True
                posA = (keyNo % 12) + 3
                posB = keyNo % 7

    return bytes(out)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: rsdk.py <Data.rsdk> [name ...]")
    pack = Pack(sys.argv[1])
    names = sys.argv[2:]

    if not names:
        total = sum(s for _, s, _ in pack.files.values())
        enc = sum(1 for _, _, e in pack.files.values() if e)
        print(f"version v{pack.version}")
        print(f"files    {len(pack.files)}")
        print(f"bytes    {total:,}")
        print(f"encrypted {enc}")
        return

    for name in names:
        entry = pack.lookup(name)
        if entry is None:
            print(f"{'-':>10}  {name}   NOT FOUND")
        else:
            offset, size, encrypted = entry
            print(f"{size:>10,}  {name}{'   [enc]' if encrypted else ''}")


if __name__ == "__main__":
    main()
