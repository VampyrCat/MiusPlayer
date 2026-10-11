"""Are the game's operations fit for the shared operational map?   op_ready.py [game folder]

On the operational map the joining player plays the same operation from the other side: for a host in
"<name>_rus" his game opens "<name>_ger" on the host's state (and the other way round). That needs
  1. the other side's operation to exist (a .campack2 next to it), and
  2. both to describe the same formations and battle groups, by name - the host's state is loaded into
     the other operation, which must know every one of them.
This reads every operation file under data\\ (an AZP3 pack; its "div_pool" entry is the binary config with
the formations) and reports both points for each pair.
"""
import os
import struct
import sys

import azcf


def operations(root):
    out = {}
    data = os.path.join(root, 'data')
    for base, dirs, files in os.walk(data):
        if base[len(data):].count(os.sep) > 6:
            dirs[:] = []
            continue
        for f in files:
            if f.lower().endswith('.campack2'):
                campaign = os.path.basename(base)
                out[(campaign.lower(), f[:-9].lower())] = os.path.join(base, f)
    return out


def counterpart(name):
    if name.endswith('_rus'):
        return name[:-4] + '_ger'
    if name.endswith('_ger'):
        return name[:-4] + '_rus'
    return ''


def formations(path):
    """names of the structs of an operation's div_pool, and for those with a side: side and commander tag"""
    d = open(path, 'rb').read()
    entry = [e for e in azcf.pack_entries(d) if e[0] == 'div_pool']
    if not entry:
        return None, None
    _, _, off, size = entry[0]
    a = azcf.Azcf(d[off:off + size])
    names, tagged = set(), {}
    for i in range(a.count):
        n = a.node(i)
        if n['type'] != 1:
            continue
        names.add(n['name'])
        fields = {}
        for r in range(n['rows']):
            c = n['data'] + r * 0x40
            field = bytes(a.b[c + 0x20:c + 0x40]).split(b'\0')[0].decode('latin1')
            fields[field] = c
        if 'side' in fields:
            def code(c):
                ln, val, x, y, kind = struct.unpack_from('<5I', a.b, c)
                raw = bytes(a.b[c:c + 4])
                return a.cstr(val) if kind == 2 and val else raw.rstrip(b'\0').decode('latin1', 'replace')
            tagged[n['name']] = (code(fields['side']), code(fields['ai']) if 'ai' in fields else '')
    return names, tagged


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ops = operations(root)
    bad = 0
    print('%d operation files' % len(ops))
    for (campaign, name), path in sorted(ops.items()):
        other = counterpart(name)
        line = '%-14s %-24s' % (campaign, name)
        if not other or (campaign, other) not in ops:
            print(line + ' NO other-side operation: the joining player would get the host\'s own view')
            bad += 1
            continue
        mine, tags = formations(path)
        theirs, _ = formations(ops[(campaign, other)])
        if mine is None or theirs is None:
            print(line + ' formations could not be read')
            bad += 1
            continue
        only_mine, only_theirs = sorted(mine - theirs), sorted(theirs - mine)
        sides = {}
        for side, ai in tags.values():
            sides.setdefault(side, set()).add(ai)
        summary = '; '.join('%s led by %s' % (s, '/'.join(sorted(x for x in a if x) or ['-'])) for s, a in sorted(sides.items()))
        if only_mine or only_theirs:
            bad += 1
            print(line + ' DIFFERS from %s: %d names only here %s, %d only there %s' % (other, len(only_mine), only_mine[:4], len(only_theirs), only_theirs[:4]))
        else:
            print(line + ' ok: %s has the same %d structs (%d with a side: %s)' % (other, len(mine), len(tags), summary))
    print('operations not ready: %d' % bad)


if __name__ == '__main__':
    main()
