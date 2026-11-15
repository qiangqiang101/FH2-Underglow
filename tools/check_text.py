# check_text.py <l10n dir> <font dir> : every translation's characters the game's fonts for that language lack
# (run with python -X utf8). The font dir holds <font>.txt from vfont.py, for DFYuanMedium-B5, DFGHSGothic-W5,
# MalgunBdGm, verdana and Anthem_A (Mount\media\ui\Fonts.zip). No output: everything can be drawn.
import glob, os, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'platform', 'tools', 'l10n'))
import pofile  # noqa: E402

F = lambda n: set(open(os.path.join(sys.argv[2], n + '.txt'), encoding='utf-8').read())
anthem = F('Anthem_A')
fonts = {'zh_Hant': F('DFYuanMedium-B5') | anthem, 'ja': F('DFGHSGothic-W5') | anthem, 'ko': F('MalgunBdGm') | anthem,
         'ru': F('verdana') | anthem}
bad = {}
for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.po'))):
    lang = os.path.splitext(os.path.basename(path))[0]
    have = fonts.get(lang, anthem)
    for e in pofile.parse(open(path, encoding='utf-8').read(), path):
        if e.is_header() or 'fuzzy' in e.flags:
            continue
        for t in e.forms():
            miss = sorted(set(c for c in t if c not in have and c not in '\n\t'))
            if miss:
                bad.setdefault(lang, []).append((e.ctxt or e.msgid[:30], t, ''.join(miss)))
for lang, rows in bad.items():
    print(f'== {lang}: {len(rows)}')
    for key, t, miss in rows:
        print(f'   {key}: {t}   missing [{miss}]')
