# vt.py <exe> <rva of a slot> : prints the vtable containing that slot, with its RTTI name
import struct, sys
d = open(sys.argv[1], 'rb').read(); slot = int(sys.argv[2], 16); B = 0x140000000
pe = struct.unpack_from('<I', d, 0x3C)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + i * 40 + 8) for i in range(nsec)]
def r2f(r):
    for vsz, va, rsz, raw in secs:
        if va <= r < va + max(vsz, rsz): return raw + r - va
def q(r): return struct.unpack_from('<Q', d, r2f(r))[0]
def text(v): return B + 0x1000 <= v < B + 0x1db4000
s = slot
while text(q(s - 8)): s -= 8
col = q(s - 8) - B
try:
    td = struct.unpack_from('<I', d, r2f(col + 12))[0]
    name = d[r2f(td + 16):r2f(td + 16) + 120].split(b'\0')[0].decode()
except Exception: name = '?'
print(f"vtable at {s:X}: {name}")
i = 0
while text(q(s + i * 8)):
    print(f"  slot {i:3d} (+0x{i*8:03X}) {q(s + i*8) - B:08X}{'  <==' if s + i*8 == slot else ''}"); i += 1
