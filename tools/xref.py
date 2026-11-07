# xref.py <exe> <rva hex>... : finds CALL/JMP rel32 to each RVA in the executable sections
import struct, sys
exe = sys.argv[1]; targets = [int(x, 16) for x in sys.argv[2:]]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
for i in range(nsec):
    s = pe + 24 + opt + i * 40
    vsz, va, rsz, raw, ch = struct.unpack_from('<I', d, s + 8)[0], struct.unpack_from('<I', d, s + 12)[0], struct.unpack_from('<I', d, s + 16)[0], struct.unpack_from('<I', d, s + 20)[0], struct.unpack_from('<I', d, s + 36)[0]
    if not ch & 0x20000000: continue
    sec = d[raw:raw + rsz]
    for t in targets:
        for op in (b'\xe8', b'\xe9'):
            p = sec.find(op)
            while p != -1:
                if p + 5 <= len(sec):
                    rel = struct.unpack_from('<i', sec, p + 1)[0]
                    if va + p + 5 + rel == t: print(f"{t:08X} <- {va + p:08X} {'call' if op == b'\xe8' else 'jmp'}")
                p = sec.find(op, p + 1)
