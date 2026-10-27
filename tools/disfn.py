"""A function's instructions as discovery sees them (for rewriting it as C, runtime/decomp/):
address, bytes' text, and for calls and jumps through an incremental-linking stub (0x401000-0x409000,
one jmp) the target it reaches. Uses gen/'s discovery cache when fresh (work/dis_cache.pkl).

    disfn.py ADDR [ADDR ...]
"""
import os, pickle, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from recomp import load_config
from image import Image, Module, Decoder

def main():
    cfg = load_config(os.path.join(os.path.dirname(__file__), '..', 'builds', 'etm12.py'))
    game = os.path.expanduser('~/.local/share/etm-native/game')
    mods = [Module(e[0], os.path.join(game, e[1]), e[3] if len(e) > 3 else None) for e in cfg.MODULES]
    pe = Image(mods)
    dec = Decoder(pe)
    cache = os.path.join(os.path.dirname(__file__), '..', 'work', 'dis_cache.pkl')
    funcs = None
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(
            os.path.join(os.path.dirname(__file__), '..', 'gen', 'funcs.h')):
        funcs = pickle.load(open(cache, 'rb'))
    if funcs is None:
        from discover import Discovery
        iat = {va: (d, n) for d, n, va in pe.imports}
        disc = Discovery(pe, dec, {va: n for va, (d, n) in iat.items()}, cfg.NORETURN_IMPORTS,
                         cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES)
        found = disc.run(pe.seeds(), pe.data_code_pointers())
        funcs = {e: sorted(f.insns) for e, f in found.items()}
        pickle.dump(funcs, open(cache, 'wb'))
    def stub_target(t):
        if 0x401000 <= t < 0x409000:
            i = dec.at(t)
            if i is not None and i.mn == 'jmp' and i.ops[0][0] == 'i':
                return i.ops[0][1]
        return None
    for a in sys.argv[1:]:
        e = int(a, 16)
        if e not in funcs:
            print('%08x: not a function start' % e)
            continue
        print('function %08x (%d instructions)' % (e, len(funcs[e])))
        for ia in funcs[e]:
            i = dec.at(ia)
            note = ''
            if i.mn in ('call', 'jmp') and i.ops and i.ops[0][0] == 'i':
                t = stub_target(i.ops[0][1])
                if t:
                    note = '   -> %08x' % t
            print('  %08x  %-40s%s' % (ia, i.text, note))

main()
