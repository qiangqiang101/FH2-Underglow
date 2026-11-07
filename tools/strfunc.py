# strfunc.py <exe> <string|0xRVA>... : for each string, the functions that load its address (lea reg, [rip+x]),
# found through the exception directory (.pdata), as RVAs
import bisect, re, struct, sys
exe = sys.argv[1]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = []
for i in range(nsec):
    s = pe + 24 + opt + i * 40
    vsz, va, rsz, raw = struct.unpack_from('<IIII', d, s + 8)
    secs.append((va, vsz, raw, rsz, struct.unpack_from('<I', d, s + 36)[0]))
def r2f(r):
    for va, vsz, raw, rsz, ch in secs:
        if va <= r < va + rsz: return raw + r - va
def f2r(f):
    for va, vsz, raw, rsz, ch in secs:
        if raw <= f < raw + rsz: return va + f - raw
exc_rva, exc_size = struct.unpack_from('<II', d, pe + 24 + 112 + 3 * 8)   # data directory 3: exceptions
o = r2f(exc_rva)
funcs = sorted(struct.unpack_from('<II', d, o + k * 12) for k in range(exc_size // 12))
starts = [f[0] for f in funcs]
def func_of(r):
    i = bisect.bisect_right(starts, r) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
# every lea r64, [rip+disp32] (REX.W 8D, modrm 00 reg 101), by target
leas = {}
for va, vsz, raw, rsz, ch in secs:
    if not ch & 0x20000000: continue
    sec = d[raw:raw + rsz]
    for m in re.finditer(rb'[\x48\x4C]\x8D', sec):
        q = m.start()
        if q + 7 > len(sec) or (sec[q + 2] & 0xC7) != 0x05: continue
        t = va + q + 7 + struct.unpack_from('<i', sec, q + 3)[0]
        leas.setdefault(t, []).append(va + q)
for name in sys.argv[2:]:
    hits = []
    if name.startswith('0x'):   # an address (RVA) instead of a string
        hits.append(int(name, 16))
    else:
        pat = name.encode() + b'\0'
        p = d.find(pat)
        while p != -1:
            if p == 0 or d[p - 1] == 0: hits.append(f2r(p))
            p = d.find(pat, p + 1)
    print(name, 'string at', [f'{h:X}' for h in hits])
    for h in hits:
        for at in leas.get(h, []):
            f = func_of(at)
            print(f'   function {f[0]:08X}' if f else '   function ?', f'(lea at {at:08X})')
