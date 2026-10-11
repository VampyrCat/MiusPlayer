"""Makes the battle description a joining player needs to command the host's opponent.
   mirror_image.py <in.cfgpack> <out.cfgpack> [parts]

Each squad row carries an owner tag, the four-character code 'USER' or 'ENEM'; that tag, not the unit ids
or the pool order, is what the game uses to decide who commands a platoon. Unit ids and row order are left
alone, so both games name and number every object identically.

parts (default "sqp"):
  s   gamespec: user_side <-> enemy_side          (whose units are "ours" for display and visibility)
  q   squads:   owner tag 'USER' <-> 'ENEM'
  p   pools:    the two pools exchange label and command-chain name
  l   plats:    every platoon takes the role columns of a platoon from the other force
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from azcf import Azcf, CELL

USER, ENEM = struct.unpack('<I', b'USER')[0], struct.unpack('<I', b'ENEM')[0]


def mirror(blob, parts='sqp'):
    c = Azcf(blob)

    def cell(node, row, col):
        o = node['data'] + (row * node['cols'] + col) * CELL
        return o, bytes(c.b[o:o + CELL])

    if 's' in parts:
        g = c.find('i_gamespec:gamespec')
        pos = {}
        for r in range(g['rows']):
            o = g['data'] + r * 0x40
            name = c.b[o + 0x20:o + 0x40].split(b'\0')[0].decode()
            if name in ('user_side', 'enemy_side'):
                pos[name] = o
        u, e = struct.unpack_from('<I', c.b, pos['user_side'])[0], struct.unpack_from('<I', c.b, pos['enemy_side'])[0]
        struct.pack_into('<I', c.b, pos['user_side'], e)
        struct.pack_into('<I', c.b, pos['enemy_side'], u)

    if 'q' in parts:
        squads = c.find('squads')
        swapped = 0
        for r in range(squads['rows']):
            for col in range(squads['cols']):
                o, raw = cell(squads, r, col)
                value, kind = struct.unpack_from('<I', raw, 0)[0], struct.unpack_from('<I', raw, 16)[0]
                if kind == 4 and value in (USER, ENEM):
                    struct.pack_into('<I', c.b, o, ENEM if value == USER else USER)
                    swapped += 1
        assert swapped == squads['rows'], 'expected one owner tag per squad'

    if 'p' in parts:
        pools = c.find('pools')
        if pools['rows'] == 2:
            for col in (0, 2):
                (o0, x), (o1, y) = cell(pools, 0, col), cell(pools, 1, col)
                c.b[o0:o0 + CELL] = y
                c.b[o1:o1 + CELL] = x

    if 'l' in parts:
        plats = c.find('plats')
        pools = c.find('pools')
        user_pool = [struct.unpack_from('<I', cell(pools, r, 1)[1], 0)[0] for r in range(pools['rows'])
                     if c.cell(cell(pools, r, 0)[0]) == 'user'][0]
        rows = range(plats['rows'])
        ids = [struct.unpack_from('<I', cell(plats, r, 1)[1], 0)[0] for r in rows]
        mine = [r for r in rows if (ids[r] & 0xff000000) == user_pool]
        theirs = [r for r in rows if r not in mine]
        role_cols = (0, 3, 4, 5, 7, 11)       # name, command chain, controller, colour/role/index, deployment
        old = {(r, col): cell(plats, r, col)[1] for r in rows for col in role_cols}
        for group, other in ((mine, theirs), (theirs, mine)):
            for k, r in enumerate(group):
                src = other[min(k, len(other) - 1)]
                for col in role_cols:
                    o, _ = cell(plats, r, col)
                    c.b[o:o + CELL] = old[(src, col)]
    return bytes(c.b)


if __name__ == '__main__':
    parts = sys.argv[3] if len(sys.argv) > 3 else 'sqp'
    out = mirror(open(sys.argv[1], 'rb').read(), parts)
    open(sys.argv[2], 'wb').write(out)
    print('mirrored parts:', parts)
