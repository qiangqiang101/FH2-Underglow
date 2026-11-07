# scan.py <exe> <start rva> <end rva> <regex> : disassembles a range linearly and prints matching instructions
import struct, sys, re, capstone
exe, a, b, pat = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), re.compile(sys.argv[4])
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
for i in range(nsec):
    s = pe + 24 + opt + i * 40
    vsz, va, raw = struct.unpack_from('<I', d, s + 8)[0], struct.unpack_from('<I', d, s + 12)[0], struct.unpack_from('<I', d, s + 20)[0]
    if va <= a < va + vsz: off = raw + a - va
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.skipdata = True
for ins in md.disasm(d[off:off + (b - a)], a):
    t = f"{ins.mnemonic} {ins.op_str}"
    if pat.search(t): print(f"{ins.address:08X}  {t}")
