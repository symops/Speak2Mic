#!/usr/bin/env python3
"""pefix.py <file.sys> [--checksum-only]
Makes a MinGW-linked driver image look like a WDK one: clears IMAGE_FILE_DLL and
recomputes the PE checksum (the checksum field is excluded from the Authenticode hash,
so this may also run after signing)."""
import struct, sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
pe = struct.unpack_from('<I', data, 0x3C)[0]
assert data[pe:pe + 4] == b'PE\0\0'
if '--checksum-only' not in sys.argv:
    ch_off = pe + 4 + 18
    ch = struct.unpack_from('<H', data, ch_off)[0]
    struct.pack_into('<H', data, ch_off, ch & ~0x2000)       # IMAGE_FILE_DLL

opt = pe + 24
csum_off = opt + 64
struct.pack_into('<I', data, csum_off, 0)
s = 0
buf = data + (b'\0' if len(data) % 2 else b'')
for i in range(0, len(buf), 2):
    if i == csum_off or i == csum_off + 2:
        continue
    s += buf[i] | (buf[i + 1] << 8)
    s = (s & 0xFFFF) + (s >> 16)
s = (s & 0xFFFF) + (s >> 16)
struct.pack_into('<I', data, csum_off, (s & 0xFFFF) + len(data))
open(path, 'wb').write(data)
print(f'{path}: checksum 0x{(s & 0xFFFF) + len(data):08X}')
