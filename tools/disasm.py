# dis.py <exe> <rva hex> [count]  : disassembles game code at an RVA (image base 0x140000000)
import struct, sys, capstone
exe, rva, n = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 80
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
off = None
for i in range(nsec):
    s = pe + 24 + opt + i * 40
    va, vsz, raw, rsz = struct.unpack_from('<IIII', d, s + 8)[0], *struct.unpack_from('<III', d, s + 8 + 4)
    va, vsz = struct.unpack_from('<II', d, s + 12)[0], struct.unpack_from('<I', d, s + 8)[0]
    raw = struct.unpack_from('<I', d, s + 20)[0]
    if va <= rva < va + vsz: off = raw + rva - va
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for k, ins in enumerate(md.disasm(d[off:off + n * 16], 0x140000000 + rva)):
    if k >= n: break
    print(f"{ins.address - 0x140000000:08X}  {ins.mnemonic:8} {ins.op_str}")
