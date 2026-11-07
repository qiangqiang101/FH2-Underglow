# check_text.py <neon_text.h> <font dir> : every translation's characters the game's fonts for that language lack
# (run with python -X utf8). The font dir holds <font>.txt from vfont.py, for DFYuanMedium-B5, DFGHSGothic-W5,
# MalgunBdGm, verdana and Anthem_A (Mount\media\ui\Fonts.zip). No output: everything can be drawn.
import re, sys, os
src = open(sys.argv[1], encoding='utf-8').read()
F = lambda n: set(open(os.path.join(sys.argv[2], n + '.txt'), encoding='utf-8').read())
anthem = F('Anthem_A')
fonts = {'CHT': F('DFYuanMedium-B5') | anthem, 'JP': F('DFGHSGothic-W5') | anthem, 'KO': F('MalgunBdGm') | anthem,
         'RU': F('verdana') | anthem}
codes = ["BR", "DE", "FR", "ES", "MX", "IT", "JP", "KO", "CHT", "CZ", "HU", "PL", "RU", "NL"]
lit = re.compile(r'"((?:[^"\\]|\\.)*)"')
BS = chr(92)
body = src[src.index('kTexts[] = {'):]
bad = {}
for m in re.finditer(r'^\{(".*?)\}\},?\s*$', body, re.S | re.M):
    strs = [s.replace(BS + '"', '"').replace(BS + BS, BS) for s in lit.findall(m.group(1))]
    if len(strs) != 15:
        print('skip', strs[:1], len(strs))
        continue
    for code, t in zip(codes, strs[1:]):
        if code in ('CZ', 'HU'):
            continue
        have = fonts.get(code, anthem)
        miss = sorted(set(c for c in t if c not in have))
        if miss:
            bad.setdefault(code, []).append((strs[0][:30], t, ''.join(miss)))
for code, rows in bad.items():
    print(f'== {code}: {len(rows)}')
    for en, t, miss in rows:
        print(f'   {en!r}: {t}   missing [{miss}]')
