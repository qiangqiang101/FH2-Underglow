# vfont.py <Fonts.zip> <font name> <out.txt> : writes the characters a .vfont has glyphs for (28-byte records,
# the code point first, in ascending order from the space)
import zipfile, struct, sys
d = zipfile.ZipFile(sys.argv[1]).read(sys.argv[2])
i = 0
while True:
    i = d.find(struct.pack('<I', 0x20), i + 1)
    if struct.unpack_from('<I', d, i + 28)[0] == 0x21: break
cps, last = [], 0
while i + 4 <= len(d):
    c = struct.unpack_from('<I', d, i)[0]
    if c <= last or c > 0x10FFFF: break
    cps.append(c); last = c; i += 28
open(sys.argv[3], 'w', encoding='utf-8').write(''.join(map(chr, cps)))
print(f"{sys.argv[2]}: {len(cps)} glyphs, U+{cps[0]:04X}..U+{cps[-1]:04X}")
